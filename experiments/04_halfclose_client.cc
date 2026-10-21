/**
 * @file 04_halfclose_client.cc
 * @brief 对比 shutdown(SHUT_WR) 和 close 的客户端
 *
 * 用法：04_halfclose_client <shutdown/close>
 * 笔记见 experiments/NOTES.md Lab 4（4d）。
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main (int argc, char* argv[]) {
  if (argc != 2 ||
      (strcmp(argv[1], "shutdown") != 0 && 
       strcmp(argv[1], "close") != 0)) {
    printf("usage: %s <shutdown/close>\n", argv[0]);
    
    return -1;
  }

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
  n = write(fd, "hello\n", 6);
  if (n == -1) {
    fprintf(stderr, "write() failed: %s (errno = %d)\n",
            strerror(errno), errno);
    exit(EXIT_FAILURE);
  }

  if (strcmp(argv[1], "shutdown") == 0) {
    if (shutdown(fd, SHUT_WR) == -1) {
      fprintf(stderr, "shutdown() failed: %s (errno = %d)\n",
              strerror(errno), errno);
      exit(EXIT_FAILURE);
    }
  } else if (strcmp(argv[1], "close") == 0) {
    close(fd);
  }

  for (;;) {
    char buf[4096];
    n = read(fd, buf, sizeof(buf));
    if (n == -1) {
      fprintf(stderr, "read() failed: %s (errno = %d)\n",
              strerror(errno), errno);
      exit(EXIT_FAILURE);
    }

    if (n == 0) {
      printf("server closed\n");
      break;
    }

    printf("%.*s\n", static_cast<int>(n), buf);
  }

  return 0;
}
