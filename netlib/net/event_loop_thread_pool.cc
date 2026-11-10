/**
 * @file event_loop_thread_pool.cc
 * @brief EventLoopThreadPool 的实现
 */

#include "netlib/net/event_loop_thread_pool.h"

#include <cassert>
#include <utility>

#include "netlib/net/event_loop.h"
#include "netlib/net/event_loop_thread.h"

namespace netlib {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop)
    : base_loop_(base_loop), started_(false), num_threads_(0), next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::SetThreadNum(int num_threads) {
  assert(!started_);
  assert(num_threads >= 0);
  num_threads_ = num_threads;
}

void EventLoopThreadPool::Start() {
  base_loop_->AssertInLoopThread();
  assert(!started_);

  started_ = true;

  for (int i = 0; i < num_threads_; ++i) {
    auto t = std::make_unique<EventLoopThread>();
    loops_.push_back(t->StartLoop());
    threads_.push_back(std::move(t));
  }
}

EventLoop* EventLoopThreadPool::GetNextLoop() {
  base_loop_->AssertInLoopThread();
  assert(started_);

  if (loops_.empty()) {
    return base_loop_;
  }

  EventLoop* ret_loop = loops_[next_];
  ++next_;
  if (next_ >= loops_.size()) {
    next_ = 0;
  }

  return ret_loop;
}

}  // namespace netlib
