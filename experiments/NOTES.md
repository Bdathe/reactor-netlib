# M1 实验笔记

## 概述

M1 的实验程序是 echo 服务器：监听 9877 端口，客户端（用 `nc` 测试）发来什么，就把什么原样发回去。

- 实验1
  - 做法：单线程 + 阻塞 IO
  - 解决：能服务一个客户端
  - 问题：线程会阻塞在一个 fd 上，同时连接的第二个客户端没人处理
- 实验2（2b ~ 2d）
  - 做法：epoll LT 同时等待多个 fd
  - 解决：多个客户端可以同时回显
  - 问题：阻塞的 `write` 会被只发不收的慢客户端卡住，导致所有客户端一起没响应
- 实验2（2e）
  - 做法：fd 改成非阻塞，处理 `EAGAIN`
  - 解决：慢客户端不再拖累其他客户端
  - 问题：写不完的数据只能丢弃，以及服务器空转
- 实验3
  - 做法：epoll ET，每次通知都循环 `read` / `accept` 到 `EAGAIN`
  - 解决：效果与 LT 版本相同，并验证了 ET 只处理一次时数据和连接都会卡住
  - 问题：`accept` 循环遇到 `EMFILE` 等错误提前退出时，剩下的连接不会再有通知；大流量连接可能独占线程
- 实验4
  - 做法：五个小实验，分别观察 TIME_WAIT、SIGPIPE、半关闭、EMFILE，并用 strace 看一次完整连接
  - 解决：`SO_REUSEADDR` 让服务器能立刻重启；忽略 SIGPIPE 后对断开的连接写入只返回 `EPIPE`
  - 问题：`accept` 遇到 `EMFILE` 时 LT 会忙等（M2 用预留空闲 fd 解决，D10）

---

## Lab 1：阻塞 echo

### 简介

监听 9877 端口，接受一个客户端，把它发来的数据原样发回，当这个客户端断开后，再接受下一个。

```cpp
listenfd = socket();
bind(listenfd, 9877);
listen(listenfd);
for (;;) {
  connfd = accept(listenfd); // 取出一个客户端
  while ((n = read(connfd, buf)) > 0) { // 读到对端关闭为止
    write(connfd, buf, n); // 原样发回
  }
  close(connfd); // 再去接受下一个
}
```

- 源码：`experiments/01_blocking_echo.cc`
- 产物：`build/experiments/01_blocking_echo`
- 编译：

  ```bash
  g++ -std=c++17 -Wall -Wextra -g experiments/01_blocking_echo.cc -o build/experiments/01_blocking_echo
  ```

### 代码演进

#### 1a ~ 1c

只建监听 socket：

```cpp
listenfd = socket(AF_INET, SOCK_STREAM, 0);
addr.sin_port = htons(9877);
addr.sin_addr.s_addr = htonl(INADDR_ANY);
bind(listenfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
listen(listenfd, SOMAXCONN);
pause();
```

操作：

1. A：启动服务器 `./build/experiments/01_blocking_echo`，看打印的 `listenfd`
2. B：`ss -tlnp | grep 9877`，看 `LISTEN` 行的 `Recv-Q` / `Send-Q`
3. B：再运行一个服务器，看第二个实例的报错
4. 临时把 `htons(9877)` 改成 `9877`，重新编译运行，用 `ss -tlnp | grep 01_blocking` 看实际端口


- `listenfd = 3`
  - 原因：0/1/2 已被占用，内核分配编号最小的空闲 fd
- 同时运行第二个实例，`bind` 报 `EADDRINUSE`
  - 原因：端口冲突在 `bind` 时就检查，不必等到 `listen`
- `ss -tlnp` 显示 `LISTEN`，`Send-Q 4096`
  - 原因：LISTEN 行的 `Send-Q` 是 backlog，被 `somaxconn` 截断为 4096
- 漏写 `htons` 时，实际监听的是 38182
  - 原因：9877 = `0x2695`，小端存储为 `95 26`，内核按大端读成 `0x9526`

#### 1d

accept 一个连接：

```cpp
connfd = accept(listenfd, reinterpret_cast<sockaddr*>(&peer), &len);
printf(connfd, ntohs(peer.sin_port));
```

操作：

1. A：启动服务器，观察程序停在哪里
2. B：`nc 127.0.0.1 9877`
3. C：`ss -tnp | grep 9877`，对比两侧的端口


- 没有客户端时，程序停在 `accept`
  - 原因：全连接队列为空，`accept` 阻塞
- `nc` 连接后打印 `connfd = 4`
  - 原因：内核分配编号最小的空闲 fd
- 服务器侧连接的本地端口也是 9877
  - 原因：`connfd` 与 `listenfd` 共用端口，内核靠四元组区分连接

#### 1e

读一次、写一次：

```cpp
n = read(connfd, buf, sizeof(buf));
write(connfd, buf, n);
```

操作：

1. A：启动服务器
2. B：`nc 127.0.0.1 9877`，输入 `hello` 回车，再输入 `world` 回车
3. C：`ss -tn | grep 9877`，看服务器一侧的 `Recv-Q`


- 输入 `hello`，`n = 6`，有回显
  - 原因：`n` 包含换行符
- 再输入 `world`，没有回显
  - 原因：只 `read` 了一次，`world\n` 留在服务器侧的接收缓冲区

