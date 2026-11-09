/**
 * @file event_loop.h
 * @brief 事件循环：循环等待事件，并分发给活跃的 Channel
 */

#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "netlib/base/noncopyable.h"

namespace netlib {

// 前置声明
class Channel;
class Poller;

class EventLoop : Noncopyable {
public:
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

private:
  void AbortNotInLoopThread() const;
  void AbortChannelNotInLoop() const;

private:
  bool looping_;                           // 是否正在 Loop 中
  std::atomic<bool> quit_;                 // 退出标志，可能由其他线程设置
  const std::thread::id thread_id_;        // 创建这个 loop 的线程
  std::unique_ptr<Poller> poller_;         // IO 多路复用
  std::vector<Channel*> active_channels_;  // 本轮的活跃 Channel
};

}  // namespace netlib
