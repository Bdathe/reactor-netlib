// echo 压测客户端（AI 编写的测试工具，不属于库代码）
//
// N 条连接并发 ping-pong：每条连接发一条消息 → 等完整的回显 → 逐字节校验 → 再发下一条
// 直接用 epoll + 非阻塞 socket 实现，不依赖 netlib，避免"用自己测自己"
//
// 用法：echo_client <ip> <port> <连接数> <秒数> [消息字节数，默认 4096]
// 退出码：所有连接都建立、没有校验失败、没有被服务器意外断开时为 0，否则为 1

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Conn {
  int fd = -1;
  bool connected = false;
  bool closed = false;
  bool want_write = false;  // 当前是否关注 EPOLLOUT
  uint64_t seq = 0;         // 第几条消息
  std::string out;          // 本轮要发的消息
  std::size_t sent = 0;     // 本轮已发出的字节数
  std::string in;           // 本轮已收到的字节
};

struct Stats {
  std::size_t established = 0;
  std::size_t connect_failed = 0;
  std::size_t unexpected_close = 0;  // 压测期间被服务器关闭或出错
  std::size_t mismatch = 0;          // 回显内容和发出的不一致
  uint64_t messages = 0;             // 校验通过的往返次数
  uint64_t bytes = 0;                // 校验通过的字节数（单向）
};

// 每个字节都和（连接号, 序号, 位置）相关：错位、串到别的连接、重复发送都能被发现
std::string MakeMessage(std::size_t conn_id, uint64_t seq, std::size_t size) {
  std::string s(size, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    s[i] = static_cast<char>('a' + (conn_id * 7 + seq * 13 + i) % 26);
  }
  return s;
}

void SetInterest(int epfd, std::size_t id, Conn& c, bool want_write) {
  epoll_event ev{};
  ev.events = EPOLLIN | (want_write ? EPOLLOUT : 0u);
  ev.data.u64 = id;
  ::epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev);
  c.want_write = want_write;
}

void CloseConn(int epfd, Conn& c) {
  if (!c.closed) {
    ::epoll_ctl(epfd, EPOLL_CTL_DEL, c.fd, nullptr);
    ::close(c.fd);
    c.closed = true;
  }
}

// 尽量把本轮消息写完；写不完就关注 EPOLLOUT，写完就取消
bool TryWrite(int epfd, std::size_t id, Conn& c) {
  while (c.sent < c.out.size()) {
    ssize_t n = ::write(c.fd, c.out.data() + c.sent, c.out.size() - c.sent);
    if (n > 0) {
      c.sent += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EAGAIN) {
      if (!c.want_write) {
        SetInterest(epfd, id, c, true);
      }
      return true;
    } else {
      return false;
    }
  }
  if (c.want_write) {
    SetInterest(epfd, id, c, false);
  }
  return true;
}

