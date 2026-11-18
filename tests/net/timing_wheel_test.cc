#include "netlib/net/timing_wheel.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "netlib/base/noncopyable.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/buffer.h"
#include "netlib/net/callbacks.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/inet_address.h"
#include "netlib/net/tcp_connection.h"
#include "netlib/net/tcp_server.h"

namespace netlib {

namespace {

using namespace std::chrono_literals;

class Client : Noncopyable {
public:
  explicit Client(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(fd_, 0);
    timeval timeout{5, 0};
    EXPECT_EQ(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    EXPECT_EQ(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  }

  ~Client() { Close(); }

  void Write(std::string_view data) {
    EXPECT_EQ(::write(fd_, data.data(), data.size()), static_cast<ssize_t>(data.size()));
  }

  // 返回 0 表示对端已关闭（EOF）
  ssize_t ReadSome(std::string* out, std::size_t n = 4096) {
    out->assign(n, '\0');
    ssize_t got = ::read(fd_, out->data(), n);
    out->resize(got > 0 ? static_cast<std::size_t>(got) : 0);
    return got;
  }

  void Close() {
    if (fd_ != -1) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_ = -1;
};

class TimingWheelTest : public ::testing::Test {
protected:
  void SetUp() override { ::alarm(10); }
  void TearDown() override { ::alarm(0); }

  static uint16_t PickFreePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    ::close(fd);
    return ntohs(addr.sin_port);
  }

  static InetAddress LoopbackAddr(uint16_t port) {
    return *InetAddress::FromIpPort("127.0.0.1", port);
  }
};

TEST_F(TimingWheelTest, IdleConnectionGetsClosed) {
  uint16_t port = PickFreePort();

  EventLoop loop;
  bool disconnected = false;
  TimingWheel wheel(&loop, 3, 20ms);  // 60ms 超时
  TcpServer server(&loop, LoopbackAddr(port), "idle-test");

  server.set_connection_callback([&](const TcpConnectionPtr& conn) {
    wheel.OnConnection(conn);
    if (!conn->connected()) {
      disconnected = true;
      loop.Quit();
    }
  });

  server.Start();
  // 一言不发
  Client client(port);

  loop.Loop();

  EXPECT_TRUE(disconnected);

  std::string buf{};
  EXPECT_EQ(client.ReadSome(&buf), 0);
}

TEST_F(TimingWheelTest, ActiveConnectionStaysAlive) {
  uint16_t port = PickFreePort();
  EventLoop loop;

  bool disconnected = false;
  bool alive_at_90ms = false;

  TimingWheel wheel(&loop, 3, 20ms);
  TcpServer server(&loop, LoopbackAddr(port), "active-test");

  server.set_connection_callback([&](const TcpConnectionPtr& conn) {
    wheel.OnConnection(conn);

    if (!conn->connected()) {
      disconnected = true;
      loop.Quit();
    }
  });

  server.set_message_callback(
      [&](const TcpConnectionPtr& conn, Buffer* buf, Timestamp /*receive_time*/) {
        // 清空缓冲区域
        buf->RetrieveAll();
        wheel.OnMessage(conn);
      });

  server.Start();

  Client client(port);
  loop.RunAfter(30ms, [&] { client.Write("ping1"); });
  loop.RunAfter(60ms, [&] { client.Write("ping2"); });
  loop.RunAfter(90ms, [&] { alive_at_90ms = !disconnected; });

  loop.Loop();

  EXPECT_TRUE(alive_at_90ms);
  EXPECT_TRUE(disconnected);
}

TEST_F(TimingWheelTest, PeerClosesBeforeTimeoutDoesNotCrash) {
  // 验证下线回调只触发 1 次、Entry 滑出时不会二次关闭或崩溃
  uint16_t port = PickFreePort();
  EventLoop loop;

  int disconnect_count = 0;

  TimingWheel wheel(&loop, 3, 20ms);
  TcpServer server(&loop, LoopbackAddr(port), "early-close-test");

  server.set_connection_callback([&](const TcpConnectionPtr& conn) {
    wheel.OnConnection(conn);

    if (!conn->connected()) {
      ++disconnect_count;
    }
  });

  server.Start();

  Client client(port);
  loop.RunAfter(10ms, [&] { client.Close(); });
  loop.RunAfter(80ms, [&] { loop.Quit(); });

  loop.Loop();

  EXPECT_EQ(disconnect_count, 1);
}

}  // namespace

}  // namespace netlib
