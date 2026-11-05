#include "netlib/net/tcp_server.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "netlib/base/noncopyable.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/buffer.h"
#include "netlib/net/callbacks.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/inet_address.h"
#include "netlib/net/tcp_connection.h"

namespace netlib {

namespace {

// 阻塞的测试客户端，连接 127.0.0.1:port；读超时 5 秒，出 bug 时测试失败而不是卡死
// TcpServer::Start() 之后就可以构造：握手由内核完成，不需要 loop 在运行
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

  int fd() const { return fd_; }

  // 客户端这一端的地址，也就是服务器看到的 peer_addr
  std::string local_ip_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    return InetAddress(addr).ToIpPort();
  }

  void Write(std::string_view data) {
    EXPECT_EQ(::write(fd_, data.data(), data.size()), static_cast<ssize_t>(data.size()));
  }

  // 读一次，最多 n 字节；返回值 < 0 表示出错或超时，== 0 表示 EOF
  ssize_t ReadSome(std::string* out, std::size_t n = 4096) {
    out->assign(n, '\0');
    ssize_t got = ::read(fd_, out->data(), n);
    out->resize(got > 0 ? static_cast<std::size_t>(got) : 0);
    return got;
  }

  // 客户端主动关闭：服务端会读到 0
  void Close() {
    if (fd_ != -1) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_ = -1;
};

class TcpServerTest : public ::testing::Test {
protected:
  // 看门狗：loop 只能由回调里的 Quit() 结束，回调没触发时 Loop() 会永远阻塞；
  // 10 秒后 SIGALRM 的默认动作终止进程，让测试失败而不是卡死
  void SetUp() override { ::alarm(10); }
  void TearDown() override { ::alarm(0); }

  // 找一个当前空闲的端口：bind 端口 0 让内核分配，读出端口号后关掉
  // （关掉到 TcpServer 再 bind 之间有极小的窗口被别人占用，测试里可以接受）
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

  // 127.0.0.1:port 的监听地址
  static InetAddress LoopbackAddr(uint16_t port) {
    return *InetAddress::FromIpPort("127.0.0.1", port);
  }
};

}  // namespace

TEST_F(TcpServerTest, StartAcceptsConnection) {
  // 准备
  EventLoop loop;
  uint16_t port = PickFreePort();
  int connection_count = 0;
  bool connected = false;
  std::string connection_name{};
  std::string peer_ip_port{};

  // 创建服务器
  TcpServer server(&loop, LoopbackAddr(port), "test-server");

  // 设置回调
  server.set_connection_callback([&](const TcpConnectionPtr& c) {
    ++connection_count;
    connected = c->connected();
    connection_name = c->name();
    peer_ip_port = c->peer_addr().ToIpPort();
    loop.Quit();
  });

  // 服务器开始监听
  server.Start();

  // 客户端连接服务器
  Client client(port);

  loop.Loop();

  // 断言
  EXPECT_EQ(connection_count, 1);
  EXPECT_TRUE(connected);
  EXPECT_EQ(connection_name, "test-server-127.0.0.1:" + std::to_string(port) + "#0");
  EXPECT_EQ(peer_ip_port, client.local_ip_port());
}

TEST_F(TcpServerTest, MessageCallbackReceivesData) {
  // 准备
  EventLoop loop;
  uint16_t port = PickFreePort();

  // 记录变量
  int message_count = 0;
  std::string content_from_client{};
  std::string content_from_server{};

  // 创建服务器
  TcpServer server(&loop, LoopbackAddr(port), "test-server");

  // 设置回调
  server.set_message_callback([&](const TcpConnectionPtr& c, Buffer* buf, Timestamp) {
    ++message_count;
    content_from_client = buf->RetrieveAllAsString();
    c->Send(content_from_client);
    loop.Quit();
  });

  // 启动服务器
  server.Start();

  // 创建客户端
  Client client(port);
  client.Write("hello");

  loop.Loop();

  ssize_t ret = client.ReadSome(&content_from_server);

  // 断言
  EXPECT_EQ(ret, 5);
  EXPECT_EQ(message_count, 1);
  EXPECT_EQ(content_from_client, "hello");
  EXPECT_EQ(content_from_server, "hello");
}

