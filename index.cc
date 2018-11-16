#include <fstream>
#include <limits>
#include <streambuf>
#include <iomanip>
#include <dlfcn.h>
#include "index.h"
#include "epoch.h"
#include "util.h"
#include "mem.h"
#include "json11/json11.hpp"
#include "gopp/gopp.h"

#include "felis_probes.h"

using util::Instance;

namespace felis {

std::map<std::string, Checkpoint *> Checkpoint::impl;

void IndexEntity::DecodeIOVec(struct iovec *vec)
{
  auto p = (uint8_t *) vec->iov_base;
  auto key_size = vec->iov_len - 12;
  if (k == nullptr || k->len < key_size) {
    delete k;
    k = VarStr::New(key_size);
  }
  memcpy(&rel_id, p, 4);
  memcpy((uint8_t *) k + sizeof(VarStr), p + 4, key_size);
  memcpy(&handle_ptr, p + key_size + 4, 8); // This is a pointer on the original machine though.
}

int IndexEntity::EncodeIOVec(struct iovec *vec, int max_nr_vec)
{
  if (max_nr_vec < 3)
    return 0;

  vec[0].iov_len = 4;
  vec[0].iov_base = &rel_id;
  vec[1].iov_len = k->len;
  vec[1].iov_base = (void *) k->data;
  vec[2].iov_len = 8;
  vec[2].iov_base = &handle_ptr;

  encoded_len = 12 + k->len;

  return 3;
}

IndexEntity::~IndexEntity()
{
  delete k;
}

void IndexShipmentReceiver::Run()
{
  // clear the affinity
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  IndexEntity ent;
  auto &mgr = Instance<RelationManager>();

  logger->info("New Shipment has arrived!");
  PerfLog perf;
  while (Receive(&ent)) {
    // TODO: multi-thread this?
    auto &rel = mgr[ent.rel_id];
    auto handle = rel.InsertOrDefault(ent.k, [&ent]() { return ent.handle_ptr; });
  }
  logger->info("Shipment processing finished");
  perf.End();
  perf.Show("Processing takes");
}

IndexShipmentReceiver::~IndexShipmentReceiver()
{
  delete sock;
}

RowSlicer::RowSlicer(int nr_slices)
    : nr_slices(nr_slices)
{
  index_slices = new Slice*[nr_slices];
  index_slice_scanners = new IndexSliceScanner*[nr_slices];
}

IndexEntity *RowSlicer::OnNewRow(int slice_idx, IndexEntity *ent)
{
  index_slices[slice_idx]->Append(ent->shipping_handle());

  // We still need to call MarkDirty() just in case the scanner is running in
  // progress.
  if (ent->shipping_handle()->MarkDirty())
    index_slice_scanners[slice_idx]->AddObject(ent);
  return ent;
}

std::vector<IndexShipment *> RowSlicer::all_index_shipments()
{
  std::vector<IndexShipment *> all;
  for (int i = 0; i < nr_slices; i++) {
    if (index_slice_scanners[i] == nullptr)
      continue;
    auto shipment = index_slice_scanners[i]->shipment();
    if (shipment) all.push_back(shipment);
  }
  return all;
}

}
