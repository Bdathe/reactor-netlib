/**
 * @file thread_pool.cc
 * @brief ThreadPool 的实现
 */

#include "netlib/base/thread_pool.h"

#include <cassert>

namespace netlib {

ThreadPool::~ThreadPool() {
  if (threads_.empty())
    return;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }

  // 唤醒所有消费者（消费者会看到 running_ == false）
  not_empty_.notify_all();
  // 唤醒所有生产者（生产者会看到 running_ == false）
  not_full_.notify_all();

  for (std::thread& t : threads_) {
    t.join();
  }
}

void ThreadPool::SetMaxQueueSize(std::size_t max_size) {
  // 断言该函数在 Start 之前调用
  assert(threads_.empty());
  max_queue_size_ = max_size;
}

void ThreadPool::Start(int num_threads) {
  // 断言该函数首次调用
  assert(threads_.empty());
  assert(num_threads > 0);

  running_ = true;

  threads_.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads_.emplace_back([this] { ThreadFunc(); });
  }
}

void ThreadPool::Run(Task task) {
  assert(task);
  assert(!threads_.empty());

  std::unique_lock<std::mutex> lock(mutex_);
  // 当线程池正在运行队列不限制长度但是队列满了的情况，继续睡眠（谓词为假则睡眠）
  not_full_.wait(lock, [this] {
    return !running_ || max_queue_size_ == 0 || queue_.size() < max_queue_size_;
  });

  if (!running_) {
    // 丢掉本次任务
    return;
  }

  queue_.push_back(std::move(task));
  // 通知消费者
  not_empty_.notify_one();
}

void ThreadPool::ThreadFunc() {
  while (Task task = Take()) {
    task();
  }
}

ThreadPool::Task ThreadPool::Take() {
  std::unique_lock<std::mutex> lock(mutex_);
  // 线程池正在运行而且队列为空则睡眠（谓词为假则睡眠）
  not_empty_.wait(lock, [this] { return !queue_.empty() || !running_; });

  Task ret{};
  if (!queue_.empty()) {
    ret = std::move(queue_.front());
    queue_.pop_front();
  }

  // 取走一个任务，通知生产者
  not_full_.notify_one();

  return ret;
}

}  // namespace netlib
