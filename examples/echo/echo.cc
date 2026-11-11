#include <getopt.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include "netlib/base/logging.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/buffer.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/inet_address.h"
#include "netlib/net/tcp_connection.h"
#include "netlib/net/tcp_server.h"

static void PrintUsage(std::ostream& os, const char* prag) {
  os << "usage: " << prag << " [--port/-p port] [--threads/-t threads]\n";
}

static bool ParseInt(std::string_view s, int min, int max, int* out) {
  if (s.empty()) {
    return false;
  }

  int num = 0;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, num);

  // 解析错、没吃完整串、超出 [min, max]
  if (ec != std::errc{} || ptr != last || num < min || num > max) {
    return false;
  }

  *out = num;
  return true;
}

int main(int argc, char* argv[]) {
  // 端口默认从命令行参数读取，没有参数默认为 8080
  int port = 8080;
  // 线程数默认从命令行参数读取，没有参数默认为 0
  int num_threads = 0;

  static const option kLongOptions[] = {
      {"port", required_argument, nullptr, 'p'},
      {"threads", required_argument, nullptr, 't'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  int opt{};
  for (;;) {
    opt = ::getopt_long(argc, argv, "p:t:h", kLongOptions, nullptr);

    if (opt == -1) {
      break;
    }

    switch (opt) {
      case 'p':
        if (!ParseInt(optarg, 1, std::numeric_limits<uint16_t>::max(), &port)) {
          LOG_ERROR << "invalid port, got=" << optarg << ", expect=1-"
                    << std::numeric_limits<uint16_t>::max();
          return EXIT_FAILURE;
        }
        break;
      case 't':
        if (!ParseInt(optarg, 0, 64, &num_threads)) {
          LOG_ERROR << "invalid threads, got=" << optarg << ", expect=0-64";
          return EXIT_FAILURE;
        }
        break;
      case 'h':
        PrintUsage(std::cout, argv[0]);
        return EXIT_SUCCESS;
      default:
        std::cerr << "arguments error!\n";
        PrintUsage(std::cerr, argv[0]);
        return EXIT_FAILURE;
    }
  }

  // 存在多余参数
  if (optind < argc) {
    std::cerr << "unexpected argument: " << argv[optind] << "\n";
    PrintUsage(std::cerr, argv[0]);
    return EXIT_FAILURE;
  }

  // 事件循环
  netlib::EventLoop loop;
  // 监听地址
  netlib::InetAddress listen_addr(static_cast<uint16_t>(port));
  // 创建服务器
  netlib::TcpServer server(&loop, listen_addr, "echo");

  // 设置回调
  server.set_connection_callback([](const netlib::TcpConnectionPtr& c) {
    LOG_INFO << "[" << c->name() << "] " << (c->connected() ? "up" : "down")
             << ", peer=" << c->peer_addr().ToIpPort();
  });

  server.set_message_callback([](const netlib::TcpConnectionPtr& c, netlib::Buffer* buf,
                                 netlib::Timestamp /*receive_time*/) {
    std::string message_from_client = buf->RetrieveAllAsString();
    c->Send(message_from_client);
  });

  server.SetThreadNum(num_threads);
  server.Start();
  LOG_INFO << "echo server started, listen=" << listen_addr.ToIpPort()
           << ", io_threads=" << num_threads << (num_threads == 0 ? " (single reactor)" : "");

  loop.Loop();

  return 0;
}
