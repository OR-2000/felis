#include <sys/mman.h>
#include <algorithm>
#include <map>
#include <sys/time.h>
#include <fstream>

#include "epoch.h"
#include "txn.h"
#include "log.h"
#include "vhandle.h"
#include "vhandle_batchappender.h"
#include "console.h"
#include "mem.h"
#include "gc.h"
#include "opts.h"

#include "literals.h"

#include "json11/json11.hpp"

namespace felis {

EpochClient *EpochClient::g_workload_client = nullptr;
bool EpochClient::g_enable_granola = false;

void EpochCallback::operator()(unsigned long cnt)
{
  auto p = static_cast<int>(phase);

  trace(TRACE_COMPLETION "callback cnt {} on core {}",
        cnt, go::Scheduler::CurrentThreadPoolId() - 1);

  if (cnt == 0) {
    perf.End();
    perf.Show(label);
    printf("\n");

    if (phase == EpochPhase::Initialize)
      logger->info("Callback handler on core {}", go::Scheduler::CurrentThreadPoolId() - 1);
    // TODO: We might Reset() the PromiseAllocationService, which would free the
    // current go::Routine. Is it necessary to run some function in the another
    // go::Routine?

    static void (EpochClient::*phase_mem_funcs[])() = {
      &EpochClient::OnInsertComplete,
      &EpochClient::OnInitializeComplete,
      &EpochClient::OnExecuteComplete,
    };
    abort_if(go::Scheduler::Current()->current_routine() == &client->control,
             "Cannot call control thread from itself");
    if (Options::kVHandleBatchAppend)
      util::Instance<BatchAppender>().Reset();

    client->control.Reset(phase_mem_funcs[p]);
    go::Scheduler::Current()->WakeUp(&client->control);
  }
}

void EpochCallback::PreComplete()
{
  if (Options::kVHandleBatchAppend) {
    if (phase == EpochPhase::Initialize || phase == EpochPhase::Insert) {
      util::Instance<BatchAppender>().FinalizeFlush(
          util::Instance<EpochManager>().current_epoch_nr());
    }
  }
}

EpochClient::EpochClient()
    : control(this),
      callback(EpochCallback(this)),
      completion(0, callback),
      disable_load_balance(false),
      conf(util::Instance<NodeConfiguration>())
{
  callback.perf.End();

  best_core = std::numeric_limits<int>::max();
  best_duration = std::numeric_limits<int>::max();
  core_limit = conf.g_nr_threads;

  auto cnt_len = conf.nr_nodes() * conf.nr_nodes() * NodeConfiguration::kPromiseMaxLevels;
  unsigned long *cnt_mem = nullptr;
  EpochWorkers *workers_mem = nullptr;

  for (int t = 0; t < NodeConfiguration::g_nr_threads; t++) {
    auto d = std::div(t + NodeConfiguration::g_core_shifting, mem::kNrCorePerNode);
    auto numa_node = d.quot;
    auto numa_offset = d.rem;
    if (numa_offset == 0) {
      cnt_mem = (unsigned long *) mem::MemMapAlloc(
          mem::Epoch,
          cnt_len * sizeof(unsigned long) * mem::kNrCorePerNode,
          numa_node);
      workers_mem = (EpochWorkers *) mem::MemMapAlloc(
          mem::Epoch,
          sizeof(EpochWorkers) * mem::kNrCorePerNode,
          numa_node);
    }
    per_core_cnts[t] = cnt_mem + cnt_len * numa_offset;
    workers[t] = new (workers_mem + numa_offset) EpochWorkers(t, this);
  }
}

EpochTxnSet::EpochTxnSet()
{
  auto nr_threads = NodeConfiguration::g_nr_threads;
  auto d = std::div((int) EpochClient::kTxnPerEpoch, nr_threads);
  for (auto t = 0; t < nr_threads; t++) {
    size_t nr = d.quot;
    if (t < d.rem) nr++;
    auto numa_node = (t + NodeConfiguration::g_core_shifting) / mem::kNrCorePerNode;
    auto p = mem::MemMapAlloc(mem::Txn, (nr + 1) * sizeof(BaseTxn *), numa_node);
    per_core_txns[t] = new (p) TxnSet(nr);
  }
}

EpochTxnSet::~EpochTxnSet()
{
  // TODO: free these pointers via munmap().
}

void EpochClient::GenerateBenchmarks()
{
  all_txns = new EpochTxnSet[g_max_epoch - 1];
  for (auto i = 1; i < g_max_epoch; i++) {
    for (uint64_t j = 1; j <= NumberOfTxns(); j++) {
      auto d = std::div((int)(j - 1), NodeConfiguration::g_nr_threads);
      auto t = d.rem, pos = d.quot;
      BaseTxn::g_cur_numa_node = t / mem::kNrCorePerNode;
      all_txns[i - 1].per_core_txns[t]->txns[pos] = CreateTxn(GenerateSerialId(i, j));
    }
  }
}

void EpochClient::Start()
{
  // Ready to start!
  control.Reset(&EpochClient::InitializeEpoch);

  logger->info("load percentage {}%", LoadPercentage());

  perf = PerfLog();
  go::GetSchedulerFromPool(0)->WakeUp(&control);
}

uint64_t EpochClient::GenerateSerialId(uint64_t epoch_nr, uint64_t sequence)
{
  return (epoch_nr << 32)
      | (sequence << 8)
      | (conf.node_id() & 0x00FF);
}

void AllocStateTxnWorker::Run()
{
  for (auto i = 0; i < client->cur_txns.load()->per_core_txns[t]->nr; i++) {
    auto txn = client->cur_txns.load()->per_core_txns[t]->txns[i];
    txn->PrepareState();
  }
}

void CallTxnsWorker::Run()
{
  auto nr_nodes = client->conf.nr_nodes();
  auto cnt = client->per_core_cnts[t];
  auto cnt_len = nr_nodes * nr_nodes * NodeConfiguration::kPromiseMaxLevels;
  std::fill(cnt, cnt + cnt_len, 0);

  set_urgent(true);
  util::Instance<NodeConfiguration>().ContinueInboundPhase();
  auto pq = client->cur_txns.load()->per_core_txns[t];

  for (auto i = 0; i < pq->nr; i++) {
    auto txn = pq->txns[i];
    txn->ResetRoot();
    std::invoke(mem_func, txn);
    client->conf.CollectBufferPlan(txn->root_promise(), cnt);
  }

  bool node_finished = client->conf.FlushBufferPlan(client->per_core_cnts[t]);

  // Try to assign a default partition scheme if nothing has been
  // assigned. Because transactions are already round-robinned, there is no
  // imbalanced here.

  long extra_offset = 0;
  for (auto i = client->core_limit; i < t; i++) {
    extra_offset += client->cur_txns.load()->per_core_txns[i]->nr;
  }
  extra_offset %= client->core_limit;

  for (auto i = 0; i < pq->nr; i++) {
    auto txn = pq->txns[i];
    auto aff = t;

    if (client->callback.phase == EpochPhase::Execute
        && t >= client->core_limit) {
      // auto avail_nr_zones = client->core_limit / mem::kNrCorePerNode;
      // auto zone = t % avail_nr_zones;
      aff = (i + extra_offset) % client->core_limit;
    }
    txn->root_promise()->AssignAffinity(aff);
    txn->root_promise()->Complete(VarStr());
  }
  set_urgent(false);

  util::Impl<PromiseRoutineTransportService>().FinishPromiseFromQueue(nullptr);

  // Granola doesn't support out of order scheduling. In the original paper,
  // Granola uses a single thread to issue. We use multiple threads, so here we
  // have to barrier.
  if (EpochClient::g_enable_granola && client->callback.phase == EpochPhase::Execute) {
    g_finished.fetch_add(1);

    while (g_finished.load() != NodeConfiguration::g_nr_threads)
      _mm_pause();
  }

  if ((util::Instance<EpochManager>().current_epoch_nr() & 0x07) == 0) {
    if (client->callback.phase == EpochPhase::Execute) {
      util::Instance<GC>().FinalizeGC();

      VHandle::Quiescence();
      RowEntity::Quiescence();

      mem::GetDataRegion().Quiescence();
    } else if (client->callback.phase == EpochPhase::Initialize) {
      util::Instance<GC>().RunGC();
    } else if (client->callback.phase == EpochPhase::Insert) {
      util::Instance<GC>().PrepareGC();
    }
  }

  client->completion.Complete();
  if (node_finished) {
    client->completion.Complete();
  }
}

void EpochClient::CallTxns(uint64_t epoch_nr, TxnMemberFunc func, const char *label)
{
  auto nr_threads = NodeConfiguration::g_nr_threads;
  conf.ResetBufferPlan();
  conf.SendStartPhase();
  callback.label = label;
  callback.perf.Clear();
  callback.perf.Start();

  // When another node sends its counter to us, it will not send pieces
  // first. This makes it hard to esitmate when we are about to finish all in a
  // phase. Therefore, we pretend all other nodes are going to send the maximum
  // pieces in this phase, and adjust this value when the counter arrives
  // eventually.

  if (EpochClient::g_enable_granola && callback.phase == EpochPhase::Execute)
    CallTxnsWorker::g_finished = 0;

  completion.Increment((conf.nr_nodes() - 1) * kMaxPiecesPerPhase + 1 + nr_threads);
  for (auto t = 0; t < nr_threads; t++) {
    auto r = &workers[t]->call_worker;
    r->Reset();
    r->set_function(func);
    go::GetSchedulerFromPool(t + 1)->WakeUp(r);
  }
}

void EpochClient::InitializeEpoch()
{
  auto &mgr = util::Instance<EpochManager>();
  mgr.DoAdvance(this);
  auto epoch_nr = mgr.current_epoch_nr();

  util::Impl<PromiseAllocationService>().Reset();
  util::Impl<PromiseRoutineDispatchService>().Reset();

  auto nr_threads = NodeConfiguration::g_nr_threads;

  disable_load_balance = true;
  cur_txns = &all_txns[epoch_nr - 1];
  total_nr_txn = NumberOfTxns();

  logger->info("Using EpochTxnSet {}", (void *) &all_txns[epoch_nr - 1]);

  for (auto t = 0; t < nr_threads; t++) {
    auto r = &workers[t]->alloc_state_worker;
    r->Reset();
    r->set_urgent(true);
    go::GetSchedulerFromPool(t + 1)->WakeUp(r);
  }

  callback.phase = EpochPhase::Insert;
  CallTxns(epoch_nr, &BaseTxn::PrepareInsert, "Insert");
}

void EpochClient::OnInsertComplete()
{
  stats.insert_time_ms += callback.perf.duration_ms();
  callback.phase = EpochPhase::Initialize;
  CallTxns(
      util::Instance<EpochManager>().current_epoch_nr(),
      &BaseTxn::Prepare,
      "Initialization");
}

void EpochClient::OnInitializeComplete()
{
  stats.initialize_time_ms += callback.perf.duration_ms();
  callback.phase = EpochPhase::Execute;

  if (NodeConfiguration::g_data_migration && util::Instance<EpochManager>().current_epoch_nr() == 1) {
    logger->info("Starting data scanner thread");
    auto &peer = util::Instance<felis::NodeConfiguration>().config().row_shipper_peer;
    go::GetSchedulerFromPool(NodeConfiguration::g_nr_threads + 1)->WakeUp(
      new felis::RowScannerRoutine());
  }

  util::Impl<VHandleSyncService>().ClearWaitCountStats();

  auto &mgr = util::Instance<EpochManager>();

  CallTxns(
      util::Instance<EpochManager>().current_epoch_nr(),
      &BaseTxn::RunAndAssignSchedulingKey,
      "Execution");
}

void EpochClient::OnExecuteComplete()
{
  stats.execution_time_ms += callback.perf.duration_ms();
  fmt::memory_buffer buf;
  long ctt = 0;
  auto cur_epoch_nr = util::Instance<EpochManager>().current_epoch_nr();
  for (int i = 0; i < NodeConfiguration::g_nr_threads; i++) {
    auto c = util::Impl<VHandleSyncService>().GetWaitCountStat(i);
    ctt += c / core_limit;
    fmt::format_to(buf, "{} ", c);
  }
  logger->info("Wait Counts {}", std::string_view(buf.begin(), buf.size()));
  if (Options::kCongestionControl && cur_epoch_nr > 1) {
    auto ctt_rate = (callback.perf.duration_ms() << 24) / ctt;
    logger->info("duration {} vs last_duration {}, ctt_rate {}",
                 callback.perf.duration_ms(), best_duration, ctt_rate);
    if (best_core == std::numeric_limits<int>::max() && ctt_rate > 1000) {
      best_core = core_limit;
    }

    if (core_limit < best_core) {
      if (callback.perf.duration_ms() * 1.05 < best_duration) {
        best_duration = callback.perf.duration_ms();
        best_core = core_limit;
        sample_count = 1;
      }

      if (callback.perf.duration_ms() / 1.15 > best_duration) {
        sample_count = 1;
      }

      if (--sample_count == 0) {
        core_limit -= 8;
        sample_count = 3;
      }
      if (core_limit == 0)
        core_limit = best_core;
    }
    logger->info("Contention Control new core_limit {}", core_limit);
  }

  if (cur_epoch_nr + 1 < g_max_epoch) {
    InitializeEpoch();
  } else {
    // End of the experiment.
    perf.Show("All epochs done in");
    auto thr = NumberOfTxns() * 1000 * (g_max_epoch - 1) / perf.duration_ms();
    logger->info("Throughput {} txn/s", thr);
    logger->info("Insert / Initialize / Execute {} ms {} ms {} ms",
                 stats.insert_time_ms, stats.initialize_time_ms, stats.execution_time_ms);
    mem::PrintMemStats();
    mem::GetDataRegion().PrintUsageEachClass();

    if (Options::kOutputDir) {
      json11::Json::object result {
        {"cpu", static_cast<int>(NodeConfiguration::g_nr_threads)},
        {"duration", static_cast<int>(perf.duration_ms())},
        {"throughput", static_cast<int>(thr)},
        {"insert_time", stats.insert_time_ms},
        {"initialize_time", stats.initialize_time_ms},
        {"execution_time", stats.execution_time_ms},
      };
      auto node_name = util::Instance<NodeConfiguration>().config().name;
      time_t tm;
      char now[80];
      time(&tm);
      strftime(now, 80, "-%F-%X", localtime(&tm));
      std::ofstream result_output(
          Options::kOutputDir.Get() + "/" + node_name + now + ".json");
      result_output << json11::Json(result).dump() << std::endl;
    }
    conf.CloseAndShutdown();
    util::Instance<Console>().UpdateServerStatus(Console::ServerStatus::Exiting);
  }
}

size_t EpochExecutionDispatchService::g_max_item = 20_M;
const size_t EpochExecutionDispatchService::kHashTableSize = 100001;

EpochExecutionDispatchService::EpochExecutionDispatchService()
{
  auto max_item_percore = g_max_item / NodeConfiguration::g_nr_threads;
  Queue *qmem = nullptr;

  for (int i = 0; i < NodeConfiguration::g_nr_threads; i++) {
    auto &queue = queues[i];
    auto d = std::div(i + NodeConfiguration::g_core_shifting, mem::kNrCorePerNode);
    auto numa_node = d.quot;
    auto offset_in_node = d.rem;

    if (offset_in_node == 0) {
      qmem = (Queue *) mem::MemMapAlloc(
          mem::EpochQueuePool, sizeof(Queue) * mem::kNrCorePerNode, numa_node);
    }
    queue = qmem + offset_in_node;

    queue->zq.end = queue->zq.start = 0;
    queue->zq.q = (PromiseRoutineWithInput *)
                 mem::MemMapAlloc(
                     mem::EpochQueuePromise,
                     max_item_percore * sizeof(PromiseRoutineWithInput),
                     numa_node);
    queue->pq.len = 0;
    queue->pq.q = (PriorityQueueHeapEntry *)
                 mem::MemMapAlloc(
                     mem::EpochQueueItem,
                     max_item_percore * sizeof(PriorityQueueHeapEntry),
                     numa_node);
    queue->pq.ht = (PriorityQueueHashHeader *)
                  mem::MemMapAlloc(
                      mem::EpochQueueItem,
                      kHashTableSize * sizeof(PriorityQueueHashHeader),
                      numa_node);
    queue->pq.pending.q = (PromiseRoutineWithInput *)
                         mem::MemMapAlloc(
                             mem::EpochQueuePromise,
                             max_item_percore * sizeof(PromiseRoutineWithInput),
                             numa_node);
    queue->pq.pending.start = 0;
    queue->pq.pending.end = 0;

    for (size_t t = 0; t < kHashTableSize; t++) {
      queue->pq.ht[t].Initialize();
    }

    queue->pq.pool = mem::BasicPool(
        mem::EpochQueuePool,
        kPriorityQueuePoolElementSize,
        max_item_percore,
        numa_node);

    queue->pq.pool.Register();

    new (&queue->lock) util::SpinLock();
  }
  tot_bubbles = 0;
}

void EpochExecutionDispatchService::Reset()
{
  for (int i = 0; i < NodeConfiguration::g_nr_threads; i++) {
    auto &q = queues[i];
    q->zq.end.store(0);
    q->zq.start.store(0);
    // q->pq.len = 0;
  }
  tot_bubbles = 0;
}

static bool Greater(const EpochExecutionDispatchService::PriorityQueueHeapEntry &a,
                    const EpochExecutionDispatchService::PriorityQueueHeapEntry &b)
{
  return a.key > b.key;
}

void EpochExecutionDispatchService::Add(int core_id, PromiseRoutineWithInput *routines,
                                        size_t nr_routines)
{
  bool locked = false;
  bool should_preempt = false;
  auto &lock = queues[core_id]->lock;
  lock.Lock();

  auto &zq = queues[core_id]->zq;
  auto &pq = queues[core_id]->pq.pending;
  size_t i = 0;

  auto max_item_percore = g_max_item / NodeConfiguration::g_nr_threads;

again:
  size_t zdelta = 0,
           zend = zq.end.load(std::memory_order_acquire),
         zlimit = max_item_percore;

  size_t pdelta = 0,
           pend = pq.end.load(std::memory_order_acquire),
         plimit = max_item_percore
                  - (pend - pq.start.load(std::memory_order_acquire));

  for (; i < nr_routines; i++) {
    auto r = routines[i];
    auto key = std::get<0>(r)->sched_key;

    if (key == 0) {
      auto pos = zend + zdelta++;
      abort_if(pos >= zlimit,
               "Preallocation of DispatchService is too small. {} < {}", pos, zlimit);
      zq.q[pos] = r;
    } else {
      if (pdelta >= plimit) goto again;
      auto pos = pend + pdelta++;
      pq.q[pos % max_item_percore] = r;
    }
  }
  if (zdelta)
    zq.end.fetch_add(zdelta, std::memory_order_release);
  if (pdelta)
    pq.end.fetch_add(pdelta, std::memory_order_release);
  lock.Unlock();
  // util::Impl<VHandleSyncService>().Notify(1 << core_id);
}

bool
EpochExecutionDispatchService::AddToPriorityQueue(
    PriorityQueue &q, PromiseRoutineWithInput &r,
    BasePromise::ExecutionRoutine *state)
{
  bool smaller = false;
  auto [rt, in] = r;
  auto node = (PriorityQueueValue *) q.pool.Alloc();
  node->Initialize();
  node->promise_routine = r;
  node->state = state;
  auto key = rt->sched_key;

  auto &hl = q.ht[Hash(key) % kHashTableSize];
  auto *ent = hl.next;
  while (ent != &hl) {
    if (ent->object()->key == key) {
      goto found;
    }
    ent = ent->next;
  }

  ent = (PriorityQueueHashEntry *) q.pool.Alloc();
  ent->Initialize();
  ent->object()->key = key;
  ent->object()->values.Initialize();
  ent->InsertAfter(hl.prev);

  if (q.len > 0 && q.q[0].key > key) {
    smaller = true;
  }
  q.q[q.len++] = {key, ent->object()};
  std::push_heap(q.q, q.q + q.len, Greater);

found:
  node->InsertAfter(ent->object()->values.prev);
  return smaller;
}

void
EpochExecutionDispatchService::ProcessPending(PriorityQueue &q)
{
  size_t pstart = q.pending.start.load(std::memory_order_acquire);
  long plen = q.pending.end.load(std::memory_order_acquire) - pstart;

  for (size_t i = 0; i < plen; i++) {
    auto pos = pstart + i;
    AddToPriorityQueue(q, q.pending.q[pos % (g_max_item / NodeConfiguration::g_nr_threads)]);
  }
  if (plen)
    q.pending.start.fetch_add(plen);
}

static long __SystemTime()
{
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool
EpochExecutionDispatchService::Peek(int core_id, DispatchPeekListener &should_pop)
{
  auto &zq = queues[core_id]->zq;
  auto &q = queues[core_id]->pq;
  auto &lock = queues[core_id]->lock;
  auto &state = queues[core_id]->state;
  auto zstart = zq.start.load(std::memory_order_acquire);
  if (zstart < zq.end.load(std::memory_order_acquire)) {
    state.running.store(true, std::memory_order_release);
    auto r = zq.q[zstart];
    if (should_pop(r, nullptr)) {
      zq.start.store(zstart + 1, std::memory_order_relaxed);
      state.current = r;
      return true;
    }
    return false;
  }

  ProcessPending(q);

  if (q.len > 0) {
    auto node = q.q[0].ent->values.next;

    auto promise_routine = node->object()->promise_routine;

    state.running.store(true, std::memory_order_relaxed);
    if (should_pop(promise_routine, node->object()->state)) {
      node->Remove();
      q.pool.Free(node);

      auto top = q.q[0];
      if (top.ent->values.empty()) {
        std::pop_heap(q.q, q.q + q.len, Greater);
        q.q[q.len - 1].ent = nullptr;
        q.len--;

        top.ent->Remove();
        q.pool.Free(top.ent);
      }

      state.current = promise_routine;
      return true;
    }
    return false;
  }

  state.running.store(false, std::memory_order_relaxed);

  // We do not need locks to protect completion counters. There can only be MT
  // access on Pop() and Add(), the counters are per-core anyway.
  auto &c = state.complete_counter;
  auto n = c.completed;
  auto comp = EpochClient::g_workload_client->completion_object();
  c.completed = 0;

  unsigned long nr_bubbles = tot_bubbles.load();
  while (!tot_bubbles.compare_exchange_strong(nr_bubbles, 0));

  if (n + nr_bubbles > 0) {
    // logger->info("DispatchService on core {} notifies {} completions",
    //             core_id, n + nr_bubbles);
    comp->Complete(n + nr_bubbles);
  }
  return false;
}

void EpochExecutionDispatchService::AddBubble()
{
  tot_bubbles.fetch_add(1);
}

bool EpochExecutionDispatchService::Preempt(int core_id, bool force, BasePromise::ExecutionRoutine *routine_state)
{
  auto &lock = queues[core_id]->lock;
  bool new_routine = true;
  auto &zq = queues[core_id]->zq;
  auto &q = queues[core_id]->pq;
  auto &state = queues[core_id]->state;

  ProcessPending(q);

  auto &r = state.current;
  auto key = std::get<0>(r)->sched_key;

  if (!force && zq.end.load(std::memory_order_acquire) == zq.start.load(std::memory_order_acquire)) {
    if (q.len == 0 || key < q.q[0].key) {
      new_routine = false;
      goto done;
    }
  }

  if (key == 0) {
    zq.q[zq.end.load(std::memory_order_acquire)] = r;
    zq.end.fetch_add(1, std::memory_order_release);
  } else  {
    AddToPriorityQueue(q, r, routine_state);
  }
  state.running.store(false, std::memory_order_relaxed);

done:
  return new_routine;
}


void EpochExecutionDispatchService::Complete(int core_id)
{
  auto &state = queues[core_id]->state;
  auto &c = state.complete_counter;
  c.completed++;
}

int EpochExecutionDispatchService::TraceDependency(uint64_t key)
{
  for (int core_id = 0; core_id < NodeConfiguration::g_nr_threads; core_id++) {
    auto max_item_percore = g_max_item / NodeConfiguration::g_nr_threads;
    auto &q = queues[core_id]->pq.pending;
    if (q.end.load() > max_item_percore) puts("pending queue wraps around");
    abort_if(q.end.load() < q.start.load(), "WTF? pending queue underflows");
    for (auto i = q.start.load(); i < q.end.load(); i++) {
      if (std::get<0>(q.q[i % max_item_percore])->sched_key == key) {
        printf("found %lu in the pending area of %d\n", key, core_id);
      }
    }
    for (auto i = 0; i < q.end.load(); i++) {
      if (std::get<0>(q.q[i % max_item_percore])->sched_key == key) {
        printf("found %lu in the consumed pending area of %d\n", key, core_id);
      }
    }

    auto &hl = queues[core_id]->pq.ht[Hash(key) % kHashTableSize];
    auto ent = hl.next;
    while (ent != &hl) {
      if (ent->object()->key == key) {
        if (ent->object()->values.empty()) {
          printf("found but empty hash entry of key %lu on core %d\n", key, core_id);
        }
        return core_id;
      }
      ent = ent->next;
    }
    if (std::get<0>(queues[core_id]->state.current)->sched_key == key)
      return core_id;
  }
  return -1;
}

static constexpr size_t kEpochPromiseAllocationWorkerLimit = 512_M;
static constexpr size_t kEpochPromiseAllocationMainLimit = 64_M;
static constexpr size_t kEpochPromiseMiniBrkSize = 4 * CACHE_LINE_SIZE;

EpochPromiseAllocationService::EpochPromiseAllocationService()
{
  size_t acc = 0;
  for (size_t i = 0; i <= NodeConfiguration::g_nr_threads; i++) {
    auto s = kEpochPromiseAllocationWorkerLimit / NodeConfiguration::g_nr_threads;
    int numa_node = -1;
    if (i == 0) {
      s = kEpochPromiseAllocationMainLimit;
    } else {
      numa_node = (i - 1 + NodeConfiguration::g_core_shifting) / mem::kNrCorePerNode;
    }
    brks[i] = mem::Brk::New(mem::MemMapAlloc(mem::Promise, s, numa_node), s);
    acc += s;
    constexpr auto mini_brk_size = 4 * CACHE_LINE_SIZE;
    minibrks[i] = mem::Brk::New(
        brks[i]->Alloc(kEpochPromiseMiniBrkSize),
        kEpochPromiseMiniBrkSize);
  }
  // logger->info("Memory allocated: PromiseAllocator {}GB", acc >> 30);
}

void *EpochPromiseAllocationService::Alloc(size_t size)
{
  int thread_id = go::Scheduler::CurrentThreadPoolId();
  if (size < CACHE_LINE_SIZE) {
    auto b = minibrks[thread_id];
    if (!b->Check(size)) {
      b = mem::Brk::New(
          brks[thread_id]->Alloc(kEpochPromiseMiniBrkSize),
          kEpochPromiseMiniBrkSize);
    }
    return b->Alloc(size);
  } else {
    return brks[thread_id]->Alloc(util::Align(size, CACHE_LINE_SIZE));
  }
}

void EpochPromiseAllocationService::Reset()
{
  for (size_t i = 0; i <= NodeConfiguration::g_nr_threads; i++) {
    // logger->info("  PromiseAllocator {} used {}MB. Resetting now.", i,
    // brks[i].current_size() >> 20);
    brks[i]->Reset();
    minibrks[i] = mem::Brk::New(
        brks[i]->Alloc(kEpochPromiseMiniBrkSize),
        kEpochPromiseMiniBrkSize);
  }
}

static constexpr size_t kEpochMemoryLimitPerCore = 16_M;

EpochMemory::EpochMemory()
{
  logger->info("Allocating EpochMemory");
  auto &conf = util::Instance<NodeConfiguration>();
  for (int i = 0; i < conf.nr_nodes(); i++) {
    node_mem[i].mmap_buf =
        (uint8_t *) mem::MemMap(
            mem::Epoch, nullptr, kEpochMemoryLimitPerCore * conf.g_nr_threads,
            PROT_READ | PROT_WRITE,
            MAP_HUGETLB | MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    for (int t = 0; t < conf.g_nr_threads; t++) {
      auto p = node_mem[i].mmap_buf + t * kEpochMemoryLimitPerCore;
      auto numa_node = (t + conf.g_core_shifting) / mem::kNrCorePerNode;
      unsigned long nodemask = 1 << numa_node;
      abort_if(syscall(
          __NR_mbind,
          p, kEpochMemoryLimitPerCore,
          2 /* MPOL_BIND */,
          &nodemask,
          sizeof(unsigned long) * 8,
          1 << 0 /* MPOL_MF_STRICT */) < 0, "mbind failed!");
    }
    abort_if(mlock(node_mem[i].mmap_buf, kEpochMemoryLimitPerCore * conf.g_nr_threads) < 0,
             "Cannot allocate memory. mlock() failed.");
  }
  Reset();
}

EpochMemory::~EpochMemory()
{
  logger->info("Freeing EpochMemory");
  auto &conf = util::Instance<NodeConfiguration>();
  for (int i = 0; i < conf.nr_nodes(); i++) {
    munmap(node_mem[i].mmap_buf, kEpochMemoryLimitPerCore * conf.g_nr_threads);
  }
}

void EpochMemory::Reset()
{
  auto &conf = util::Instance<NodeConfiguration>();
  // I only manage the current node.
  auto node_id = conf.node_id();
  auto &m = node_mem[node_id - 1];
  for (int t = 0; t < conf.g_nr_threads; t++) {
    auto p = m.mmap_buf + t * kEpochMemoryLimitPerCore;
    m.brks[t] = mem::Brk::New(p, kEpochMemoryLimitPerCore);
  }
}

Epoch *EpochManager::epoch(uint64_t epoch_nr) const
{
  abort_if(epoch_nr != cur_epoch_nr, "Confused by epoch_nr {} since current epoch is {}",
           epoch_nr, cur_epoch_nr);
  return cur_epoch;
}

uint8_t *EpochManager::ptr(uint64_t epoch_nr, int node_id, uint64_t offset) const
{
  abort_if(epoch_nr != cur_epoch_nr,
           "Confused by epoch_nr {} since current epoch is {}, node {}, offset "
           "{}, current core {}",
           epoch_nr, cur_epoch_nr, node_id, offset, go::Scheduler::CurrentThreadPoolId() - 1);
  return epoch(epoch_nr)->mem->node_mem[node_id - 1].mmap_buf + offset;
}

static Epoch *g_epoch; // We don't support concurrent epochs for now.

void EpochManager::DoAdvance(EpochClient *client)
{
  cur_epoch_nr.fetch_add(1);
  cur_epoch.load()->~Epoch();
  cur_epoch = new (cur_epoch) Epoch(cur_epoch_nr, client, mem);
  logger->info("We are going into epoch {}", cur_epoch_nr);
}

EpochManager::EpochManager(EpochMemory *mem, Epoch *epoch)
    : cur_epoch_nr(0), cur_epoch(epoch), mem(mem)
{
  cur_epoch.load()->mem = mem;
}

}

namespace util {

using namespace felis;

EpochManager *InstanceInit<EpochManager>::instance = nullptr;

InstanceInit<EpochManager>::InstanceInit()
{
  // We currently do not support concurrent epochs.
  static Epoch g_epoch;
  static EpochMemory mem;
  instance = new EpochManager(&mem, &g_epoch);
}

}
