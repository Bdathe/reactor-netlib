/**
 * @file timer_queue.h
 * @brief 基于 timerfd 和最小堆的定时器管理，支持单次/周期定时与延迟删除取消
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <queue>
#include <unordered_set>
#include <vector>

#include "netlib/base/noncopyable.h"
#include "netlib/net/callbacks.h"
#include "netlib/net/channel.h"
#include "netlib/net/timer_id.h"

namespace netlib {

// 前置声明
class EventLoop;

class TimerQueue : Noncopyable {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Duration = Clock::duration;

  explicit TimerQueue(EventLoop* loop);
  ~TimerQueue();

  /// 添加定时任务，返回取消凭证 TimerId
  /// interval > Duration::zero() 时周期触发，否则单次触发
  /// 第一次在 when 时调用，之后间隔 interval 调用
  /// 任意线程都可以调用
  TimerId AddTimer(TimerCallback cb, TimePoint when, Duration interval);

  /// 取消 timer_id 对应的定时器（已到期或者已取消时无效果）
  /// 任意线程都可以调用
  void Cancel(TimerId timer_id);

private:
  struct Timer {
    TimePoint expiration;
    Duration interval;
    uint64_t sequence;
    TimerCallback callback;
  };

  struct TimerCompare {
    // priority_queue 默认是大根堆
    // 当 comp(lhs, rhs) 返回 true 时，lhs 会沉到 rhs
    // 这里我们要使用小根堆，即最早到期的在堆顶部
    bool operator()(const Timer& lhs, const Timer& rhs) const {
      return (lhs.expiration > rhs.expiration) ||
             (lhs.expiration == rhs.expiration && lhs.sequence > rhs.sequence);
    }
  };

private:
  /// 将定时器加入最小堆和有效集合，必要时更新 timerfd
  /// 只能在 loop 线程调用
  void AddTimerInLoop(Timer timer);
  /// 从有效集合中移除序号（延迟删除）
  /// 只能在 loop 线程调用
  void CancelInLoop(TimerId timer_id);
  /// timer_fd_ 的可读回调，读走计数器并执行所有已经到期而且未取消的回调，
  /// 处理周期重入堆与重设 timerfd
  /// 只能在 loop 线程调用
  void HandleRead();
  /// 调用 timerfd_settime 将内核下次响铃时间设为 expiration
  /// 只能在 loop 线程调用
  void ResetTimerfd(TimePoint expiration);

private:
  EventLoop* loop_;
  const int timer_fd_;
  Channel timer_channel_;
  std::priority_queue<Timer, std::vector<Timer>, TimerCompare> timers_;
  std::unordered_set<uint64_t> active_timers_;
  std::atomic<uint64_t> next_sequence_;
};

}  // namespace netlib
