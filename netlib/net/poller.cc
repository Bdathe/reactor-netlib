/**
 * @file poller.cc
 * @brief Poller 的实现
 */

#include "netlib/net/poller.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>

#include "netlib/base/logging.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/channel.h"
#include "netlib/net/event_loop.h"

namespace netlib {

namespace {

constexpr int kInitEventListSize = 16;

}  // namespace

Poller::Poller(EventLoop* loop)
    : owner_loop_(loop), epfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kInitEventListSize) {
  if (epfd_ == -1) {
    LOG_SYSFATAL << "epoll_create1 failed";
  }
}

Poller::~Poller() {
  if (::close(epfd_) == -1) {
    LOG_SYSERR << "close failed";
  }
}

Timestamp Poller::Poll(int timeout_ms, ChannelList* active_channels) {
  int n = ::epoll_wait(epfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
  int saved_errno = errno;

  Timestamp ret = Timestamp::Now();

  if (n > 0) {
    FillActiveChannels(n, active_channels);
    if (n == static_cast<int>(events_.size())) {
      events_.resize(events_.size() * 2);
    }
  } else if (n == 0) {
    LOG_TRACE << "no events";
  } else {
    if (saved_errno != EINTR) {
      errno = saved_errno;
      LOG_SYSERR << "epoll_wait failed";
    }
  }

  return ret;
}

void Poller::UpdateChannel(Channel* channel) {
  owner_loop_->AssertInLoopThread();

  Channel::PollerState state = channel->poller_state();

  if (state == Channel::PollerState::kNew) {
    int fd = channel->fd();
    channels_[fd] = channel;
    UpdateEpoll(EPOLL_CTL_ADD, channel);
    channel->set_poller_state(Channel::PollerState::kAdded);
  } else if (state == Channel::PollerState::kDeleted) {
    UpdateEpoll(EPOLL_CTL_ADD, channel);
    channel->set_poller_state(Channel::PollerState::kAdded);
  } else if (state == Channel::PollerState::kAdded) {
    if (channel->IsNoneEvent()) {
      UpdateEpoll(EPOLL_CTL_DEL, channel);
      channel->set_poller_state(Channel::PollerState::kDeleted);
    } else {
      UpdateEpoll(EPOLL_CTL_MOD, channel);
    }
  }
}

void Poller::RemoveChannel(Channel* channel) {
  owner_loop_->AssertInLoopThread();

  int fd = channel->fd();
  channels_.erase(fd);

  Channel::PollerState state = channel->poller_state();
  if (state == Channel::PollerState::kAdded) {
    UpdateEpoll(EPOLL_CTL_DEL, channel);
  }

  channel->set_poller_state(Channel::PollerState::kNew);
}

bool Poller::HasChannel(Channel* channel) const {
  owner_loop_->AssertInLoopThread();

  int fd = channel->fd();
  auto it = channels_.find(fd);

  return it != channels_.end() && it->second == channel;
}

void Poller::FillActiveChannels(int num_events, ChannelList* active_channels) const {
  for (int i = 0; i < num_events; ++i) {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
    channel->set_ready_events(events_[i].events);
    active_channels->push_back(channel);
  }
}

void Poller::UpdateEpoll(int operation, Channel* channel) {
  epoll_event event{};
  event.events = channel->interested_events();
  event.data.ptr = channel;
  int fd = channel->fd();

  if (::epoll_ctl(epfd_, operation, fd, &event) == -1) {
    if (operation == EPOLL_CTL_DEL) {
      LOG_SYSERR << "epoll_ctl DEL failed, fd = " << fd;
    } else if (operation == EPOLL_CTL_ADD) {
      LOG_SYSFATAL << "epoll_ctl ADD failed, fd = " << fd;
    } else if (operation == EPOLL_CTL_MOD) {
      LOG_SYSFATAL << "epoll_ctl MOD failed, fd = " << fd;
    }
  }
}

}  // namespace netlib
