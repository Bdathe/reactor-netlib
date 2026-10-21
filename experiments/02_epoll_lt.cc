/**
 * @file 02_epoll_lt.cc
 * @brief epoll LT + 非阻塞 echo 服务器（端口 9877）
 *
 * 单线程同时处理多个客户端；写不完的数据直接丢弃（实验简化）。
 * 笔记见 experiments/NOTES.md Lab 2。
 */

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main () {
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd == -1) {
    fprintf(stderr, "socket() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  // IPv4 地址结构体
  sockaddr_in addr{}; // 所有成员初始化为 0，这里的 sin_zero[8] 也会被置为 0
  addr.sin_port = htons(9877);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_family = AF_INET;

  if (bind(listenfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
    fprintf(stderr, "bind() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  if (listen(listenfd, SOMAXCONN) == -1) {
    fprintf(stderr, "listen() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  // 设置 listenfd 为非阻塞
  int flags = fcntl(listenfd, F_GETFL);
  if (flags == -1) {
    fprintf(stderr, "fcntl(F_GETFL) failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  if (fcntl(listenfd, F_SETFL,flags | O_NONBLOCK) == -1) {
    fprintf(stderr, "fcntl(F_SETFL) failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  int epfd = epoll_create1(0);
  if (epfd == -1) {
    fprintf(stderr, "epoll_create1() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listenfd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) {
    fprintf(stderr, "epoll_ctl() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  constexpr int kMaxEvents = 16;
  epoll_event events[kMaxEvents];
  for (;;) {
    int n = epoll_wait(epfd, events, kMaxEvents, -1);
    if (n == -1) {
      if (errno == EINTR) continue;

      fprintf(stderr, "epoll_wait() failed: %s (errno = %d)\n",
              strerror(errno), errno);
      break;
    }

    for (int i = 0; i < n; ++ i) {
      int fd = events[i].data.fd;
      if (fd == listenfd) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int connfd = accept4(listenfd, reinterpret_cast<sockaddr*>(&peer), &len, SOCK_NONBLOCK);
        if (connfd == -1) {
          if (errno == EAGAIN) continue;
          fprintf(stderr, "accept4() failed: %s (errno = %d)\n",
                  strerror(errno), errno);
          continue;
        }

        printf("client: fd = %d, port = %d\n", connfd, ntohs(peer.sin_port));
        epoll_event cev{};
        cev.events = EPOLLIN;
        cev.data.fd = connfd;
        
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &cev) == -1) {
          fprintf(stderr, "epoll_ctl() failed: %s (errno = %d)\n",
                  strerror(errno), errno);
          close(connfd);
        }
      } else {
        constexpr int kBuffSize = 4096;
        char buf[kBuffSize];
        ssize_t nread = read(fd, buf, kBuffSize);
        if (nread == -1) {
          if (errno == EAGAIN) continue;

          fprintf(stderr, "read() failed: %s (errno = %d)\n",
                  strerror(errno), errno);
          if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            fprintf(stderr, "epoll_ctl() failed: %s (errno = %d)\n",
                    strerror(errno), errno);
          }
          close(fd);
          continue;
        } 

        if (nread == 0) {
          printf("fd = %d closed\n", fd);
          if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            fprintf(stderr, "epoll_ctl() failed: %s (errno = %d)\n",
                    strerror(errno), errno);
          }
          close(fd);
          continue;
        }
        
        ssize_t nwrite = write(fd, buf, nread);
        if (nwrite >= 0 && nwrite < nread) {
          printf("partial drop %zd bytes\n", nread - nwrite);
        } else if (nwrite == -1) {
          if (errno == EAGAIN) {
            printf("eagain drop %zd bytes\n", nread);
          } else {
            fprintf(stderr, "write() failed: %s (errno = %d)\n",
                    strerror(errno), errno);
            if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
              fprintf(stderr, "epoll_ctl() failed: %s (errno = %d)\n",
                      strerror(errno), errno);
            }
            close(fd);
            continue;
          }
        }
      }
    }
  }

  close(epfd);
  close(listenfd);
   
  return 0;
}
