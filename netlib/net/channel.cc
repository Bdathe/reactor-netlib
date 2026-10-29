/**
 * @file channel.cc
 * @brief Channel 的实现
 */

#include "netlib/net/channel.h"

#include <sys/epoll.h>

#include <memory>

#include "netlib/base/timestamp.h"
#include "netlib/net/event_loop.h"

namespace netlib {

namespace {

constexpr uint32_t kNoneEvent = 0;
constexpr uint32_t kReadEvent = EPOLLIN | EPOLLPRI;
constexpr uint32_t kWriteEvent = EPOLLOUT;

}  // namespace

void Channel::EnableReading() {
  interested_events_ |= kReadEvent;
  Update();
}

void Channel::DisableReading() {
  interested_events_ &= ~kReadEvent;
  Update();
}

void Channel::EnableWriting() {
  interested_events_ |= kWriteEvent;
  Update();
}

void Channel::DisableWriting() {
  interested_events_ &= ~kWriteEvent;
  Update();
}

void Channel::DisableAll() {
  interested_events_ = kNoneEvent;
  Update();
}

bool Channel::IsNoneEvent() const {
  return interested_events_ == kNoneEvent;
}

bool Channel::IsWriting() const {
  return interested_events_ & kWriteEvent;
}

bool Channel::IsReading() const {
  return interested_events_ & kReadEvent;
}

void Channel::Remove() {
  loop_->RemoveChannel(this);
}

void Channel::Update() {
  loop_->UpdateChannel(this);
}

void Channel::HandleEventWithGuard(Timestamp receive_time) {
  if ((ready_events_ & EPOLLHUP) && !(ready_events_ & EPOLLIN)) {
    if (close_callback_)
      close_callback_();
  }

  if (ready_events_ & EPOLLERR) {
    if (error_callback_)
      error_callback_();
  }

  if (ready_events_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
    if (read_callback_)
      read_callback_(receive_time);
  }

  if (ready_events_ & EPOLLOUT) {
    if (write_callback_)
      write_callback_();
  }
}

void Channel::Tie(const std::shared_ptr<void>& owner) {
  tie_ = owner;
  tied_ = true;
}

void Channel::HandleEvent(Timestamp receive_time) {
  if (tied_) {
    std::shared_ptr<void> guard = tie_.lock();
    if (guard) {
      HandleEventWithGuard(receive_time);
    }
  } else {
    HandleEventWithGuard(receive_time);
  }
}

}  // namespace netlib
