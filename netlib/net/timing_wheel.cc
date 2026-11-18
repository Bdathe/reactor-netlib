/**
 * @file timing_wheel.cc
 * @brief TimingWheel 的实现
 */

#include "netlib/net/timing_wheel.h"

#include <any>
#include <cassert>
#include <utility>

#include "netlib/base/logging.h"
#include "netlib/net/event_loop.h"
#include "netlib/net/tcp_connection.h"

namespace netlib {

TimingWheel::Entry::~Entry() {
  // 提升 weak_conn
  TcpConnectionPtr conn = weak_conn.lock();

  if (conn && conn->connected()) {
    LOG_INFO << "[" << conn->name() << "] idle timeout, force closing";
    conn->ForceClose();
  }
}

TimingWheel::TimingWheel(EventLoop* loop, std::size_t num_buckets, Duration tick_interval)
    : loop_(loop), buckets_(num_buckets) {
  loop_->AssertInLoopThread();

  assert(num_buckets > 0);
  assert(tick_interval > Duration::zero());

  timer_id_ = loop_->RunEvery(tick_interval, [this] { OnTick(); });
}

TimingWheel::~TimingWheel() {
  loop_->AssertInLoopThread();
  // 取消周期定时器
  loop_->Cancel(timer_id_);
}

void TimingWheel::OnConnection(const TcpConnectionPtr& conn) {
  loop_->AssertInLoopThread();
  assert(conn->loop() == loop_);

  if (conn->connected()) {
    EntryPtr entry = std::make_shared<Entry>(conn);
    // conn 只能有用一份 weak_conn 用于观察，真正是实体在 buckets_ 中
    conn->set_context(WeakEntryPtr(entry));
    buckets_.back().insert(std::move(entry));
  }
}

void TimingWheel::OnMessage(const TcpConnectionPtr& conn) {
  loop_->AssertInLoopThread();

  // 从上下文中拿到 weak_conn
  const WeakEntryPtr* weak_entry = std::any_cast<WeakEntryPtr>(&conn->context());
  if (weak_entry) {
    EntryPtr entry = weak_entry->lock();
    if (entry) {
      buckets_.back().insert(entry);
    }
  }
}

void TimingWheel::OnTick() {
  loop_->AssertInLoopThread();

  // 尾部构造一个空的篮子
  buckets_.emplace_back();
  // 销毁最前面的篮子
  buckets_.pop_front();
}

}  // namespace netlib
