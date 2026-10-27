/**
 * @file poller.h
 * @brief 封装 epoll：登记 Channel 关注的事件，等待并返回活跃的 Channel
 *
 * 只有 Poller 调用 epoll_*，EventLoop 不知道 epoll 的存在
 */

#pragma once

#include <sys/epoll.h>

#include <unordered_map>
#include <vector>

#include "netlib/base/noncopyable.h"
#include "netlib/base/timestamp.h"

namespace netlib {

// 前置声明
class EventLoop;
class Channel;

class Poller : Noncopyable {
public:
  using ChannelList = std::vector<Channel*>;

  explicit Poller(EventLoop* loop);
  ~Poller();

  /// 等待事件，把活跃的 Channel 填入 active_channels，返回 epoll_wait 返回的时刻
  Timestamp Poll(int timeout_ms, ChannelList* active_channels);

  /// 根据 channel 关注的事件，在 epoll 上添加、修改或移除它
  void UpdateChannel(Channel* channel);

  /// 把 channel 从映射表和 epoll 上彻底移除，之后 Channel 可以安全析构
  void RemoveChannel(Channel* channel);

  /// channel 是否由这个 Poller 管理（在映射表中）
  bool HasChannel(Channel* channel) const;

private:
  void FillActiveChannels(int num_events, ChannelList* active_channels) const;

  /// 对 channel 执行一次 epoll_ctl，operation 为 EPOLL_CTL_ADD / MOD / DEL
  void UpdateEpoll(int operation, Channel* channel);

private:
  EventLoop* owner_loop_;
  const int epfd_;
  std::unordered_map<int, Channel*> channels_;
  std::vector<epoll_event> events_;
};

}  // namespace netlib
