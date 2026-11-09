/**
 * @file acceptor.h
 * @brief 监听端口，接受新的连接
 */

#pragma once

#include <functional>
#include <utility>

#include "netlib/base/noncopyable.h"
#include "netlib/net/channel.h"
#include "netlib/net/socket.h"

namespace netlib {

// 前置声明
class EventLoop;
class InetAddress;
class Timestamp;

class Acceptor : Noncopyable {
public:
  using NewConnectionCallback = std::function<void(int sockfd, const InetAddress& peer_addr)>;

  /// 创建监听 socket 并 bind，失败时终止程序；此时还没有 listen
  Acceptor(EventLoop* loop, const InetAddress& listen_addr);
  ~Acceptor();

  /// 设置新连接回调。回调接管 sockfd 的所有权；没有设置时，新连接会被直接关闭
  void set_new_connection_callback(NewConnectionCallback callback) {
    new_connection_callback_ = std::move(callback);
  }

  bool listening() const { return listening_; }

  /// 开始 listen，并关注 listenfd 的读事件。只能在 loop 线程调用
  void Listen();

private:
  /// listenfd 可读：accept 一个连接交给回调；遇到 EMFILE 时用 idle_fd_ 拒绝这个连接
  void HandleRead(Timestamp receive_time);

private:
  EventLoop* loop_;                                // 所属的 loop
  Socket accept_socket_;                           // 监听 socket，持有 listenfd
  Channel accept_channel_;                         // listenfd 的事件分发器
  NewConnectionCallback new_connection_callback_;  // 新连接的回调
  bool listening_;                                 // 是否已经开始监听
  int idle_fd_;                                    // 预留的空闲 fd
};

}  // namespace netlib