#### 1f

循环读写，直到对端关闭：

```cpp
while (true) {
  n = read(connfd, buf, sizeof(buf));
  if (n == 0 || n == -1) break; // 对端关闭 / 出错
  if (write(connfd, buf, n) == -1) break;
}
close(connfd);
```

操作：

1. A：启动服务器
2. B：`nc 127.0.0.1 9877`，连续输入几行
3. B：按 Ctrl+C 结束 `nc`，看服务器的输出


- 多行输入都有回显
- `nc` 按 Ctrl+C 后，服务器打印 `peer closed`
  - 原因：内核替 `nc` 关闭 socket 并发出 FIN，`read` 返回 0

#### 1g

外层 accept 循环：

```cpp
while (true) {
  connfd = accept(listenfd, ...); // 失败时 continue
  while (true) { read → write } // 服务这一个客户端，直到它关闭
  close(connfd);
}
```

操作：

1. A：启动服务器
2. B：`nc 127.0.0.1 9877`，输入 `aaa`
3. C：`nc -v 127.0.0.1 9877`，输入 `bbb`
4. D：`ss -tlnp | grep 9877`、`ss -tnp | grep 9877`、`sudo strace -p $(pgrep -f 01_blocking_echo)`
5. B：按 Ctrl+C 结束第一个 `nc`，看 C 的变化


- 客户端依次连接都正常
  - 原因：一个客户端断开后，回到外层循环 `accept` 下一个
- 同时开两个 `nc`，第二个连上了但没有回显
  - 原因：服务器阻塞在 `read(4)` 等第一个客户端，回不到 `accept`；握手由内核完成，所以客户端看来"连上了"
- 关掉第一个后，第二个立刻收到回显
  - 原因：回到 `accept` 取出第二个连接，`read` 读到早已在缓冲区里的数据

证据：

```bash
# 服务器阻塞在第一个连接的 read 上（sudo strace -p <pid>）
read(4,

# 全连接队列里有 1 个连接在等 accept（ss -tlnp）
LISTEN 1      4096   0.0.0.0:9877   0.0.0.0:*

# 服务器侧第二个连接已 ESTAB，Recv-Q 存着 "bbb\n"，但没有对应的 fd（ss -tnp）
ESTAB  4      0      127.0.0.1:9877 127.0.0.1:<C 的端口>
```

> 遗留问题：一个线程同一时刻只能阻塞在一个 fd 上

### 要点

**1. TCP 与系统调用**

- **三次握手**：`listen` 之后由内核自己完成；`accept` 只从全连接队列取连接、分配 fd，不参与握手，也不读数据。
- **listen**：只调一次，它是切换到 LISTEN 状态的开关；循环调用的是 `accept`。
- **read 返回值**：`>0` 字节数 / `0` 对端 FIN / `-1` 出错，三种情况要互斥处理。
- **FIN 由内核发**：进程被杀时内核代发 FIN，对端能感知；断电、拔网线则不能。

**2. 地址与端口**

- **字节序**：写端口用 `htons`，读端口用 `ntohs`。
- **四元组**：内核靠源 IP、源端口、目的 IP、目的端口区分连接。
- **值-结果参数**：`accept` 的 `addrlen` 每次调用前都要重新设置，否则变长地址可能被静默截断。

**3. 错误处理**

- **错误分级**：单个连接的错误用 `break` / `continue`，不要 `exit` 整个服务器。
- **errno**：出错后立刻读，之后的调用可能覆盖它。

**4. 工具**

| 命令 | 用途 |
|---|---|
| `ss -tlnp` | LISTEN 行的 `Recv-Q` 是等待 `accept` 的连接数，`Send-Q` 是 backlog 上限 |
| `ss -tnp` | 本机测试会显示连接两侧，看本地端口是 9877 的那行；`-p` 显示 fd |
| `sudo strace -p <pid>` | 看进程阻塞在哪个系统调用上 |

---

## 讨论：一个线程如何服务多个客户端

实验 1 服务器卡住的根本原因是：一个线程阻塞在一个 fd 上。解决思路有四种：

| 方案 | 做法 | 代价 / 特点 |
|---|---|---|
| ① 多进程 | 每个连接 `fork` 一个子进程 | 创建、切换开销最大；`fork` 用写时复制；优点是隔离 |
| ② 多线程 | 每个连接一个线程 | 大量线程阻塞空等，调度和上下文切换开销大；8MB 栈只是虚拟地址预留 |
| ③ 非阻塞 + 轮询 | 循环对每个 fd 调 `read`，`EAGAIN` 就试下一个 | **轮询**：CPU 忙等，每轮 N 次系统调用，大多只返回 `EAGAIN` |
| ④ IO 多路复用 | 线程只阻塞在 `epoll_wait`，谁就绪处理谁 | **事件驱动**：数据到达 $\to$ 网卡中断 $\to$ 协议栈 $\to$ 回调把 fd 放入就绪列表并唤醒线程 |

---

## Lab 2：epoll LT + 非阻塞 echo

### 简介

同样是 echo，但单线程用 epoll（LT）同时盯住 `listenfd` 和所有 `connfd`，谁发来数据就先回谁，最后把 fd 改成非阻塞，用一个只发不收的慢客户端检验效果。

