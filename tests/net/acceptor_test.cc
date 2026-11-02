#include "netlib/net/acceptor.h"

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <optional>

#include "netlib/base/noncopyable.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/channel.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/inet_address.h"

namespace netlib {

namespace {

// 测试监听的端口；监听 socket 设了 SO_REUSEADDR，各个测试可以依次复用
constexpr uint16_t kTestPort = 19527;

// 测试用的客户端：构造时用阻塞 connect 连上 127.0.0.1:port
// 握手由内核完成，不需要服务端先 accept，所以 connect 会立刻返回；
// 之后连接留在全连接队列里，listenfd 变为可读
class Client : Noncopyable {
public:
  explicit Client(uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    EXPECT_GE(fd_, 0);
    std::optional<InetAddress> server = InetAddress::FromIpPort("127.0.0.1", port);
    EXPECT_TRUE(server.has_value());
    EXPECT_EQ(::connect(fd_, server->GetSockAddr(), sizeof(sockaddr_in)), 0);
  }
  ~Client() { ::close(fd_); }

  int fd() const { return fd_; }

private:
  const int fd_;
};

// 测试用的 eventfd：Notify() 后变为可读，Drain() 读出计数并恢复为不可读
class EventFd : Noncopyable {
public:
  EventFd() : fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
  ~EventFd() { ::close(fd_); }

  int fd() const { return fd_; }

  void Notify() {
    uint64_t one = 1;
    ASSERT_EQ(::write(fd_, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
  }

  uint64_t Drain() {
    uint64_t value = 0;
    return ::read(fd_, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)) ? value : 0;
  }

private:
  const int fd_;
};

TEST(AcceptorTest, ListenSetsListening) {
  EventLoop loop;
  Acceptor acceptor(&loop, InetAddress(kTestPort));
  
  ASSERT_FALSE(acceptor.listening());
  acceptor.Listen();
  ASSERT_TRUE(acceptor.listening());
}

TEST(AcceptorTest, NewConnectionCallbackReceivesConnfd) {
  EventLoop loop;
  Acceptor acceptor(&loop, InetAddress(kTestPort));

  int connfd = -1;
  std::string peer{};
  acceptor.set_new_connection_callback([&](int sockfd, const InetAddress& peer_addr){
    connfd = sockfd;
    peer = peer_addr.ToIpPort();
    loop.Quit(); 
  });

  acceptor.Listen();
  Client client(kTestPort);
  loop.Loop();

  ASSERT_GE(connfd, 0);
  EXPECT_EQ(peer.rfind("127.0.0.1:", 0), 0);
  ::close(connfd);
}

TEST(AcceptorTest, ClosesConnfdWithoutCallback) {
  EventLoop loop;
  Acceptor acceptor(&loop, InetAddress(kTestPort));

  acceptor.Listen();
  Client client(kTestPort);

  // 唯一的作用就是让 loop 停下来
  EventFd efd;
  Channel channel(&loop, efd.fd());

  channel.set_read_callback([&](Timestamp) {
    efd.Drain();
    loop.Quit();
  });

  channel.EnableReading();
  efd.Notify();

  loop.Loop();

  // loop.Loop() 的第一轮
  // epoll_wait 立刻返回 2 个活跃 Channel（顺序不确定）：
  //   ├─ listenfd 的 Channel
  //   │    → Acceptor::HandleRead
  //   │    → accept 得到 connfd
  //   │    → 没有设置 NewConnectionCallback → close(connfd)
  //   │    → 内核向客户端发送 FIN
  //   │
  //   └─ eventfd 的 Channel
  //        → lambda：Drain()，然后 Quit()，把 quit_ 设为 true
  //
  // 这一轮结束后，回到 while (!quit_) 检查 → quit_ 为 true → Loop() 返回

  char buf;
  EXPECT_EQ(::read(client.fd(), &buf, 1), 0);
  channel.DisableAll();
  channel.Remove();
}

}  // namespace

}  // namespace netlib
