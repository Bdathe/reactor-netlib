/**
 * @file event_loop.cc
 * @brief EventLoop 的实现
 */

#include "netlib/net/event_loop.h"

#include <memory>
#include <thread>

#include "netlib/base/logging.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/channel.h"
#include "netlib/net/poller.h"

namespace netlib {

namespace {

// 保证一个线程最多一个 EventLoop
// 用一个线程局部变量记住本线程已经有的 EventLoop
thread_local EventLoop* t_loop_in_this_thread = nullptr;

constexpr int kPollTimeMs = 10000;  // 10s

}  // namespace

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      thread_id_(std::this_thread::get_id()),
      poller_(std::make_unique<Poller>(this)) {
  if (t_loop_in_this_thread) {
    LOG_FATAL << "attempt to create a second EventLoop in a single thread";
  }

  t_loop_in_this_thread = this;
}

EventLoop::~EventLoop() {
  t_loop_in_this_thread = nullptr;
}

void EventLoop::Loop() {
  AssertInLoopThread();

  if (looping_) {
    LOG_FATAL << "cannot enter the Loop more than once";
  }

  looping_ = true;
  quit_ = false;

  while (!quit_) {
    // 清理掉上一轮的活跃 channels
    active_channels_.clear();
    Timestamp receive_time = poller_->Poll(kPollTimeMs, &active_channels_);

    for (Channel* channel : active_channels_) {
      channel->HandleEvent(receive_time);
    }
  }

  looping_ = false;
}

void EventLoop::UpdateChannel(Channel* channel) {
  AssertInLoopThread();
  if (channel->owner_loop() != this) {
    AbortChannelNotInLoop();
  }

  poller_->UpdateChannel(channel);
}

void EventLoop::RemoveChannel(Channel* channel) {
  AssertInLoopThread();
  if (channel->owner_loop() != this) {
    AbortChannelNotInLoop();
  }

  poller_->RemoveChannel(channel);
}

bool EventLoop::HasChannel(Channel* channel) const {
  AssertInLoopThread();
  if (channel->owner_loop() != this) {
    AbortChannelNotInLoop();
  }

  return poller_->HasChannel(channel);
}

void EventLoop::AbortNotInLoopThread() const {
  LOG_FATAL << "this thread ID = " << std::this_thread::get_id()
            << ", EventLoop's owning thread ID = " << thread_id_;
}

void EventLoop::AbortChannelNotInLoop() const {
  LOG_FATAL << "the channel does not belong to this EventLoop";
}

}  // namespace netlib