```cpp
listenfd = socket();
bind(listenfd, 9877);
listen(listenfd);
set_nonblocking(listenfd);
epfd = epoll_create1();
epoll_ctl(epfd, ADD, listenfd);
for (;;) {
  n = epoll_wait(epfd, events); // 阻塞到有 fd 就绪
  for (每个就绪的 fd) {
    if (fd == listenfd) { // 有新连接
      connfd = accept4(listenfd, SOCK_NONBLOCK);
      epoll_ctl(epfd, ADD, connfd);
    } else { // 某个客户端发来了数据
      nread = read(fd, buf); // 返回 0 或出错 → DEL + close
      write(fd, buf, nread); // 写不完的部分丢弃
    }
  }
}
```

- 源码：`experiments/02_epoll_lt.cc`
- 测试工具：`experiments/tools/no_read_client.py`（AI 编写，只发不收的客户端）
- 产物：`build/experiments/02_epoll_lt`
- 编译：

  ```bash
  g++ -std=c++17 -Wall -Wextra -Wshadow -Werror -g experiments/02_epoll_lt.cc -o build/experiments/02_epoll_lt
  ```

### 代码演进

#### 2b

只等待、不处理：

```cpp
epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev); // ev.events = EPOLLIN
while (true) {
  n = epoll_wait(epfd, events, kMaxEvents, -1);
  for (i = 0; i < n; ++i) printf(events[i].data.fd, events[i].events); // 不 accept
}
```

操作：

1. A：启动服务器，先观察没有连接时的输出
2. B：`nc 127.0.0.1 9877`，只连接、不输入
3. ET 对照：注册时改成 `EPOLLIN | EPOLLET`，重新编译运行，用两个 `nc` 各连一次，再执行 `ss -tln | grep 9877`


- 没有连接时不打印
  - 原因：`epoll_wait` 阻塞，不占 CPU
- 连接一次后一直刷屏 `fd = 3, events = 1`
  - 原因：LT 下没人 `accept`，队列一直非空（`Recv-Q` 一直是 1），每次 `epoll_wait` 都报告
- 改成 `EPOLLIN | EPOLLET` 后，每个新连接只通知一次；`Recv-Q = 2` 也不再通知
  - 原因：ET 只在状态变化时报告，没处理完就不会再提醒

#### 2c

accept 并注册 connfd，但不读数据：

```cpp
if (fd == listenfd) {
  connfd = accept(listenfd, ...);
  epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &cev); // cev.data.fd = connfd
} else {
  printf("fd %d readable\n", fd); // 不 read
}
```

操作：

1. A：启动服务器
2. B：`nc 127.0.0.1 9877`，先不输入，观察服务器
3. B：输入 `hello` 回车，再观察服务器


- 只连接、不输入时不刷屏
  - 原因：`accept` 取空了队列，`connfd` 也没有数据
- 输入 `hello` 后一直刷屏 `fd 5 readable`
  - 原因：`connfd` 的接收缓冲区有数据没人读，LT 一直报告

#### 2d

读写数据，fd 仍是阻塞的：

```cpp
nread = read(fd, buf, sizeof(buf)); // 每次事件只读一次
if (nread == 0 || nread == -1) { DEL + close; continue; }
write(fd, buf, nread); // 失败也 DEL + close
```

操作：

1. A：启动服务器
2. B、C、D：各开一个 `nc 127.0.0.1 9877`，轮流输入
3. 逐个按 Ctrl+C 结束 `nc`，最后执行 `ls -l /proc/$(pgrep -f 02_epoll_lt)/fd`

慢客户端：

1. A：启动服务器
2. B：`nc 127.0.0.1 9877`，输入 `aaa`
3. C：`python3 experiments/tools/no_read_client.py`，等它打印"发送卡住了"
4. B：输入 `bbb`
5. D：在 C 卡住期间执行 `sudo strace -p $(pgrep -f 02_epoll_lt)` 和 `ss -tn | grep 9877`
6. C：按 Ctrl+C 结束，看 A 和 B 的变化


- 3 个 `nc` 同时输入都能立刻回显，关闭后不刷屏
  - 原因：谁就绪处理谁；对端关闭时 `DEL` + `close`
- 加入只发不收的 C 后，所有客户端都没有回显
  - 原因：C 不读 → 窗口为 0 → 服务器发送缓冲区满 → 阻塞的 `write` 睡眠，线程回不到 `epoll_wait`
- C 结束后恢复，服务器打印 `ECONNRESET`
  - 原因：C 退出时接收缓冲区有未读数据，内核发 RST 而不是 FIN

证据：

```bash
# 服务器卡在给 C（fd 6）的 write 上（sudo strace -p <pid>）
write(6,

# 两个方向都堵死（ss -tn，C 的本地端口是 46714）
ESTAB  128512  2774528  127.0.0.1:46714  127.0.0.1:9877   # C：回显没人读，窗口为 0
ESTAB  37888   4131840  127.0.0.1:9877   127.0.0.1:46714  # 服务器：约 4MB 发不出去，也不再读 C
```

> 遗留问题：阻塞的 `write` 会卡住唯一的线程

#### 2e

改成非阻塞：