TEST_F(TcpServerTest, PeerCloseRemovesConnection) {
  // 准备
  EventLoop loop;
  uint16_t port = PickFreePort();

  // 记录变量
  int connection_count = 0;
  bool connected = true;
  std::weak_ptr<TcpConnection> weak_conn{};

  // 创建服务器
  TcpServer server(&loop, LoopbackAddr(port), "test-server");

  // 设置回调
  server.set_connection_callback([&](const TcpConnectionPtr& c) {
    ++connection_count;
    connected = c->connected();

    if (c->connected()) {
      weak_conn = c;
    } else {
      loop.Quit();
    }
  });

  // 启动服务器
  server.Start();

  // 创建客户端
  Client client(port);
  client.Close();

  loop.Loop();

  // 断言
  EXPECT_EQ(connection_count, 2);
  EXPECT_FALSE(connected);
  EXPECT_TRUE(weak_conn.expired());
}

TEST_F(TcpServerTest, ConnectionNamesAreUnique) {
  // 准备
  EventLoop loop;
  uint16_t port = PickFreePort();

  // 记录变量
  std::vector<std::string> names{};

  // 创建服务器
  TcpServer server(&loop, LoopbackAddr(port), "test-server");

  // 设置回调
  server.set_connection_callback([&](const TcpConnectionPtr& c) {
    if (c->connected()) {
      names.push_back(c->name());

      if (names.size() >= 2) {
        loop.Quit();
      }
    }
  });

  // 启动服务器
  server.Start();

  // 创建客户端
  Client client1(port);
  Client client2(port);

  loop.Loop();

  // 断言
  ASSERT_EQ(names.size(), 2);
  EXPECT_EQ(names[0], "test-server-127.0.0.1:" + std::to_string(port) + "#0");
  EXPECT_EQ(names[1], "test-server-127.0.0.1:" + std::to_string(port) + "#1");
}

TEST_F(TcpServerTest, DestructorDestroysLiveConnections) {
  // 准备
  EventLoop loop;
  uint16_t port = PickFreePort();

  // 记录变量
  int connection_count = 0;
  bool connected = true;
  std::weak_ptr<TcpConnection> weak_conn;
  std::string reply{};

  // 创建服务器
  auto server = std::make_unique<TcpServer>(&loop, LoopbackAddr(port), "test-server");

  // 设置回调
  server->set_connection_callback([&](const TcpConnectionPtr& c) {
    ++connection_count;
    connected = c->connected();

    if (c->connected()) {
      weak_conn = c;
      loop.Quit();
    }
  });

  // 启动服务器
  server->Start();

  // 创建客户端
  Client client(port);

  loop.Loop();

  // 断言1
  EXPECT_EQ(connection_count, 1);
  EXPECT_TRUE(connected);
  EXPECT_FALSE(weak_conn.expired());

  server.reset();

  // 断言2
  EXPECT_EQ(connection_count, 2);
  EXPECT_TRUE(weak_conn.expired());
  EXPECT_FALSE(connected);
  EXPECT_EQ(client.ReadSome(&reply), 0);
}

TEST_F(TcpServerTest, ForceCloseInConnectionCallback) {
  // 准备
  EventLoop loop;
  uint16_t port = PickFreePort();

  // 记录变量
  int connection_count = 0;
  bool connected = true;
  std::weak_ptr<TcpConnection> weak_conn;
  std::string reply{};

  // 创建服务器
  TcpServer server(&loop, LoopbackAddr(port), "test-server");

  // 设置回调
  server.set_connection_callback([&](const TcpConnectionPtr& c) {
    ++connection_count;
    connected = c->connected();

    if (c->connected()) {
      weak_conn = c;
      c->ForceClose();
    } else {
      loop.Quit();
    }
  });

  // 启动服务器
  server.Start();

  // 创建客户端
  Client client(port);

  loop.Loop();

  // 断言
  EXPECT_EQ(connection_count, 2);
  EXPECT_FALSE(connected);
  EXPECT_TRUE(weak_conn.expired());
  EXPECT_EQ(client.ReadSome(&reply), 0);
}

TEST_F(TcpServerTest, IgnoresSigPipe) {
  struct sigaction old_action{};
  int ret = ::sigaction(SIGPIPE, nullptr, &old_action);

  EXPECT_EQ(ret, 0);
  EXPECT_EQ(old_action.sa_handler, SIG_IGN);
}

}  // namespace netlib
