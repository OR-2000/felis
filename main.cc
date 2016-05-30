#include <unistd.h>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "net-io.h"
#include "epoch.h"
#include "worker.h"
#include "goplusplus/gopp.h"
#include "goplusplus/epoll-channel.h"

#include "log.h"

void DoDumpNetwork(int fds[])
{
  int nfds = dolly::Worker::kNrThreads;
  std::ofstream **fout = new std::ofstream*[nfds];
  char *buffer = new char[4096];

  for (int i = 0; i < nfds; i++) {
    fout[i] = new std::ofstream(std::string("dolly-net.") + std::to_string(i) + ".dump");
  }

  logger->info("Dumping the network packets into dolly-net.*.dump");

  int max_fd = -1;
  fd_set fset;
  fd_set eof_fset;
  FD_ZERO(&fset);
  FD_ZERO(&eof_fset);

  // set sockets to async and watch for reading
  for (int i = 0; i < nfds; i++) {
    int opt = fcntl(fds[i], F_GETFL);
    fcntl(fds[i], F_SETFL, opt | O_NONBLOCK);
    FD_SET(fds[i], &fset);
    max_fd = std::max(max_fd, fds[i] + 1);
  }
  int eof_count = 0;
  while (true) {
    int res = select(max_fd, &fset, NULL, NULL, NULL);
    if (errno == EINTR || res == 0) continue;

    if (res < 0) {
      perror("select");
      std::abort();
    }
    logger->debug("found {} connections readable", res);
    max_fd = -1;
    for (int i = 0; i < nfds; i++) {
      int fd = fds[i];
      if (!FD_ISSET(fd, &fset)) {
	if (!FD_ISSET(fd, &eof_fset)) {
	  FD_SET(fd, &fset);
	  max_fd = std::max(max_fd, fd + 1);
	}
	continue;
      }
      int rsize = 0;
      int rcount = 0;
      int tot_rsize = 0;
      while (++rcount < 160 && (rsize = read(fd, buffer, 4096)) > 0) {
	fout[i]->write(buffer, rsize);
	tot_rsize += rsize;
      }
      if (rsize < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
	perror("read");
	std::abort();
      }
      if (rsize == 0) {
	logger->debug("connection {} EOF", i);
	FD_CLR(fd, &fset);
	FD_SET(fd, &eof_fset);
	eof_count++;
      } else {
	logger->debug("connection {} recv {} bytes", i, tot_rsize);
	max_fd = std::max(max_fd, fd + 1);
      }
    }
    if (eof_count == nfds)
      break;
  }
  puts("Network dump all done!");
  delete [] buffer;
  for (int i = 0; i < nfds; i++)
    delete fout[i];
  delete [] fout;
}

void show_usage(const char *progname)
{
  printf("Usage: %s [-t core_count] [-d]\n\n", progname);
  puts("\t-t\tnumber of cores used");
  puts("\t-d\tdump only");
  puts("\t-r\treplay from trace files");

  std::exit(-1);
}

static const char *kDummyWebServerHost = "127.0.0.1";
static const int kDummyWebServerPort = 8000;

int main(int argc, char *argv[])
{
  int opt;
  bool dump_only = false;
  bool replay_from_file = false;

  while ((opt = getopt(argc, argv, "drt:")) != -1) {
    switch (opt) {
    case 'd':
      dump_only = true;
      break;
    case 'r':
      replay_from_file = true;
      break;
    case 't':
      dolly::Worker::kNrThreads = atoi(optarg);
      break;
    default:
      show_usage(argv[0]);
      break;
    }
  }

  // check

  InitializeLogger();

  int fd = dolly::SetupServer();
  int peer_fd = -1;

  socklen_t addrlen = sizeof(struct sockaddr_in);
  struct sockaddr_in addr;

  // we assume each core is creating a connection
  int *peer_fds = new int[dolly::Worker::kNrThreads];

  if (replay_from_file) {
    memset(&addr, 0, addrlen);
    addr.sin_addr.s_addr = inet_addr(kDummyWebServerHost);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDummyWebServerPort);
  }

  for (int i = 0; i < dolly::Worker::kNrThreads; i++) {
    if (replay_from_file) {
      peer_fd = socket(AF_INET, SOCK_STREAM, 0);
      if (connect(peer_fd, (struct sockaddr *) &addr, addrlen) < 0) {
	perror("connect");
	std::abort();
      }
    } else {
      if ((peer_fd = accept(fd, (struct sockaddr *) &addr, &addrlen)) < 0) {
	perror("accept");
	std::abort();
      }
    }
    peer_fds[i] = peer_fd;
  }
  logger->info("replica received {} connections\n", dolly::Worker::kNrThreads);
  logger->info("replica ready!");

  if (dump_only) {
    DoDumpNetwork(peer_fds);
    std::exit(0);
  }

  dolly::BaseRequest::LoadWorkloadSupportFromConf();

  auto epoch_routine = go::Make([peer_fds, replay_from_file]{
      try {
	std::vector<go::EpollSocket *> socks;

	for (int i = 0; i < dolly::Worker::kNrThreads; i++) {
	  auto sock = new go::EpollSocket(peer_fds[i], go::GlobalEpoll(),
				    new go::InputSocketChannel(32 << 20),
				    new go::OutputSocketChannel(4096));
	  if (replay_from_file) {
	    auto out = sock->output_channel();
	    std::stringstream ss;
	    ss << "GET /dolly-net." << i << ".dump HTTP/1.0\r\n\r\n";
	    out->Write(ss.str().c_str(), ss.str().length());
	    auto in = sock->input_channel();
	    uint8_t ch;
	    int line_len = 0;
	    while (in->Read(&ch)) {
	      /*
	      if (ch > 128) std::abort();
	      putchar(ch);
	      */
	      if (ch == '\r')	{
		continue;
	      } else if (ch == '\n') {
		if (line_len == 0) break;
		line_len = 0;
	      } else {
		line_len++;
	      }
	    }
	  }

	  socks.push_back(sock);
	}
	while (true) {
	  dolly::Epoch epoch(socks);
	  logger->info("received a complete epoch");
	}
      } catch (dolly::ParseBufferEOF &ex) {
	logger->info("EOF");
      }
    });

  go::InitThreadPool();
  go::CreateGlobalEpoll();

  auto t = std::thread([]{ go::GlobalEpoll()->EventLoop(); });
  t.detach();

  epoch_routine->StartOn(1);
  go::WaitThreadPool();

  delete [] peer_fds;

  return 0;
}