```cpp
fcntl(listenfd, F_SETFL, fcntl(listenfd, F_GETFL) | O_NONBLOCK);
connfd = accept4(listenfd, ..., SOCK_NONBLOCK);
// accept4 / read 遇到 EAGAIN：跳过，不当作错误
nwrite = write(fd, buf, nread);
// 0 <= nwrite < nread → partial drop；-1 且 EAGAIN → eagain drop（实验简化：直接丢弃）
```

操作：

1. A：`./build/experiments/02_epoll_lt > build/experiments/server.log`（输出太多，重定向到文件）
2. B：`nc 127.0.0.1 9877`，输入 `aaa`
3. C：`python3 experiments/tools/no_read_client.py`
4. B：C 运行几秒后输入 `bbb`
5. D：`top -p $(pgrep -f 02_epoll_lt)` 看 CPU 占用
6. C：运行约 5 秒后按 Ctrl+C，再用 `grep -c` 统计日志


- 同样加入 C，B 照样立刻回显
  - 原因：`write` 立即返回 `EAGAIN`，线程不会被卡住
- C 一直能发，服务器 CPU 占用很高
  - 原因：C → 服务器方向是通的，服务器一直"读 → 写不进 → 丢弃"空转
- `partial drop` 1 次，`eagain drop` 754 万次
  - 原因：发送缓冲区只会从"没满"变成"满"一次；C 不读就没有 ACK，之后全是 `EAGAIN`

证据：

```bash
# 统计 server.log 中的丢弃次数
grep -c 'partial drop' build/experiments/server.log    # 1
grep -c 'eagain drop'  build/experiments/server.log    # 7546966
```

> 遗留问题（M2）：没写完的数据要放进输出缓冲区并关注 `EPOLLOUT`，写完再取消关注；堆积太多时暂停读取（D7 背压）

### 要点

**1. epoll**

| 调用 | 作用 |
|---|---|
| `epoll_create1(0)` | 创建 epoll 实例：兴趣列表（红黑树）+ 就绪列表 + 等待队列 |
| `epoll_ctl(epfd, op, fd, &ev)` | `ADD` / `MOD` / `DEL` 兴趣列表，登记一次长期有效 |
| `epoll_wait(epfd, events, max, timeout)` | 阻塞到有 fd 就绪，只返回就绪的事件 |

- **与 select/poll 的区别**：后者每次调用都要传入全部 fd、内核逐个检查、返回后再遍历；epoll 靠回调维护就绪列表。
- **data**：留给自己的"便条"，内核原样返回；事件里没有 fd 字段，写错就会操作错误的 fd。
- **EPOLLERR / EPOLLHUP**：不用设置也总会报告（fdinfo 里 `events: 19`）。
- **清理顺序**：先 `EPOLL_CTL_DEL`，再 `close`。
- **LT / ET**：LT 只要就绪就每次报告；ET 只在状态变化时报告一次，必须一次处理到 `EAGAIN`。
- **就绪通知**：epoll 不替你做 IO（Reactor 模型），通知只是那一刻的快照。

**2. 非阻塞**

| 调用 | 阻塞 fd 办不成时 | 非阻塞 fd 办不成时 |
|---|---|---|
| `accept` | 睡到队列非空 | `-1` + `EAGAIN` |
| `read` | 睡到有数据 | `-1` + `EAGAIN` |
| `write` | 睡到全部写完 | 部分写入返回字节数；完全写不进返回 `-1` + `EAGAIN` |

- **分工**：epoll 管"什么时候去试"，非阻塞保证"试的时候不会被卡住"。
- **fcntl**：先 `F_GETFL` 再 `F_SETFL`，直接设置会覆盖其他标志。
- **accept4**：`accept` 返回的 fd 不继承 `O_NONBLOCK`，用 `SOCK_NONBLOCK` 一步到位。
- **connect**：非阻塞 `connect` 返回的是 `EINPROGRESS`，不是 `EAGAIN`。

**3. TCP**

- **零窗口**：接收方不读 → 窗口为 0 → 发送方缓冲区满；缓冲区空间要等对方 ACK 才释放。
- **RST**：关闭 socket 时接收缓冲区还有未读数据，发 RST 而不是 FIN，对端报 `ECONNRESET`。
- **缓冲区上限**：`sysctl net.ipv4.tcp_wmem` 的最后一个值（默认 4MB）。

**4. 编码教训**

- **警告当错误**：`-Wall` 报 `cev` 未使用，实际注册的是 `listenfd` 的 `ev`，数据到达时会去 `accept` 并卡死。
- **互斥处理**：`read` 返回 0 / -1 并 `close` 之后没有跳过 `write`，又一次漏掉。
- **改名要改全**：`write` 误用了事件数 `n`，可能把栈上残留数据发出去。
- **打印**：高频打印会淹没终端、拖慢程序；`stdout` 重定向到文件时是全缓冲，进程被杀会丢掉最后几行。

**5. 工具**

| 命令 | 用途 |
|---|---|
| `cat /proc/<pid>/fdinfo/<epfd>` | `tfd` 是被监听的 fd，`events`、`data` 都是十六进制 |
| `grep flags /proc/<pid>/fdinfo/<fd>` | 八进制，`04002` = `O_NONBLOCK \| O_RDWR` |
| `top -p <pid>` | 按 `I` 切换 CPU 占用按单核还是全部核计算 |

---

## Lab 3：epoll ET echo

