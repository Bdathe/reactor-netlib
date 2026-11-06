/**
 * @file socket.cc
 * @brief Socket 的实现
 */

#include "netlib/net/socket.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "netlib/base/logging.h"
#include "netlib/net/inet_address.h"

namespace netlib {

Socket::~Socket() {
  if (::close(fd_) == -1) {
    LOG_SYSERR << "close failed, fd=" << fd_;
  }
}

void Socket::BindAddress(const InetAddress& addr) {
  if (::bind(fd_, addr.GetSockAddr(), sizeof(sockaddr_in)) == -1) {
    LOG_SYSFATAL << "bind failed, fd=" << fd_ << ", addr=" << addr.ToIpPort();
  }
}

void Socket::Listen() {
  if (::listen(fd_, SOMAXCONN) == -1) {
    LOG_SYSFATAL << "listen failed, fd=" << fd_;
  }
}

int Socket::Accept(InetAddress* peer) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  int connfd =
      ::accept4(fd_, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);

  if (connfd == -1) {
    return -1;
  }

  *peer = InetAddress(addr);

  return connfd;
}

void Socket::SetReuseAddr(bool on) {
  int optval = on ? 1 : 0;
  // SO_REUSEADDR 是 socket 层的通用选项，层写 SOL_SOCKET
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
    LOG_SYSERR << "setsockopt(SO_REUSEADDR) failed, fd=" << fd_;
  }
}

void Socket::SetKeepAlive(bool on) {
  int optval = on ? 1 : 0;
  // SO_KEEPALIVE 是 socket 层的通用选项，层写 SOL_SOCKET
  if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) == -1) {
    LOG_SYSERR << "setsockopt(SO_KEEPALIVE) failed, fd=" << fd_;
  }
}

void Socket::SetTcpNoDelay(bool on) {
  int optval = on ? 1 : 0;
  // TCP_NODELAY 是 TCP 协议自己的选项，层写 IPPROTO_TCP
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) == -1) {
    LOG_SYSERR << "setsockopt(TCP_NODELAY) failed, fd=" << fd_;
  }
}

void Socket::ShutdownWrite() {
  if (::shutdown(fd_, SHUT_WR) == -1) {
    LOG_SYSERR << "shutdown(SHUT_WR) failed, fd=" << fd_;
  }
}

int CreateNonblockingSocketOrDie() {
  // SOCK_CLOEXEC：进程以后如果 fork + exec 启动别的程序，这个 fd 不会被子程序继承
  // SOCK_STREAM：面向连接、可靠、有序的字节流
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);

  if (fd == -1) {
    LOG_SYSFATAL << "socket failed";
  }

  return fd;
}

InetAddress GetLocalAddr(int sockfd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);

  if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&addr), &len) == -1) {
    LOG_SYSERR << "getsockname failed, fd=" << sockfd;
  }

  return InetAddress(addr);
}

}  // namespace netlib
