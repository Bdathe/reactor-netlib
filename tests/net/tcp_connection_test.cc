#include "netlib/net/tcp_connection.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "netlib/base/noncopyable.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/buffer.h"
#include "netlib/net/callbacks.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/inet_address.h"

namespace netlib {

namespace {

// 一对已经连上的 TCP socket（127.0.0.1，端口由内核分配，测试之间不会冲突）
// - server 端：非阻塞，交给 TcpConnection，由它的 Socket 负责关闭
// - client 端：阻塞，测试代码直接 read/write；读超时 5 秒，出 bug 时测试失败而不是卡死
// 两端的缓冲区都调小到 64KB，发送几 MB 的数据就一定会写不完，用来测"有积压"的情况
class TcpPair : Noncopyable {
public:
  static constexpr int kSocketBufferSize = 64 * 1024;

  TcpPair() {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(listen_fd, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    EXPECT_EQ(::listen(listen_fd, 1), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);

    client_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(client_fd_, 0);
    SetIntOption(client_fd_, SO_RCVBUF, kSocketBufferSize);  // 要在 connect 之前设置
    timeval timeout{5, 0};
    EXPECT_EQ(::setsockopt(client_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
    EXPECT_EQ(::connect(client_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    server_fd_ = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    EXPECT_GE(server_fd_, 0);
    SetIntOption(server_fd_, SO_SNDBUF, kSocketBufferSize);

    ::close(listen_fd);
  }

  ~TcpPair() { CloseClient(); }

  int server_fd() const { return server_fd_; }
  int client_fd() const { return client_fd_; }

  InetAddress server_local_addr() const { return GetAddr(::getsockname); }
  InetAddress server_peer_addr() const { return GetAddr(::getpeername); }

  // 客户端主动关闭：服务端会读到 0
  void CloseClient() {
    if (client_fd_ != -1) {
      ::close(client_fd_);
      client_fd_ = -1;
    }
  }

private:
  static void SetIntOption(int fd, int option, int value) {
    EXPECT_EQ(::setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value)), 0);
  }

  template <typename GetName>
  InetAddress GetAddr(GetName get_name) const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    EXPECT_EQ(get_name(server_fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    return InetAddress(addr);
  }

  int server_fd_ = -1;
  int client_fd_ = -1;
};

// 用 pair 的 server 端创建一个连接（还没有调用 ConnectEstablished）
TcpConnectionPtr MakeConnection(EventLoop* loop, const TcpPair& pair) {
  return std::make_shared<TcpConnection>(loop, "test-conn", pair.server_fd(),
                                         pair.server_local_addr(), pair.server_peer_addr());
}

// 生成 n 字节有规律的数据（"abc...zabc..."），比对时能发现顺序错乱
std::string MakePattern(std::size_t n) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) {
    s[i] = static_cast<char>('a' + i % 26);
  }
  return s;
}

// 阻塞地从 fd 读 n 字节；遇到 EOF、出错或超时就提前返回已经读到的部分
std::string ReadExactly(int fd, std::size_t n) {
  std::string result;
  char buf[64 * 1024];
  while (result.size() < n) {
    std::size_t want = std::min(sizeof(buf), n - result.size());
    ssize_t got = ::read(fd, buf, want);
    if (got <= 0) {
      break;
    }
    result.append(buf, static_cast<std::size_t>(got));
  }
  return result;
}

// 阻塞地一直读到 EOF（或者出错、超时），返回读到的全部数据
std::string ReadUntilEof(int fd) {
  std::string result;
  char buf[64 * 1024];
  while (true) {
    ssize_t got = ::read(fd, buf, sizeof(buf));
    if (got <= 0) {
      break;
    }
    result.append(buf, static_cast<std::size_t>(got));
  }
  return result;
}

}  // namespace

// 每个测试的结尾都要调用 conn->ConnectDestroyed()：
// 真实场景里由 TcpServer 在 close_callback_ 里调用；测试里没有 TcpServer，要自己调。
// 否则析构时 state_ 不是 kDisconnected，析构函数里的 assert 会失败

TEST(TcpConnectionTest, ConnectEstablishedNotifiesConnected) {
  // ① 准备：一个 loop，加上一对已经连上的 socket
  EventLoop loop;
  TcpPair pair;

  // ② 创建连接对象：必须由 shared_ptr 管理，否则 ConnectEstablished 里的 shared_from_this()
  // 会抛异常
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // ③ 设置回调：回调里不做断言，只把"被调用了几次""当时是否已连接"记到局部变量里
  int callback_count = 0;
  bool connected_in_callback = false;
  conn->set_connection_callback([&](const TcpConnectionPtr& c) {
    ++callback_count;
    connected_in_callback = c->connected();
  });

  // ④ 触发：ConnectEstablished 会当场调用连接回调，所以这个测试不需要跑 loop
  conn->ConnectEstablished();

  // ⑤ 断言：在清理之前检查，因为 ConnectDestroyed 还会再调用一次连接回调（通知下线）
  EXPECT_EQ(callback_count, 1);
  EXPECT_TRUE(connected_in_callback);

  // ⑥ 清理：状态改为 kDisconnected，Channel 从 Poller 移除；否则析构函数里的 assert 会失败
  conn->ConnectDestroyed();
}

TEST(TcpConnectionTest, MessageCallbackReceivesData) {
  // 准备一个 loop 加上一对已经连上的 socket
  EventLoop loop;
  TcpPair pair;

  ASSERT_EQ(::write(pair.client_fd(), "hello", 5), 5);

  // 创建连接对象
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // 设置回调
  std::string msg{};
  conn->set_message_callback([&](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
    msg = buf->RetrieveAllAsString();
    loop.Quit();
  });

  // 注册读事件
  conn->ConnectEstablished();
  loop.Loop();

  EXPECT_EQ(msg, "hello");

  conn->ConnectDestroyed();
}

TEST(TcpConnectionTest, PeerCloseInvokesCallbacks) {
  // 准备一个 loop 加上一对已经连上的 socket
  EventLoop loop;
  TcpPair pair;

  // 创建连接
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // 设置回调
  int connection_count = 0;
  int close_count = 0;
  bool last_connected = false;
  conn->set_connection_callback([&](const TcpConnectionPtr& c) {
    ++connection_count;
    last_connected = c->connected();
  });

  conn->set_close_callback([&](const TcpConnectionPtr&) {
    ++close_count;
    loop.Quit();
  });

  // 触发
  conn->ConnectEstablished();
  pair.CloseClient();
  loop.Loop();

  EXPECT_EQ(connection_count, 2);
  EXPECT_FALSE(last_connected);
  EXPECT_EQ(close_count, 1);
  EXPECT_FALSE(conn->connected());

  // 清理
  conn->ConnectDestroyed();
}

TEST(TcpConnectionTest, SendWritesDirectlyWhenIdle) {
  // 准备
  EventLoop loop;
  TcpPair pair;

  // 创建连接
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // 设置回调
  int write_complete_count = 0;
  conn->set_write_complete_callback([&](const TcpConnectionPtr&) { ++write_complete_count; });

  // 触发
  conn->ConnectEstablished();
  conn->Send("hello");

  // 断言
  EXPECT_EQ(write_complete_count, 1);
  EXPECT_EQ(ReadExactly(pair.client_fd(), 5), "hello");

  // 清理
  conn->ConnectDestroyed();
}

TEST(TcpConnectionTest, SendQueuesBehindPendingData) {
  // 准备
  EventLoop loop;
  TcpPair pair;

  // 创建连接
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // 设置回调
  conn->set_write_complete_callback([&](const TcpConnectionPtr&) { loop.Quit(); });

  // 触发
  conn->ConnectEstablished();
  std::string big = MakePattern(4 * 1024 * 1024);
  conn->Send(big);
  conn->Send("tail");

  std::string received;
  std::thread reader([&] { received = ReadExactly(pair.client_fd(), big.size() + 4); });
  loop.Loop();
  reader.join();

  // 断言
  EXPECT_EQ(received.size(), big.size() + 4);
  EXPECT_TRUE(received == big + "tail");

  // 清理
  conn->ConnectDestroyed();
}

TEST(TcpConnectionTest, HighWaterMarkCallbackFiresOnce) {
  constexpr std::size_t kHighWaterMark = 64 * 1024;

  // 准备
  EventLoop loop;
  TcpPair pair;

  // 创建连接
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // 设置回调
  int high_water_count = 0;
  std::size_t last_len = 0;
  conn->set_high_water_mark_callback(
      [&](const TcpConnectionPtr&, std::size_t len) {
        ++high_water_count;
        last_len = len;
      },
      kHighWaterMark);

  // 触发
  conn->ConnectEstablished();
  std::string big = MakePattern(4 * 1024 * 1024);
  conn->Send(big);
  conn->Send(big);

  // 断言
  EXPECT_EQ(high_water_count, 1);
  EXPECT_GE(last_len, kHighWaterMark);

  // 清理
  conn->ConnectDestroyed();
}

TEST(TcpConnectionTest, ShutdownSendsFinAfterPendingData) {
  // 准备
  EventLoop loop;
  TcpPair pair;

  // 创建连接
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // 设置回调
  conn->set_close_callback([&](const TcpConnectionPtr&) { loop.Quit(); });

  // 触发
  conn->ConnectEstablished();
  std::string big = MakePattern(4 * 1024 * 1024);
  conn->Send(big);
  conn->Shutdown();  // state_ = kDisconnecting

  ssize_t eof_result = -1;
  char c;
  std::string received;
  std::thread reader([&] {
    received = ReadUntilEof(pair.client_fd());
    eof_result = ::read(pair.client_fd(), &c, 1);
    pair.CloseClient();
  });
  loop.Loop();
  reader.join();

  // 断言
  EXPECT_EQ(eof_result, 0);
  EXPECT_EQ(received.size(), big.size());
  EXPECT_TRUE(received == big);

  // 清理
  conn->ConnectDestroyed();
}

TEST(TcpConnectionTest, ForceCloseDiscardsPendingData) {
  // 准备
  EventLoop loop;
  TcpPair pair;

  // 创建连接
  TcpConnectionPtr conn = MakeConnection(&loop, pair);

  // 设置回调
  int close_count = 0;
  conn->set_close_callback([&](const TcpConnectionPtr&) { ++close_count; });

  // 触发
  conn->ConnectEstablished();
  std::string big = MakePattern(4 * 1024 * 1024);
  conn->Send(big);
  conn->ForceClose();
  conn->ForceClose();

  // 断言
  EXPECT_EQ(close_count, 1);
  EXPECT_FALSE(conn->connected());

  // 清理
  conn->ConnectDestroyed();
  conn.reset();

  // 断言
  char c;
  std::string received = ReadUntilEof(pair.client_fd());
  ssize_t eof_result = ::read(pair.client_fd(), &c, 1);
  EXPECT_EQ(eof_result, 0);
  EXPECT_LT(received.size(), big.size());
  EXPECT_TRUE(big.compare(0, received.size(), received) == 0);
}

}  // namespace netlib