### 简介

在 Lab 2 的基础上，把 `listenfd` 和 `connfd` 都改成 ET。ET 只在新数据、新连接到达时通知一次，所以每次通知都必须处理到 `EAGAIN` 为止。

```cpp
epoll_ctl(epfd, ADD, listenfd, EPOLLIN | EPOLLET);
for (;;) {
  n = epoll_wait(epfd, events);
  for (每个就绪的 fd) {
    if (fd == listenfd) {
      for (;;) { // 循环 accept，直到 EAGAIN
        connfd = accept4(listenfd, SOCK_NONBLOCK);
        if (EAGAIN) break; // 队列取空了
        epoll_ctl(epfd, ADD, connfd, EPOLLIN | EPOLLET);
      }
    } else {
      for (;;) { // 循环 read，直到 EAGAIN
        nread = read(fd, buf);
        if (EAGAIN) break; // 读空了，回到 epoll_wait
        if (nread == 0 或出错) { DEL + close; break; }
        write(fd, buf, nread);
      }
    }
  }
}
```

- 源码：`experiments/03_epoll_et.cc`
- 测试工具：`experiments/tools/burst_connect.py`（AI 编写，一次建立 N 个连接并保持）
- 产物：`build/experiments/03_epoll_et`
- 编译：

  ```bash
  g++ -std=c++17 -Wall -Wextra -Wshadow -Werror -g experiments/03_epoll_et.cc -o build/experiments/03_epoll_et
  ```

### 代码演进

#### 3a

connfd 改成 ET，但仍然只读一次：

```cpp
cev.events = EPOLLIN | EPOLLET; // 只改这一行，读数据的代码不动
```

操作：

1. A：启动 Lab 2 的 LT 版本作为对照
2. B：`python3 -c "print('x' * 20000)" | timeout 2 nc 127.0.0.1 9877 | wc -c`
3. A：换成 `03_epoll_et`，B 再执行一次同样的命令
4. 测试结束后看服务器是否还在；如果退出了，在 A 执行 `echo $?`


- 一次发送 20001 字节，LT 版本收回 20001，ET 版本只收回 8192
  - 原因：数据分两批到达，ET 只通知两次，每次读 4096；剩下的 11809 字节留在接收缓冲区，不会再有通知
- 测试结束后服务器自己退出，退出码 141
  - 原因：141 = 128 + 13，被 SIGPIPE 杀死。`nc` 退出后，服务器还在往这个连接 `write`

时间线：

```
第 1 批数据到达 → 通知 → read 4096 → write
第 2 批数据到达 → 通知 → read 4096 → write（nc 共收到 8192）
没有新数据，11809 字节卡在接收缓冲区
nc 被杀，FIN 到达 → 通知 → read 读到的是数据（FIN 排在数据后面）→ write，对端回 RST
RST 到达 → 通知 → read 又读到数据 → write → SIGPIPE → 进程被杀
```

证据：

```bash
# 发 20001 字节，统计收回多少
python3 -c "print('x' * 20000)" | timeout 2 nc 127.0.0.1 9877 | wc -c # LT：20001；ET：8192

# 服务器退出后查看退出码
echo $? # 141
```

> 遗留问题：ET 下只读一次，数据会卡在缓冲区里

#### 3b

循环 read，直到 EAGAIN：

```cpp
for (;;) {
  nread = read(fd, buf, sizeof(buf));
  if (nread == -1 && errno == EAGAIN) break; // 读空了
  if (nread == 0 || nread == -1) { DEL + close; break; }
  write(fd, buf, nread);
}
```

操作：

1. A：重新启动服务器
2. B：执行和 3a 相同的命令
3. 测试结束后确认服务器还活着


- 同样的测试收回 20001 字节
  - 原因：每次通知都读到 `EAGAIN`，缓冲区里不会剩数据
- 测试结束后服务器还活着
  - 原因：缓冲区已经读空，FIN 到达时 `read` 直接返回 0，服务器关闭连接，之后不会再 `write`

#### 3c

listenfd 也改成 ET：

```cpp
ev.events = EPOLLIN | EPOLLET; // listenfd
for (;;) { // 循环 accept，直到 EAGAIN
  connfd = accept4(listenfd, ..., SOCK_NONBLOCK);
  if (connfd == -1) break; // EAGAIN：队列取空了；其他错误：实验里简化处理
  epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &cev);
}
```

操作：

1. A：启动服务器
2. B：`kill -STOP $(pgrep -f 03_epoll_et)`，暂停服务器
3. B：`python3 experiments/tools/burst_connect.py 10 30`，暂停期间建立 10 个连接
4. B：`kill -CONT $(pgrep -f 03_epoll_et)`，恢复服务器
5. B：`ss -tln | grep 9877`，看 `Recv-Q`；同时数一数服务器打印了几条 `client:`
6. 先用"只 accept 一次"的版本做一遍，再用"循环 accept"的版本做一遍


- 先只改事件、仍然只 accept 一次：暂停服务器期间建立 10 个连接，恢复后只打印 1 条 `client:`，`Recv-Q = 9`
  - 原因：10 个连接只换来一次通知；`accept` 取走连接不会触发通知，剩下 9 个要等下一个新连接到来
