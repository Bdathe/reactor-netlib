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
  void set_close_callback(CloseCallback callback) { close_callback_ = std::move(callback); }

  void ConnectEstablished();

  void ConnectDestroyed();

  void Send(std::string_view message);

  void Shutdown();

  void ForceClose();

private:
  enum class State { kConnecting, kConnected, kDisconnecting, kDisconnected };

  void set_state(State state) { state_ = state; }

  void HandleRead(Timestamp receive_time);
  void HandleClose();
  void HandleError();
  void HandleWrite();

  void SendInLoop(const char* data, std::size_t len);

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
