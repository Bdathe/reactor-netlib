#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "netlib/base/logging.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/buffer.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/inet_address.h"
#include "netlib/net/tcp_connection.h"
#include "netlib/net/tcp_server.h"

static bool ParsePort(std::string_view s, uint16_t* out) {
  if (s.empty()) {
    return false;
  }

  unsigned int num = 0;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, num);

  // 解析错、没吃完整串、端口 0、端口太大
  if (ec != std::errc{} || ptr != last || num == 0 || num > 65535) {
    return false;
  }

  *out = static_cast<uint16_t>(num);
  return true;
}

int main(int argc, char* argv[]) {
  // 端口默认从命令行参数读取，没有参数默认为 8080
  uint16_t port = 8080;
  if (argc == 2) {
    if (!ParsePort(argv[1], &port)) {
      LOG_ERROR << "invalid port: " << argv[1] << " (expect 1-65535)";
      return EXIT_FAILURE;
    }
  } else if (argc != 1) {
    LOG_ERROR << "usage: " << argv[0] << " [port]";
    return EXIT_FAILURE;
  }

  // 事件循环
  netlib::EventLoop loop;
  // 监听地址
  netlib::InetAddress listen_addr(port);
  // 创建服务器
  netlib::TcpServer server(&loop, listen_addr, "echo");

  // 设置回调
  server.set_connection_callback([](const netlib::TcpConnectionPtr& c) {
    LOG_INFO << "connection: " << c->name() << ", peer address: " << c->peer_addr().ToIpPort()
             << " state: " << (c->connected() ? "connected" : "disconnected");
  });

  server.set_message_callback([](const netlib::TcpConnectionPtr& c, netlib::Buffer* buf,
                                 netlib::Timestamp /*receive_time*/) {
    std::string message_from_client = buf->RetrieveAllAsString();
    c->Send(message_from_client);
  });

  server.Start();
  loop.Loop();

  return 0;
}