- 改成循环 accept 后：打印 10 条 `client:`，`Recv-Q = 0`
  - 原因：每次通知都把全连接队列取空

证据：

```bash
kill -STOP $(pgrep -f 03_epoll_et) # 暂停服务器
python3 experiments/tools/burst_connect.py 10 30 # 暂停期间建立 10 个连接
kill -CONT $(pgrep -f 03_epoll_et) # 恢复服务器
ss -tln | grep 9877 # 只 accept 一次：Recv-Q = 9；循环 accept：Recv-Q = 0
```

> 遗留问题：`accept` 循环遇到 `EMFILE` 时提前退出，剩下的连接同样不会再有通知（任务 4、D10）

### 要点

**1. LT 与 ET**

- **通知时机**：LT 只要就绪就每次通知；ET 只在新数据、新连接到达时通知一次，`read` / `accept` 取走东西不会触发通知。
- **ET 的规矩**：每次通知都处理到 `EAGAIN`；只读一次或只 accept 一次，剩下的就卡住了。
- **ET 必须非阻塞**：要靠 `EAGAIN` 判断"取空了"，阻塞 fd 在最后一次 `read` / `accept` 时会卡住线程。
- **公平性**：ET 读到 `EAGAIN` 才停，一个流量很大的连接可能长时间占住线程；LT 每次只读一次，各连接轮流处理。这是 D2 选 LT 的理由之一。

**2. 信号与关闭**

- **SIGPIPE**：向已收到 RST 的连接 `write` 时触发，默认动作是终止进程，不会打印任何错误；忽略它之后 `write` 返回 `EPIPE`。服务器启动时要忽略 SIGPIPE。
- **退出码**：进程被信号杀死时，退出码 = 128 + 信号编号；`kill -l 13` 查到 13 是 `PIPE`。
- **FIN 排在数据后面**：缓冲区里还有数据时，`read` 先返回数据，读完才返回 0。

**3. 编码教训**

- **加一层循环后检查 `break` / `continue`**：原来跳到下一个事件的 `continue`，放进内层循环后会变成继续 `read` / `accept`。

**4. 工具**

| 命令 | 用途 |
|---|---|
| `kill -STOP <pid>` / `kill -CONT <pid>` | 暂停 / 恢复进程；暂停期间内核照样完成握手，可以制造"多个连接同时到达" |
| `echo $?` | 查看上一个进程的退出码 |

---

## Lab 4：TCP 细节小实验

### 简介

五个独立的小实验。服务器用 `04_misc`（Lab 2 的 LT 版本，加上 `SO_REUSEADDR`），另外写了两个客户端。

- 源码：
  - `experiments/04_misc.cc`：服务器
  - `experiments/04_sigpipe_client.cc`：服务器退出后连续写两次（4c）
  - `experiments/04_halfclose_client.cc`：参数 `shutdown` / `close`，对比两种关闭方式（4d）
- 测试工具：`experiments/tools/burst_connect.py`（4e）
- 产物：`build/experiments/` 下的同名程序
- 编译：

  ```bash
  g++ -std=c++17 -Wall -Wextra -Wshadow -Werror -g experiments/04_misc.cc -o build/experiments/04_misc
  ```

### 代码演进

#### 4a

服务器有连接时 Ctrl+C，然后立刻重启（代码不改）：

操作：

1. A：启动 `02_epoll_lt`
2. B：`nc 127.0.0.1 9877`，保持连接
3. A：Ctrl+C 结束服务器，然后立刻重新启动
4. C：`ss -tan | grep 9877`，记下两侧的状态
5. B：Ctrl+C 结束 `nc`
6. C：再执行一次 `ss -tan | grep 9877`
7. A：每隔 15 秒左右重启一次，记下多久后能成功


- 重启报 `bind() failed: Address already in use (errno = 98)`
  - 原因：服务器进程虽然退出了，旧连接还在内核里，占着本地地址 `127.0.0.1:9877`
- `nc` 还连着时：服务器一侧 FIN-WAIT-2，`nc` 一侧 CLOSE-WAIT
  - 原因：内核替退出的服务器发了 FIN，`nc` 回了 ACK；但 `nc` 收到 FIN 不退出，一直没 `close`
- `nc` 退出后：服务器一侧变成 TIME-WAIT，约 1 分钟后才能重启成功
  - 原因：主动关闭的一方要在 TIME-WAIT 等待，Linux 写死为 60 秒

证据：

```bash
# nc 还连着
CLOSE-WAIT 0      0      127.0.0.1:49540     127.0.0.1:9877
FIN-WAIT-2 0      0      127.0.0.1:9877      127.0.0.1:49540

# nc 退出后
TIME-WAIT  0      0      127.0.0.1:9877      127.0.0.1:49540
```

> 遗留问题：服务器重启要等 1 分钟

#### 4b

`bind` 之前打开 `SO_REUSEADDR`：

```cpp
int on = 1;
setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)); // 必须在 bind 之前
```

操作：

1. 重复 4a 的步骤 1～3，这次用 `04_misc`
2. C：`ss -tan | grep 9877`
3. 同时运行两个 `04_misc`，看第二个能否启动


- 同样的步骤，服务器能立刻重启
  - 原因：`SO_REUSEADDR` 允许端口上还有 TIME-WAIT 等状态的旧连接时 `bind` 成功
