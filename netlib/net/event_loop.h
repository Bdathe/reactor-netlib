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

  bool IsInLoopThread() const { return thread_id_ == std::this_thread::get_id(); }

  void AssertInLoopThread() const {
    if (!IsInLoopThread()) {
      AbortNotInLoopThread();
    }
  }

  void Loop();

  void Quit() { quit_ = true; }

  void UpdateChannel(Channel* channel);
  void RemoveChannel(Channel* channel);
  bool HasChannel(Channel* channel) const;

private:
  void AbortNotInLoopThread() const;
  void AbortChannelNotInLoop() const;

private:
  bool looping_;
  std::atomic<bool> quit_;
  const std::thread::id thread_id_;
  std::unique_ptr<Poller> poller_;
  std::vector<Channel*> active_channels_;
};

}  // namespace netlib
