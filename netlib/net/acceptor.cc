/**
 * @file acceptor.cc
 * @brief Acceptor 的实现
 */

#include "netlib/net/acceptor.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "netlib/base/logging.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/inet_address.h"
#include "netlib/net/poller.h"

namespace netlib {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_addr)
    : loop_(loop),
      accept_socket_(CreateNonblockingSocketOrDie()),
      accept_channel_(loop, accept_socket_.fd()),
      listening_(false),
      idle_fd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
  if (idle_fd_ == -1) {
    LOG_SYSFATAL << "open(/dev/null) failed";
  }

  accept_socket_.SetReuseAddr(true);
  accept_socket_.BindAddress(listen_addr);

  accept_channel_.set_read_callback([this](Timestamp receive_time) { HandleRead(receive_time); });
}

Acceptor::~Acceptor() {
  accept_channel_.DisableAll();  // 不再关注 listenfd 上的任何事件
  accept_channel_.Remove();      // 把 Channel 从 Poller 里移除
  if (idle_fd_ >= 0) {
    if (::close(idle_fd_) == -1) {
      LOG_SYSERR << "close(idle_fd_) failed";
    }
  }
}

void Acceptor::Listen() {
  loop_->AssertInLoopThread();
  listening_ = true;
  accept_socket_.Listen();
  accept_channel_.EnableReading();
}

void Acceptor::HandleRead(Timestamp /*receive_time*/) {
  // 1. 关闭 idle_fd_，腾出一个 fd 的名额
  // 2. 用这个名额 accept 这个连接，把它从全连接队列里取出来
  // 3. 立刻关闭这个连接。对客户端来说，它会收到 FIN，知道自己被拒绝了，而不是一直挂着
  // 4. 重新打开 /dev/null，把名额再占回来，留给下一次用

  loop_->AssertInLoopThread();
  InetAddress peer_addr(0);
  int connfd = accept_socket_.Accept(&peer_addr);

  if (connfd < 0) {
    int saved_errno = errno;

    LOG_SYSERR << "Accept failed";

    if (saved_errno == EMFILE) {
      if (idle_fd_ >= 0) {
        if (::close(idle_fd_) == -1) {
          LOG_SYSERR << "close(idle_fd_) failed";
        }
      }

      int fd = ::accept(accept_socket_.fd(), nullptr, nullptr);
      if (fd >= 0) {
        if (::close(fd) == -1) {
          LOG_SYSERR << "close(accepted fd) failed";
        }
      } else {
        LOG_SYSERR << "accept failed";
      }

      idle_fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (idle_fd_ == -1) {
        LOG_SYSERR << "open(/dev/null) failed";
      }
    }
  } else {
    if (new_connection_callback_) {
      new_connection_callback_(connfd, peer_addr);
    } else {
      if (::close(connfd) == -1) {
        LOG_SYSERR << "close(connfd) failed";
      }
    }
  }
}

}  // namespace netlib
