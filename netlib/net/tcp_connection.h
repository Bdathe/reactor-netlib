/**
 * @file tcp_connection.h
 * @brief 一条 TCP 连接：持有 Socket、Channel 和输入/输出 Buffer
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "netlib/base/noncopyable.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/buffer.h"
#include "netlib/net/callbacks.h"
#include "netlib/net/channel.h"
#include "netlib/net/inet_address.h"
#include "netlib/net/socket.h"

namespace netlib {

// 前置声明
class EventLoop;

class TcpConnection : Noncopyable, public std::enable_shared_from_this<TcpConnection> {
public:
  static constexpr std::size_t kDefaultHighWaterMark = 64 * 1024 * 1024;  // 64 MB

  /// 接管 sockfd 的所有权，注册 Channel 的四个回调。状态为 kConnecting，还没有进入 epoll
  TcpConnection(EventLoop* loop, std::string name, int sockfd, const InetAddress& local_addr,
                const InetAddress& peer_addr);
  ~TcpConnection();

  EventLoop* loop() const { return loop_; }
  const std::string& name() const { return name_; }
  const InetAddress& local_addr() const { return local_addr_; }
  const InetAddress& peer_addr() const { return peer_addr_; }
  bool connected() const { return state_ == State::kConnected; }

  void set_connection_callback(ConnectionCallback callback) {
    connection_callback_ = std::move(callback);
  }
  void set_message_callback(MessageCallback callback) { message_callback_ = std::move(callback); }
  void set_write_complete_callback(WriteCompleteCallback callback) {
    write_complete_callback_ = std::move(callback);
  }
  void set_high_water_mark_callback(HighWaterMarkCallback callback, std::size_t high_water_mark) {
    high_water_mark_callback_ = std::move(callback);
    high_water_mark_ = high_water_mark;
  }
  /// 库内部使用：TcpServer 设置，连接关闭时通知它删除连接
  void set_close_callback(CloseCallback callback) { close_callback_ = std::move(callback); }

  /// 连接建立：状态改为 kConnected，Tie，关注读事件，回调用户（上线）。
  /// 由 TcpServer 在 conn 被 shared_ptr 管理之后调用，只调用一次
  void ConnectEstablished();

  /// 连接销毁的最后一步：从 Poller 中移除 Channel。
  /// 如果还是 kConnected（如 TcpServer 析构），先改为 kDisconnected 并回调用户（下线）
  void ConnectDestroyed();

  /// 发送数据：能直接写就直接写，写不完的放进输出 Buffer 并关注 EPOLLOUT。
  /// 不是 kConnected 时丢弃。目前只能在 loop 线程调用
  void Send(std::string_view message);

  /// 半关闭：状态改为 kDisconnecting，输出 Buffer 写空后再 shutdown(SHUT_WR)
  void Shutdown();

  /// 立即关闭，不等输出 Buffer 写完，走 HandleClose
  void ForceClose();

private:
  enum class State { kConnecting, kConnected, kDisconnecting, kDisconnected };

  void set_state(State state) { state_ = state; }

  /// 读到数据回调用户；读到 0 进入 HandleClose；出错进入 HandleError
  void HandleRead(Timestamp receive_time);
  /// 状态改为 kDisconnected，取消所有事件，回调用户（下线），再通知 TcpServer
  void HandleClose();
  /// 用 SO_ERROR 取出并清除 socket 错误，只记录日志
  void HandleError();
  /// 继续写输出 Buffer；写空后取消 EPOLLOUT，回调写完成，kDisconnecting 时半关闭
  void HandleWrite();

  /// 已经在 loop 线程中的发送实现
  void SendInLoop(const char* data, std::size_t len);

  /// 没有在关注写事件（输出 Buffer 已写空）时才 shutdown(SHUT_WR)
  void ShutdownInLoop();

private:
  EventLoop* loop_;                                 // 连接所属的 loop
  const std::string name_;                          // 连接名，TcpServer 生成，作为连接表的 key
  State state_;                                     // 当前的连接状态
  Socket socket_;                                   // 拥有 connfd，析构时关闭
  Channel channel_;                                 // connfd 的事件分发
  const InetAddress local_addr_;                    // 本端地址
  const InetAddress peer_addr_;                     // 对端地址
  Buffer input_buffer_;                             // 收到、还没被用户取走的数据
  Buffer output_buffer_;                            // 还没写进内核的数据
  std::size_t high_water_mark_;                     // 高水位阈值
  ConnectionCallback connection_callback_;          // 连接上线/下线的回调
  MessageCallback message_callback_;                // 收到数据的回调
  WriteCompleteCallback write_complete_callback_;   // 写完成的回调
  HighWaterMarkCallback high_water_mark_callback_;  // 高水位的回调
  CloseCallback close_callback_;                    // 通知 TcpServer 删除连接
};

}  // namespace netlib
