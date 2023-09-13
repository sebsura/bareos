/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2023 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "benchmark/benchmark.h"
#else
#  include "include/bareos.h"
#  include "benchmark/benchmark.h"
#endif

#include "lib/bsock.h"
#include "lib/bsock_tcp.h"

#include <thread>
#include <iostream>
#include <string_view>
#include <poll.h>

#include <signal.h>

#define ENABLE_TLS

BareosSocket* sock;

static int OpenSocketAndBind(int family, int port)
{
  int fd = -1;
  int tries = 0;

  do {
    ++tries;
    if ((fd = socket(family, SOCK_STREAM, 0)) < 0) {
      Bmicrosleep(10, 0);
    }
  } while (fd < 0 && tries < 6);

  ASSERT(fd >= 0);

  int reuseaddress = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (sockopt_val_t)&reuseaddress,
                 sizeof(reuseaddress))
      < 0) {
    return -2;
  }

  if (family == AF_INET6) {
    int ipv6only_option_value = 1;
    socklen_t option_len = sizeof(int);

    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                   (sockopt_val_t)&ipv6only_option_value, option_len)
        < 0) {
      return -2;
    }
  }

  tries = 0;

  struct sockaddr_storage addr_storage;

  if (family == AF_INET) {
    auto* addr = (struct sockaddr_in*) &addr_storage;
    addr->sin_family = AF_INET;
    inet_aton("127.0.0.1", &addr->sin_addr);
    addr->sin_port = htons(port);
  }

  do {
    ++tries;
    if (bind(fd, (sockaddr*)&addr_storage, sizeof(addr_storage)) < 0) {
      Bmicrosleep(5, 0);
    } else {
      // success
      return fd;
    }
  } while (tries < 3);

  return -3;
}

std::atomic<bool> stop_receive{false};

void receive()
{
  auto fd = OpenSocketAndBind(AF_INET, 12345);
  ASSERT(fd >= 0);
  ASSERT(listen(fd, 5) >= 0);

  struct pollfd pfd = {};
  pfd.fd = fd;

  int events = POLLIN;
  events |= POLLRDNORM;
  events |= POLLRDBAND;
  events |= POLLPRI;
  pfd.events = events;

  int status;
  int nmsg = 0;
  while (status = poll(&pfd, 1, 50), status >= 0) {
    if (stop_receive.load()) { break; }
    if (status == 0) continue;
    if (!(pfd.revents & events)) continue;
    struct sockaddr_storage client;
    socklen_t len = sizeof(client);
    int newsock = -1;

    do {
      newsock = accept(fd, (sockaddr*)&client, &len);
    } while (newsock < 0 && errno == EINTR);
    ASSERT(newsock >= 0);

    int keepalive = 1;
    if (setsockopt(newsock, SOL_SOCKET, SO_KEEPALIVE,
		   (sockopt_val_t)&keepalive, sizeof(keepalive))
	< 0) {
      std::cerr << "keep alive warning" << std::endl;
    }

    BareosSocket* sock = new BareosSocketTCP;
    sock->fd_ = newsock;
#ifdef ENABLE_TLS
    TlsResource res;
    //res.authenticate_ = true; /* Authenticate only with TLS */
    res.tls_enable_ = true;
    res.tls_require_ = true;
    std::unordered_map<std::string, std::string> map{{"input", "password"}};
    ASSERT(sock->DoTlsHandshakeAsAServer(&res, &map, nullptr));
#endif

    bool stop = false;
    for (;;) {
      std::size_t bytes_received = 0;
      while (sock->recv() >= 0) {
	bytes_received += sock->message_length;
	nmsg += 1;
      }

      auto ret = sock->message_length;

      if (ret == -1) {
	//Dmsg0(50, "Received %llu bytes\n", bytes_received);
	sock->send((const char*)&bytes_received, sizeof(bytes_received));
      } else {
	break;
      }

    }
    sock->close();
    delete sock;

    nmsg = 0;

    if (stop) { break; }
  }
}

