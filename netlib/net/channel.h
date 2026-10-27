/**
 * @file channel.h
 * @brief 把 fd 上发生的事件分发给对应的回调
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "netlib/base/noncopyable.h"

namespace netlib {

// 前置声明
class EventLoop;
class Timestamp;

class Channel : Noncopyable {
public:
  /// Channel 在 Poller 中的状态，由 Poller 读写，Poller 据此选择 epoll_ctl 的操作
  enum class PollerState {
    kNew,      ///< 从未交给 Poller：不在映射表中，也不在 epoll 上
    kAdded,    ///< 在映射表中，也在 epoll 上
    kDeleted,  ///< 仍在映射表中，只是从 epoll 上移除（不关注任何事件时）
  };

  using EventCallback = std::function<void()>;
  using ReadEventCallback = std::function<void(Timestamp)>;

  Channel(EventLoop* loop, int fd)
      : loop_(loop),
        fd_(fd),
        interested_events_(0),
        ready_events_(0),
        poller_state_(PollerState::kNew),
        tied_(false) {}

  void set_read_callback(ReadEventCallback callback) { read_callback_ = std::move(callback); }
  void set_write_callback(EventCallback callback) { write_callback_ = std::move(callback); }
  void set_close_callback(EventCallback callback) { close_callback_ = std::move(callback); }
  void set_error_callback(EventCallback callback) { error_callback_ = std::move(callback); }

  int fd() const { return fd_; }
  uint32_t interested_events() const { return interested_events_; }
  PollerState poller_state() const { return poller_state_; }

  void set_poller_state(PollerState poller_state) { poller_state_ = poller_state; }
  void set_ready_events(uint32_t ready_events) { ready_events_ = ready_events; }

  void EnableReading();
  void DisableReading();
  void EnableWriting();
  void DisableWriting();
  void DisableAll();

  bool IsNoneEvent() const;
  bool IsWriting() const;
  bool IsReading() const;

  void HandleEvent(Timestamp receive_time);

  void Tie(const std::shared_ptr<void>& owner);

  void Remove();

private:
  void Update();

  void HandleEventWithGuard(Timestamp receive_time);

private:
  EventLoop* loop_;
  const int fd_;
  uint32_t interested_events_;
  uint32_t ready_events_;
  PollerState poller_state_;
  ReadEventCallback read_callback_;
  EventCallback write_callback_;
  EventCallback close_callback_;
  EventCallback error_callback_;
  std::weak_ptr<void> tie_;
  bool tied_;
};

}  // namespace netlib