- 同时运行两个服务器，第二个仍然报 `EADDRINUSE`
  - 原因：它不允许两个 socket 同时 LISTEN 同一个端口
- 新的 LISTEN 和旧连接同时存在
  - 原因：旧服务器一侧的 socket 变成"孤儿"，由内核维持；新服务器只能 `accept` 新连接，接管不了它。`nc` 再发数据时，按四元组找到的是孤儿 socket，内核会回 RST

证据：

```bash
LISTEN     0      4096         0.0.0.0:9877           0.0.0.0:*     # 新服务器
CLOSE-WAIT 0      0          127.0.0.1:54420        127.0.0.1:9877  # 旧连接，nc 一侧
FIN-WAIT-2 0      0          127.0.0.1:9877         127.0.0.1:54420 # 旧连接，孤儿
TIME-WAIT  0      0          127.0.0.1:9877         127.0.0.1:50896 # 更早一轮留下的
```

#### 4c

服务器退出后，客户端连续写两次：

```cpp
connect(fd, 127.0.0.1:9877);
sleep(5); // 这期间 Ctrl+C 服务器
write(fd, "a", 1);
sleep(1);
write(fd, "b", 1);
```

操作：

1. A：启动 `04_misc`
2. B：`./build/experiments/04_sigpipe_client`
3. A：5 秒内按 Ctrl+C 结束服务器
4. B：客户端结束后执行 `echo $?`
5. 在客户端 `main` 开头加上 `signal(SIGPIPE, SIG_IGN)`，重复一遍


- 第一次 `write` 返回 1
  - 原因：`write` 只是拷进本机的发送缓冲区；客户端收到 FIN 只知道对方"不发了"，发送仍然合法。"a" 到达孤儿 socket，换回一个 RST
- 第二次 `write` 什么都没打印，退出码 141
  - 原因：连接已被 RST 作废，再写就触发 SIGPIPE（13 号），默认动作是终止进程，进程死在 `write` 里面
- 加上 `signal(SIGPIPE, SIG_IGN)` 后，第二次 `write` 打印 `Broken pipe (errno = 32)`，退出码 1
  - 原因：忽略信号只改变通知方式（信号 → 返回值 `EPIPE`），连接照样是坏的；退出码 1 来自自己的 `exit(EXIT_FAILURE)`

#### 4d

写完 `hello` 后，分别用 `shutdown(SHUT_WR)` 和 `close` 关闭，再循环 `read`：

```cpp
write(fd, "hello\n", 6);
if (mode == "shutdown") shutdown(fd, SHUT_WR);
else close(fd);
for (;;) { n = read(fd, buf, sizeof(buf)); ... } // 0 → server closed；-1 → 打印错误
```

操作：

1. A：启动 `04_misc`
2. B：`./build/experiments/04_halfclose_client shutdown`
3. B：`./build/experiments/04_halfclose_client close`


- `shutdown`：收到 `hello` 回显，然后打印 `server closed`
  - 原因：只关了发送方向，fd 还能读；服务器读到数据和 FIN，回显后关闭连接
- `close`：`read() failed: Bad file descriptor (errno = 9)`
  - 原因：fd 被整个释放了；回显到达时客户端已没人读，内核会回 RST

#### 4e

只给服务器 10 个 fd，一次连上 8 个客户端：

操作：

1. A：`prlimit --nofile=10 ./build/experiments/04_misc > build/experiments/emfile.log 2>&1`
2. B：`python3 experiments/tools/burst_connect.py 8 30`，建立 8 个连接，保持 30 秒
3. C：用 `btop` 或 `command top -p $(pgrep -f 04_misc)` 看 CPU 占用
4. C：`ss -tln | grep 9877`，看 `Recv-Q`
5. C：`ls /proc/$(pgrep -f 04_misc)/fd | wc -l`，看服务器打开了几个 fd
6. C：30 秒内执行 `grep -c 'Too many open files' build/experiments/emfile.log`，隔几秒再执行一次


- 服务器打开的 fd 数是 10，只 `accept` 了 5 个，`Recv-Q = 3`
  - 原因：0/1/2、`listenfd`、`epfd` 占了 5 个，剩下 5 个给客户端；之后 `accept4` 返回 `EMFILE`
- 日志里 1300 多万行 `Too many open files`，一个核跑满
  - 原因：LT 下队列一直非空 → 一直通知 → `accept4` 失败不改变任何状态 → 死循环
- 那 3 个客户端看来"已连接"，却永远没人处理

证据：

```bash
ss -tln | grep 9877                                        # LISTEN 3  4096  0.0.0.0:9877
ls /proc/$(pgrep -f 04_misc)/fd | wc -l                    # 10
grep -c 'Too many open files' build/experiments/emfile.log # 13275821
# btop：C8 60% + C9 39%，Load avg ≈ 1（总 CPU 只显示 10%，因为是 16 个逻辑核的平均）
```

> 遗留问题（M2）：EMFILE 时要把连接从队列里取出来。预留一个空闲 fd（`/dev/null`），EMFILE 时关掉它 → `accept` → 立刻 `close` → 再占回来（D10）

#### 4f

用 strace 看一次完整连接：

操作：