struct handle {
  std::thread receiver{receive};
  handle() {
    // debug_level = 500;
#if !defined(HAVE_WIN32)
    struct sigaction sig = {};
    sig.sa_handler = SIG_IGN;
    sigaction(SIGUSR2, &sig, nullptr);
    sigaction(SIGPIPE, &sig, nullptr);
#endif
  }

  ~handle()
  {
    // BareosSocket* sock = new BareosSocketTCP;
    // ASSERT(sock->connect(nullptr, 10, 10, 10, "Input", "localhost",
    // 			 nullptr, 12345, false));

    // TlsResource res;
    // //res.authenticate_ = true; /* Authenticate only with TLS */
    // res.tls_enable_ = true;
    // res.tls_require_ = true;
    // ASSERT(sock->DoTlsHandshake(TlsPolicy::kBnetTlsAuto,
    // 				&res, false,
    // 				"input", "password",
    // 				nullptr));

    // std::unique_lock l(mut);
    // std::cout << "4" << std::endl;
    // ASSERT(sock == nullptr);
    // Setup_();

    // PoolMem msg;
    // msg.check_size(message_size);

    // sock->signal(-3);
    // sock->close();
    // delete sock;
    // sock = nullptr;

    stop_receive.store(true);

    receiver.join();
  }

};

handle H;

static void Setup(const benchmark::State& s)
{
  sock = new BareosSocketTCP;
  ASSERT(sock->connect(nullptr, 10, 10, 10, "Input", "localhost",
		       nullptr, 12345, false));

#ifdef ENABLE_TLS
  TlsResource res{};
  //res.authenticate_ = true; /* Authenticate only with TLS */
  res.tls_enable_ = true;
  res.tls_require_ = true;
  ASSERT(sock->DoTlsHandshake(TlsPolicy::kBnetTlsAuto,
  			      &res, false,
  			      "input", "password",
  			      nullptr));
#endif

  switch (s.range(2)) {
  case 0: {} break;
  case 1: {
    sock->MakeWritesBuffered();
    sock->MakeReadsBuffered();
  } break;
  }
}

static void TearDown(const benchmark::State&)
{
  sock->MakeWritesUnBuffered();
  sock->MakeReadsUnBuffered();
  sock->signal(-2);
  sock->close();
  delete sock;
  sock = nullptr;
}

static void BM_send(benchmark::State& state)
{
  std::size_t message_size = state.range(0);
  std::size_t data_size = state.range(1);

  PoolMem msg;
  msg.check_size(message_size);

  auto* old = sock->msg;
  std::size_t counter = 0;

  ASSERT(message_size >= sizeof(counter));
  for (auto _ : state) {
    std::size_t to_go = data_size;
    while (to_go > 0) {
      std::size_t size = std::min(to_go, message_size);
      // Dmsg0(50, "Sending %llu bytes\n", size);
      msg.check_size(size);
      sock->message_length = size;
      sock->msg = msg.addr();
      benchmark::DoNotOptimize(sock->send());
      to_go -= size;
    }
    sock->signal(-1);
    auto num_bytes = sock->recv();
    ASSERT(num_bytes == sizeof(size_t));
    std::size_t bytes_reached;
    memcpy(&bytes_reached, sock->msg, sizeof(std::size_t));
    ASSERT(bytes_reached == data_size);
    counter += 1;
  }
  state.SetBytesProcessed(counter * data_size);

  sock->msg = old;
}

BENCHMARK(BM_send)
  ->Setup(Setup)
  ->Teardown(TearDown)
  ->Args({50, 1024 *  1024, 0})
  ->Args({50, 1024 *  1024, 1})
  ->Ranges({{64 * 1024, 512 * 1024}, {1 * 1024, 512 * 1024 * 1024}, {0, 1}});

BENCHMARK_MAIN();
