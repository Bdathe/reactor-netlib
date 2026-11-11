/**
 * @file tcp_server.cc
 * @brief TcpServer 的实现
 */

#include "netlib/net/tcp_server.h"

#include <cassert>

#include "netlib/base/logging.h"
#include "netlib/net/acceptor.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/event_loop_thread_pool.h"
#include "netlib/net/tcp_connection.h"

namespace netlib {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listen_addr, std::string name)
    : loop_(loop),
      name_(std::move(name)),
      ip_port_(listen_addr.ToIpPort()),
      acceptor_(std::make_unique<Acceptor>(loop, listen_addr)),
      started_(false),
      next_connection_id_(0),
      thread_pool_(std::make_unique<EventLoopThreadPool>(loop)) {
  acceptor_->set_new_connection_callback(
      [this](int sockfd, const InetAddress& peer_addr) { NewConnection(sockfd, peer_addr); });
}

TcpServer::~TcpServer() {
  // 连接表不加锁，只能在 loop_ 所在的线程里调用
  loop_->AssertInLoopThread();

  for (const auto& [name, conn] : connection_table_) {
    conn->loop()->RunInLoop([c = conn] { c->ConnectDestroyed(); });
  }
}

void TcpServer::Start() {
  loop_->AssertInLoopThread();

  if (!started_) {
    started_ = true;

    thread_pool_->Start();

    assert(!acceptor_->listening());
    acceptor_->Listen();
  }
}

void TcpServer::SetThreadNum(int num_threads) {
  thread_pool_->SetThreadNum(num_threads);
}

void TcpServer::NewConnection(int sockfd, const InetAddress& peer_addr) {
  loop_->AssertInLoopThread();

  EventLoop* io_loop = thread_pool_->GetNextLoop();
  std::string conn_name = name_ + "-" + ip_port_ + "#" + std::to_string(next_connection_id_);
  ++next_connection_id_;
  LOG_INFO << "[" << conn_name << "] new connection, peer=" << peer_addr.ToIpPort()
           << ", fd=" << sockfd;

  InetAddress local_addr = GetLocalAddr(sockfd);

  // 创建连接
  TcpConnectionPtr conn =
      std::make_shared<TcpConnection>(io_loop, conn_name, sockfd, local_addr, peer_addr);

  // 加入连接表
  connection_table_[conn_name] = conn;

  // 设置回调
  conn->set_connection_callback(connection_callback_);
  conn->set_message_callback(message_callback_);
  conn->set_write_complete_callback(write_complete_callback_);
  conn->set_close_callback([this](const TcpConnectionPtr& c) { RemoveConnection(c); });

  io_loop->RunInLoop([conn] { conn->ConnectEstablished(); });
}

void TcpServer::RemoveConnection(const TcpConnectionPtr& conn) {
  loop_->RunInLoop([this, conn] { RemoveConnectionInLoop(conn); });
}

void TcpServer::RemoveConnectionInLoop(const TcpConnectionPtr& conn) {
  loop_->AssertInLoopThread();

  LOG_INFO << "[" << conn->name() << "] connection removed";
  [[maybe_unused]] std::size_t ret = connection_table_.erase(conn->name());
  assert(ret == 1);

  // 用 QueueInLoop 而不是 RunInLoop：线程数为 0 时也推迟到这一轮 HandleEvent 结束后再 Remove，
  // 避免在 Channel 自己的事件处理还在调用栈上时移除它；单线程、多线程的执行顺序也保持一致
  EventLoop* io_loop = conn->loop();
  io_loop->QueueInLoop([conn] { conn->ConnectDestroyed(); });
}

}  // namespace netlib