1. A：`strace -f -e trace=socket,setsockopt,bind,listen,fcntl,epoll_create1,epoll_ctl,epoll_wait,accept4,read,write,close ./build/experiments/04_misc`
2. B：`printf 'hello\n' | nc -N 127.0.0.1 9877`（`-N`：发完就 `shutdown(SHUT_WR)`）
3. A：Ctrl+C，从 `socket(...)` 那一行开始看输出


- 看不到三次握手
  - 原因：strace 只能看到进程和内核之间的系统调用；握手由内核在收包时完成，发生在 `listen` 之后、`epoll_wait` 因 `listenfd` 可读而返回之前，那时进程正睡在 `epoll_wait` 里
- 对 FIN 的处理也只能间接看到：`epoll_wait` 报告 fd 5 可读，`read` 返回 0

证据（预跑输出，端口号不同）：

```
listen(3, 4096) = 0                                             # 从这里起内核可以自己握手
epoll_wait(4, [{events=EPOLLIN, data=0x3}], 16, -1) = 1         # 握手已完成，连接在全连接队列里
accept4(3, {... sin_port=htons(55748) ...}, [16], SOCK_NONBLOCK) = 5
epoll_ctl(4, EPOLL_CTL_ADD, 5, {events=EPOLLIN, data=0x5}) = 0
epoll_wait(4, [{events=EPOLLIN, data=0x5}], 16, -1) = 1
read(5, "hello\n", 4096) = 6
write(5, "hello\n", 6) = 6
epoll_wait(4, [{events=EPOLLIN, data=0x5}], 16, -1) = 1         # FIN 让 fd 变成可读
read(5, "", 4096) = 0
epoll_ctl(4, EPOLL_CTL_DEL, 5, NULL) = 0
close(5) = 0
```

### 要点

**1. 关闭与 TIME_WAIT**

- **TIME_WAIT 在主动关闭方**：等 60 秒，一是保证最后一个 ACK 能送达（丢了对方会重发 FIN），二是让旧连接的残留报文消失，避免串进相同四元组的新连接。
- **CLOSE_WAIT**：对方已关、自己的应用还没 `close`；大量 CLOSE_WAIT 说明代码忘了 `close`。
- **SO_REUSEADDR**：服务器都应该在 `bind` 前打开，否则重启要等 TIME_WAIT 结束。
- **FIN 只关一个方向**：被动关闭方在 CLOSE_WAIT 里仍可 `write`；内核不会自动发 FIN，要等应用 `close`。
- **孤儿 socket 回 RST**：报文由内核接收；数据到达一个应用已关闭的 socket，没人会读，内核回 RST。

**2. shutdown 与 close**

- **方向**：`shutdown(SHUT_WR)` 只关发送，还能 `read`；`close` 两个方向都不能用，fd 被释放。
- **FIN 的时机**：`shutdown` 立刻发；`close` 只在指向该连接的最后一个 fd 关闭时才发（`dup`、`fork` 会增加引用）。
- **未读数据**：`close` 时接收缓冲区还有数据，发 RST 而不是 FIN。
- **D11**：优雅关闭 = 输出缓冲区发完后 `shutdown(SHUT_WR)`；`forceClose` = 直接关。

**3. SIGPIPE**

- **触发**：对已被 RST 的连接 `write`；第一次 `write` 往往还会成功，因为 RST 要一个来回才到。
- **处理**：服务器启动时 `signal(SIGPIPE, SIG_IGN)`，再按 `EPIPE` 关闭这一个连接。
- **write 成功 ≠ 对方收到**：要确认对方处理了，只能靠应用层的回复。

**4. 三次握手的时机**

- **对客户端**：发生在 `connect` 调用期间，握手完成 `connect` 才返回。
- **对服务器**：和自己的代码不同步；只要已经 `listen` 且队列没满，内核在收包时就完成握手。`listen` 之前收到 SYN 会回 RST（`ECONNREFUSED`）。

**5. 编码教训**

- **`strcmp` 要写 `== 0`**：少写会让分支条件反过来；这次导致 `close` 没执行、没发 FIN，双方互等卡住。
- **按长度打印 `read` 的数据**：缓冲区不以 `'\0'` 结尾，用 `printf("%.*s", (int)n, buf)`。
- **`fflush` 的位置**：要紧跟在可能被信号杀死之前的那次 `printf` 后面。
- **参数检查后要退出**：否则后面会拿空指针调用 `strcmp`。

**6. 工具**

| 命令 | 用途 |
|---|---|
| `ss -tan` | 显示所有状态的连接，看 FIN-WAIT-2、CLOSE-WAIT、TIME-WAIT |
| `prlimit --nofile=N ./prog` | 只限制一个进程的 fd 上限；在 zsh 里直接 `ulimit -n 10` 会把 shell 自己弄坏 |
| `strace -f -e trace=... ./prog` | 跟踪进程（及子进程、线程）的系统调用 |
| `\top` / `command top` | 绕过 `top=btop` 这样的别名 |

---

## 参考资料

- `man 2 socket`、`man 2 bind`、`man 2 listen`、`man 2 accept`
- `man 7 epoll`、`man 2 epoll_ctl`、`man 2 epoll_wait`
- `man 2 fcntl`、`man 7 tcp`、`man 7 signal`
- `man 2 setsockopt`、`man 2 connect`、`man 2 shutdown`、`man 1 strace`、`man 1 prlimit`
