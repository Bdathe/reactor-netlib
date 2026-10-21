/**
 * @file 01_blocking_echo.cc
 * @brief 阻塞 IO 的单线程 echo 服务器（端口 9877）
 *
 * 一次只服务一个客户端，断开后才 accept 下一个。
 * 笔记见 experiments/NOTES.md Lab 1。
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>
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

  while (true) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);

    int connfd = accept(listenfd, reinterpret_cast<sockaddr*>(&peer), &len);
    if (connfd == -1) {
      fprintf(stderr, "accept() failed: %s (errno = %d)\n",
              strerror(errno), errno);
      continue;
    }

    printf("client: fd = %d, port = %d (connection)\n", connfd, ntohs(peer.sin_port));

    char buf[4096];
    while (true) {
      ssize_t n = read(connfd, buf, sizeof(buf));
      if (n == 0) {
        printf("peer closed\n");
        break;
      }

      if (n == -1) {
        fprintf(stderr, "read() failed: %s (errno = %d)\n",
                strerror(errno), errno);
        break;
      }

      printf("size of bytes read = %zd\n", n);

      ssize_t w = write(connfd, buf, n);
      if (w == -1) {
        fprintf(stderr, "write() failed: %s (errno = %d)\n",
                strerror(errno), errno);
        break;
      }
    }

    close(connfd);

    printf("client: fd = %d, port = %d (closed)\n", connfd, ntohs(peer.sin_port));
  }

  close(listenfd);

  return 0;
}
