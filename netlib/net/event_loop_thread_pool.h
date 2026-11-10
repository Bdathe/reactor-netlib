/**
 * @file event_loop_thread_pool.h
 * @brief EventLoop 线程池，管理 IO 线程
 */

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "netlib/base/noncopyable.h"

namespace netlib {

// 前置声明
class EventLoop;
class EventLoopThread;

class EventLoopThreadPool : Noncopyable {
public:
  explicit EventLoopThreadPool(EventLoop* base_loop);
  /// threads_ 析构时，逐个 Quit 并 join 每个 IO 线程
  ~EventLoopThreadPool();

  /// 在 Start 之前设置线程数量
  /// 0 表示不创建 IO 线程，所有连接都在 base loop 上
  void SetThreadNum(int num_threads);

  /// 创建 num_threads_ 个 EventLoopThread，逐个调用 StartLoop，收集 loop 指针
  /// 只能在 base loop 线程调用
  /// 只能调用一次
  void Start();

  /// round-robin 返回下一个 loop，num_threads_ 为 0 的时候返回 base_loop_
  /// 只能在 base loop 线程调用
  EventLoop* GetNextLoop();

private:
  EventLoop* base_loop_;                                   // 主 loop
  bool started_;                                           // 是否已经 Start
  int num_threads_;                                        // 线程数
  std::size_t next_;                                       // 下一个要分配的下标
  std::vector<std::unique_ptr<EventLoopThread>> threads_;  // 拥有的线程
  std::vector<EventLoop*> loops_;                          // 每个线程里的 loop 指针，不拥有
};

}  // namespace netlib
