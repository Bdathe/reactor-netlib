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
  EventLoop* owner_loop() const { return loop_; }

  void set_poller_state(PollerState poller_state) { poller_state_ = poller_state; }
  void set_ready_events(uint32_t ready_events) { ready_events_ = ready_events; }

  /// 修改关注的事件，并同步到 Poller（epoll_ctl）。只能在 loop 线程调用
  void EnableReading();
  void DisableReading();
  void EnableWriting();
  void DisableWriting();
  void DisableAll();

  bool IsNoneEvent() const;
  bool IsWriting() const;
  bool IsReading() const;

  /// 按 ready_events_ 分发到读/写/关闭/错误回调，由 EventLoop::Loop 调用。
  /// tie 过的 Channel 先把 tie_ 提升为 shared_ptr，所有者已析构时什么也不做
  void HandleEvent(Timestamp receive_time);

  /// 绑定所有者（TcpConnection），HandleEvent 期间持有它，防止回调中途被析构。
  /// 所有者必须已被 shared_ptr 管理，所以不能在其构造函数里调用
  void Tie(const std::shared_ptr<void>& owner);

  /// 从 Poller 中彻底移除，之后 Channel 可以安全析构。调用前应先 DisableAll
  void Remove();

private:
  /// 把 interested_events_ 同步到 Poller
  void Update();

  void HandleEventWithGuard(Timestamp receive_time);

private:
  EventLoop* loop_;                  // 所属的 loop
  const int fd_;                     // 不拥有，由所有者的 Socket 负责关闭
  uint32_t interested_events_;       // 关注的事件，交给 epoll_ctl
  uint32_t ready_events_;            // 本轮 epoll_wait 返回的事件
  PollerState poller_state_;         // 在 Poller 中的状态
  ReadEventCallback read_callback_;  // EPOLLIN | EPOLLPRI | EPOLLRDHUP
  EventCallback write_callback_;     // EPOLLOUT
  EventCallback close_callback_;     // EPOLLHUP 且没有 EPOLLIN
  EventCallback error_callback_;     // EPOLLERR
  std::weak_ptr<void> tie_;          // 所有者，不延长其生命周期
  bool tied_;                        // 是否调用过 Tie
};

}  // namespace netlib