void StartRound(int epfd, std::size_t id, Conn& c, std::size_t msg_size, Stats& st) {
  c.out = MakeMessage(id, c.seq, msg_size);
  c.sent = 0;
  c.in.clear();
  if (!TryWrite(epfd, id, c)) {
    ++st.unexpected_close;
    CloseConn(epfd, c);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <ip> <port> <conns> <seconds> [msg_size=4096]\n", argv[0]);
    return 1;
  }
  const char* ip = argv[1];
  const int port = std::atoi(argv[2]);
  const std::size_t nconns = std::strtoul(argv[3], nullptr, 10);
  const int seconds = std::atoi(argv[4]);
  const std::size_t msg_size = argc > 5 ? std::strtoul(argv[5], nullptr, 10) : 4096;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    std::fprintf(stderr, "invalid ip: %s\n", ip);
    return 1;
  }

  int epfd = ::epoll_create1(EPOLL_CLOEXEC);
  std::vector<Conn> conns(nconns);
  Stats st;

  // 1. 发起全部连接（非阻塞 connect，EPOLLOUT 表示连接完成或失败）
  for (std::size_t id = 0; id < nconns; ++id) {
    Conn& c = conns[id];
    c.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c.fd < 0) {
      std::perror("socket");
      return 1;
    }
    int ret = ::connect(c.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
      ++st.connect_failed;
      ::close(c.fd);
      c.closed = true;
      continue;
    }
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.u64 = id;
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, c.fd, &ev);
    c.want_write = true;
  }

  // 2. 事件循环，直到时间到
  const auto start = Clock::now();
  const auto deadline = start + std::chrono::seconds(seconds);
  auto next_report = start + std::chrono::seconds(10);
  std::vector<epoll_event> events(1024);
  char buf[64 * 1024];

  while (Clock::now() < deadline) {
    int n = ::epoll_wait(epfd, events.data(), static_cast<int>(events.size()), 100);
    for (int i = 0; i < n; ++i) {
      const std::size_t id = events[i].data.u64;
      Conn& c = conns[id];
      if (c.closed) {
        continue;
      }

      // 连接完成（或失败）
      if (!c.connected) {
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
          ++st.connect_failed;
          CloseConn(epfd, c);
          continue;
        }
        c.connected = true;
        ++st.established;
        SetInterest(epfd, id, c, false);
        StartRound(epfd, id, c, msg_size, st);
        continue;
      }

      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        ++st.unexpected_close;
        CloseConn(epfd, c);
        continue;
      }

      if (events[i].events & EPOLLOUT) {
        if (!TryWrite(epfd, id, c)) {
          ++st.unexpected_close;
          CloseConn(epfd, c);
          continue;
        }
      }

      if (events[i].events & EPOLLIN) {
        ssize_t got = ::read(c.fd, buf, sizeof(buf));
        if (got == 0 || (got < 0 && errno != EAGAIN)) {
          ++st.unexpected_close;  // 压测期间服务器不应该主动断开
          CloseConn(epfd, c);
          continue;
        }
        if (got < 0) {
          continue;
        }
        c.in.append(buf, static_cast<std::size_t>(got));
        if (c.in.size() > c.sent) {
          ++st.mismatch;  // 收到的比发出的还多：服务器多发了数据
          CloseConn(epfd, c);
          continue;
        }
        if (c.in.size() == c.out.size()) {
          if (c.in != c.out) {
            ++st.mismatch;
            CloseConn(epfd, c);
            continue;
          }
          ++st.messages;
          st.bytes += c.out.size();
          ++c.seq;
          StartRound(epfd, id, c, msg_size, st);
        }
      }
    }

    if (Clock::now() >= next_report) {
      double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
      std::printf("[%5.1fs] established=%zu messages=%llu mismatch=%zu unexpected_close=%zu\n",
                  elapsed, st.established, static_cast<unsigned long long>(st.messages),
                  st.mismatch, st.unexpected_close);
      std::fflush(stdout);
      next_report += std::chrono::seconds(10);
    }
  }

  // 3. 客户端主动关闭全部连接（服务器应该把它们全部清理掉）
  for (Conn& c : conns) {
    CloseConn(epfd, c);
  }
  ::close(epfd);

  double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("\n");
  std::printf("连接数        : %zu / %zu（失败 %zu）\n", st.established, nconns, st.connect_failed);
  std::printf("往返次数      : %llu（%.0f 次/秒）\n", static_cast<unsigned long long>(st.messages),
              static_cast<double>(st.messages) / elapsed);
  std::printf("校验通过字节  : %.1f MiB（%.1f MiB/s，单向）\n",
              static_cast<double>(st.bytes) / (1 << 20),
              static_cast<double>(st.bytes) / (1 << 20) / elapsed);
  std::printf("校验失败      : %zu\n", st.mismatch);
  std::printf("意外断开      : %zu\n", st.unexpected_close);

  bool ok = st.established == nconns && st.mismatch == 0 && st.unexpected_close == 0;
  std::printf("结果          : %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
