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

  TcpServer(EventLoop* loop, const InetAddress& listen_addr, std::string name);
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

  void Start();

private:
  void NewConnection(int sockfd, const InetAddress& peer_addr);
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
