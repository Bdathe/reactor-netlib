/**
 * @file event_loop.h
 * @brief 事件循环：循环等待事件，并分发给活跃的 Channel
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "netlib/base/noncopyable.h"

namespace netlib {

// 前置声明
class Channel;
class Poller;

class EventLoop : Noncopyable {
public:
  // 投递给 loop 线程执行的任务
  using Functor = std::function<void()>;

  EventLoop();
  ~EventLoop();

  /// 当前线程是不是创建这个 loop 的线程
  bool IsInLoopThread() const { return thread_id_ == std::this_thread::get_id(); }

  /// 不在 loop 线程时终止程序，用在只能由 loop 线程调用的函数开头
  void AssertInLoopThread() const {
    if (!IsInLoopThread()) {
      AbortNotInLoopThread();
    }
  }

  /// 事件循环：Poll → 逐个 HandleEvent，直到 Quit。只能在 loop 线程调用，不可重入
  void Loop();

  /// 请求退出，当前这一轮处理完后 Loop 返回
  void Quit() { quit_ = true; }

  /// 转发给 Poller，只能在 loop 线程调用，channel 必须属于这个 loop
  void UpdateChannel(Channel* channel);
  void RemoveChannel(Channel* channel);
  bool HasChannel(Channel* channel) const;

  /// 调用线程是这个 loop 线程时，直接执行 cb；否则交给 QueueInLoop
  /// 任何线程都可以调用
  void RunInLoop(Functor cb);

  /// 把 cb 放入队列，由 loop 线程在下一次 DoPendingFunctors 时执行
  /// 任何线程都可以调用
  void QueueInLoop(Functor cb);

private:
  void AbortNotInLoopThread() const;
  void AbortChannelNotInLoop() const;

  /// 执行队列里的任务，只在 loop 线程调用
  void DoPendingFunctors();

private:
  bool looping_;                           // 是否正在 Loop 中
  std::atomic<bool> quit_;                 // 退出标志，可能由其他线程设置
  const std::thread::id thread_id_;        // 创建这个 loop 的线程
  std::unique_ptr<Poller> poller_;         // IO 多路复用
  std::vector<Channel*> active_channels_;  // 本轮的活跃 Channel
  std::mutex mutex_;                       // 保护 pending_functors_
  std::vector<Functor> pending_functors_;  // 任务队列
};

}  // namespace netlib
