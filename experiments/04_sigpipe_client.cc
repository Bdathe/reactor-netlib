/**
 * @file 04_sigpipe_client.cc
 * @brief 观察 SIGPIPE 的客户端
 *
 * 连接后等服务器退出，再连续 write 两次。
 * 笔记见 experiments/NOTES.md Lab 4（4c）。
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main () {
  // 忽略 SIGPIPE 信号
  signal(SIGPIPE, SIG_IGN);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    fprintf(stderr, "socket() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  // IPv4 地址结构体
  sockaddr_in addr{}; // 所有成员初始化为 0，这里的 sin_zero[8] 也会被置为 0
  addr.sin_port = htons(9877);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
  addr.sin_family = AF_INET;

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
    fprintf(stderr, "connect() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  ssize_t n{};

  sleep(5);

  n = write(fd, "a", 1);
  if (n == -1) {
    fprintf(stderr, "write() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }
  printf("the first write, n = %zd\n", n);

  sleep(1);

  n = write(fd, "b", 1);
  if (n == -1) {
    fprintf(stderr, "write() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }
  printf("the second write, n = %zd\n", n);

  fflush(stdout);

  close(fd);

  return 0;
}
