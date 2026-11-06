/**
 * @file tcp_connection.cc
 * @brief TcpConnection 的实现
 */

#include "netlib/net/tcp_connection.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>

#include "netlib/base/logging.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/event_loop.h"

namespace netlib {

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int sockfd,
                             const InetAddress& local_addr, const InetAddress& peer_addr)
    : loop_(loop),
      name_(std::move(name)),
      state_(State::kConnecting),
      socket_(sockfd),
      channel_(loop, sockfd),
      local_addr_(local_addr),
      peer_addr_(peer_addr),
      high_water_mark_(kDefaultHighWaterMark) {
  socket_.SetKeepAlive(true);
  channel_.set_read_callback([this](Timestamp receive_time) { HandleRead(receive_time); });
  channel_.set_write_callback([this] { HandleWrite(); });
  channel_.set_close_callback([this] { HandleClose(); });
  channel_.set_error_callback([this] { HandleError(); });
}

TcpConnection::~TcpConnection() {
  assert(state_ == State::kDisconnected);

  LOG_DEBUG << "[" << name_ << "] destroyed, fd=" << socket_.fd();
}

void TcpConnection::ConnectEstablished() {
  loop_->AssertInLoopThread();
  assert(state_ == State::kConnecting);

  set_state(State::kConnected);
  // 把 channel_ 和连接对象绑定一起
  channel_.Tie(shared_from_this());
  // connfd 进入 epoll
  channel_.EnableReading();
  // 通知用户连接已上线
  if (connection_callback_) {
    connection_callback_(shared_from_this());
  }
}

void TcpConnection::ConnectDestroyed() {
  loop_->AssertInLoopThread();

  if (state_ == State::kConnected) {
    set_state(State::kDisconnected);
    channel_.DisableAll();
    // 通知用户连接已下线
    if (connection_callback_) {
      connection_callback_(shared_from_this());
    }
  }

  channel_.Remove();
}

void TcpConnection::Send(std::string_view message) {
  loop_->AssertInLoopThread();

  if (state_ != State::kConnected)
    return;

  SendInLoop(message.data(), message.size());
}

void TcpConnection::Shutdown() {
  loop_->AssertInLoopThread();

  if (state_ == State::kConnected) {
    set_state(State::kDisconnecting);
    ShutdownInLoop();
  }
}

void TcpConnection::ForceClose() {
  loop_->AssertInLoopThread();

  if (state_ == State::kConnected || state_ == State::kDisconnecting) {
    HandleClose();
  }
}

void TcpConnection::HandleRead(Timestamp receive_time) {
  loop_->AssertInLoopThread();

  int saved_errno = 0;
  ssize_t n = input_buffer_.ReadFd(socket_.fd(), &saved_errno);

  if (n > 0) {
    if (message_callback_) {
      message_callback_(shared_from_this(), &input_buffer_, receive_time);
    }
  } else if (n == 0) {
    HandleClose();
  } else {
    errno = saved_errno;
    LOG_SYSERR << "[" << name_ << "] read failed, fd=" << socket_.fd();
    HandleError();
  }
}

void TcpConnection::HandleClose() {
  loop_->AssertInLoopThread();
  assert(state_ == State::kConnected || state_ == State::kDisconnecting);

  set_state(State::kDisconnected);
  channel_.DisableAll();
  TcpConnectionPtr guard_this = shared_from_this();

  // 通知用户连接已下线
  if (connection_callback_) {
    connection_callback_(guard_this);
  }

  // 通知 TcpServer 删除这条连接
  if (close_callback_) {
    close_callback_(guard_this);
  }
}

void TcpConnection::HandleError() {
  loop_->AssertInLoopThread();

  int optval = 0;
  socklen_t optlen = sizeof(optval);
  if (::getsockopt(socket_.fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) == -1) {
    LOG_SYSERR << "[" << name_ << "] getsockopt(SO_ERROR) failed, fd=" << socket_.fd();
  } else {
    errno = optval;
    LOG_SYSERR << "[" << name_ << "] socket error, fd=" << socket_.fd();
  }
}

void TcpConnection::HandleWrite() {
  loop_->AssertInLoopThread();

  if (!channel_.IsWriting()) {
    LOG_TRACE << "[" << name_
              << "] not watching EPOLLOUT, ignore writable event, fd=" << socket_.fd();
    return;
  }

  // 把输出 Buffer 里所有可读的数据写出去
  ssize_t n = ::write(socket_.fd(), output_buffer_.Peek(), output_buffer_.ReadableBytes());

  if (n > 0) {
    output_buffer_.Retrieve(n);
    if (output_buffer_.ReadableBytes() == 0) {
      // 已经没有数据要发送到内核了，不关注写事件
      channel_.DisableWriting();
      if (write_complete_callback_) {
        write_complete_callback_(shared_from_this());
      }

      // 如果状态是 kDisconnecting，在这里半关闭
      if (state_ == State::kDisconnecting) {
        ShutdownInLoop();
      }
    }
  } else {
    LOG_SYSERR << "[" << name_ << "] write failed, fd=" << socket_.fd();
  }
}

void TcpConnection::SendInLoop(const char* data, std::size_t len) {
  loop_->AssertInLoopThread();

  if (state_ == State::kDisconnected) {
    LOG_WARN << "[" << name_ << "] already disconnected, give up sending, len=" << len;
    return;
  }

  ssize_t nwrote = 0;           // 这次写入内核的字节数
  std::size_t remaining = len;  // 还剩多少没有写
  bool fault_error = false;     // 是否出现“连接已经坏了”的错误

  // 不关注写事件 && 输出 Buffer 为空说明这条连接空闲
  if (!channel_.IsWriting() && output_buffer_.ReadableBytes() == 0) {
    nwrote = ::write(socket_.fd(), data, len);
    if (nwrote >= 0) {
      remaining = len - nwrote;
      if (remaining == 0 && write_complete_callback_) {
        write_complete_callback_(shared_from_this());
      }
    } else {
      nwrote = 0;

      int saved_errno = errno;
      if (saved_errno != EWOULDBLOCK) {
        LOG_SYSERR << "[" << name_ << "] write failed, fd=" << socket_.fd();
        if (saved_errno == EPIPE || saved_errno == ECONNRESET) {
          fault_error = true;
        }
      }
    }
  }

  if (!fault_error && remaining > 0) {
    std::size_t old_len = output_buffer_.ReadableBytes();
    if (old_len + remaining >= high_water_mark_ && old_len < high_water_mark_ &&
        high_water_mark_callback_) {
      high_water_mark_callback_(shared_from_this(), old_len + remaining);
    }

    output_buffer_.Append(data + nwrote, remaining);
    if (!channel_.IsWriting()) {
      // 开始关注写事件，等内核有空位时再写
      channel_.EnableWriting();
    }
  }
}

void TcpConnection::ShutdownInLoop() {
  loop_->AssertInLoopThread();

  // 输出 Buffer 中没有数据在排队了
  if (!channel_.IsWriting()) {
    socket_.ShutdownWrite();
  }
}

}  // namespace netlib
