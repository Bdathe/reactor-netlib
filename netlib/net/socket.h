/**
 * @file socket.h
 * @brief 持有一个 socket fd，析构时关闭它
 *
 * 构造时接管 fd 的所有权，调用者之后不要再 close 它
 */

#pragma once

#include "netlib/base/noncopyable.h"

namespace netlib {

// 前置声明
class InetAddress;

class Socket : Noncopyable {
public:
  explicit Socket(int fd) : fd_(fd) {}

  ~Socket();

  int fd() const { return fd_; }

  /// 绑定地址，失败时终止程序
  void BindAddress(const InetAddress& addr);

  /// 开始监听，失败时终止程序
  void Listen();

  /// 接受一个连接，返回非阻塞、close-on-exec 的 connfd，对端地址写入 *peer。
  /// 失败返回 -1，errno 保持不变，由调用者处理
  int Accept(InetAddress* peer);

  /// 设置 SO_REUSEADDR 选项
  /// 允许 bind 仍有 TIME_WAIT 连接的端口，服务器重启时用
  void SetReuseAddr(bool on);

  /// 设置 TCP_NODELAY 选项
  /// 关闭 Nagle 算法，小数据立即发送，降低延迟
  void SetTcpNoDelay(bool on);

  /// 设置 SO_KEEPALIVE 选项
  /// 连接空闲时由内核定期探测对端是否存活
  void SetKeepAlive(bool on);

  /// 关闭写方向（发送 FIN），仍可继续读
  void ShutdownWrite();

private:
  const int fd_;
};

/// 创建非阻塞、close-on-exec 的 TCP socket，失败时终止程序
int CreateNonblockingSocketOrDie();

/// 获取连接的本地地址，失败时返回全零地址
InetAddress GetLocalAddr(int sockfd);

}  // namespace netlib
