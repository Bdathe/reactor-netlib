/**
 * @file event_loop_thread.cc
 * @brief EventLoopThread 的实现
 */

#include "netlib/net/event_loop_thread.h"

#include <cassert>

#include "netlib/net/event_loop.h"

namespace netlib {

EventLoopThread::EventLoopThread() : loop_(nullptr) {}

EventLoop* EventLoopThread::StartLoop() {
  assert(!thread_.joinable());

  thread_ = std::thread([this] { ThreadFunc(); });

  // loop_ 之后还会被新线程改写（Loop 返回后置空），锁外读它是数据竞争
  // 所以在锁内拷贝到局部变量，解锁后只用这份拷贝
  EventLoop* loop{};
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return loop_ != nullptr; });
    loop = loop_;
  }

  return loop;
}

EventLoopThread::~EventLoopThread() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Quit 必须在锁内调用：ThreadFunc 要拿到这把锁才能置空 loop_ 并返回，
    // 所以持锁期间栈上的 loop 不会析构；锁外调用可能访问已析构的 loop
    if (loop_ != nullptr) {
      loop_->Quit();
    }
  }

  if (thread_.joinable()) {
    thread_.join();
  }
}

void EventLoopThread::ThreadFunc() {
  EventLoop loop;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = &loop;
  }

  cond_.notify_one();
  loop.Loop();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
  }
}

}  // namespace netlib
