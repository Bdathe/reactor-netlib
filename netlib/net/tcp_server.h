/**
 * @file tcp_server.h
 * @brief 负责接受新连接并加入连接表
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "netlib/base/noncopyable.h"
#include "netlib/net/callbacks.h"
#include "netlib/net/inet_address.h"

namespace netlib {

// 前置声明
class EventLoop;
class Acceptor;

class TcpServer : Noncopyable {
public:
  using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

  /// 创建 Acceptor（此时 bind，还没有 listen）
  TcpServer(EventLoop* loop, const InetAddress& listen_addr, std::string name);
  /// 对连接表中剩下的连接调用 ConnectDestroyed。只能在 loop 线程析构
  ~TcpServer();

  EventLoop* loop() const { return loop_; }
  const std::string& name() const { return name_; }
  const std::string& ip_port() const { return ip_port_; }

  void set_connection_callback(ConnectionCallback callback) {
    connection_callback_ = std::move(callback);
  }
  void set_message_callback(MessageCallback callback) { message_callback_ = std::move(callback); }
  void set_write_complete_callback(WriteCompleteCallback callback) {
    write_complete_callback_ = std::move(callback);
  }

  /// 开始监听，可以重复调用，只生效一次。只能在 loop 线程调用
  /// 用户回调要在 Start 之前设置，之后的新连接才能拿到
  void Start();

private:
  /// Acceptor 的回调：创建 TcpConnection，放进连接表，拷贝用户回调，再 ConnectEstablished
  void NewConnection(int sockfd, const InetAddress& peer_addr);
  /// 连接的关闭回调：从连接表中删除，再 ConnectDestroyed
  void RemoveConnection(const TcpConnectionPtr& conn);

private:
  EventLoop* loop_;                                // 所属的 loop
  const std::string name_;                         // 服务器名字
  const std::string ip_port_;                      // 监听地址
  std::unique_ptr<Acceptor> acceptor_;             // 接受器
  ConnectionCallback connection_callback_;         // 连接回调
  MessageCallback message_callback_;               // 消息回调
  WriteCompleteCallback write_complete_callback_;  // 写完成回调
  bool started_;                                   // 是否已经 Start
  int next_connection_id_;                         // 下一个连接的编号
  ConnectionMap connection_table_;                 // 连接表
};

}  // namespace netlib
