/**
 * @file thread_pool.h
 * @brief 业务线程池，负责处理复杂计算任务
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "netlib/base/noncopyable.h"

namespace netlib {

class ThreadPool : Noncopyable {
public:
  using Task = std::function<void()>;

  explicit ThreadPool(std::string name)
      : name_(std::move(name)), max_queue_size_(0), running_(false) {}
  ~ThreadPool();

  /// 设置最大队列长度，在 Start 之前调用
  void SetMaxQueueSize(std::size_t max_size);
  /// 初始化线程池，创建线程，只能调用一次
  void Start(int num_threads);
  /// 生产者
  /// 往线程池中投递任务，任意线程都可以调用
  void Run(Task task);

private:
  /// 线程入口，循环调用 Take，拿到非空任务就执行
  void ThreadFunc();
  /// 消费者
  /// 工作线程取任务的唯一入口
  Task Take();

private:
  std::string name_;                   // 线程池名字
  std::deque<Task> queue_;             // 存放任务
  std::mutex mutex_;                   // 保护 queue_ 和 running_
  std::size_t max_queue_size_;         // 队列长度上限
  std::condition_variable not_empty_;  // 给消费者等
  std::condition_variable not_full_;   // 给生产者等
  std::vector<std::thread> threads_;   // 保存 Start 创建的线程
  bool running_;                       // 线程池是否还在工作
};

}  // namespace netlib
