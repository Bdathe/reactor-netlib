/**
 * @file timer_queue.cc
 * @brief TimerQueue 的实现
 */

#include "netlib/net/timer_queue.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <utility>

#include "netlib/base/logging.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/event_loop.h"

namespace netlib {

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timer_fd_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
      timer_channel_(loop, timer_fd_),
      next_sequence_(1) {
  if (timer_fd_ == -1) {
    LOG_SYSFATAL << "timerfd_create failed";
  }

  timer_channel_.set_read_callback([this](Timestamp /*receive_time*/) { HandleRead(); });
  timer_channel_.EnableReading();
}

TimerQueue::~TimerQueue() {
  timer_channel_.DisableAll();
  timer_channel_.Remove();
  if (::close(timer_fd_) == -1) {
    LOG_SYSERR << "close failed, timer_fd=" << timer_fd_;
  }
}

TimerId TimerQueue::AddTimer(TimerCallback cb, TimePoint when, Duration interval) {
  // fetch_add 返回自增之前的值
  uint64_t seq = next_sequence_.fetch_add(1, std::memory_order_relaxed);

  Timer timer{when, interval, seq, std::move(cb)};

  loop_->RunInLoop(
      [this, timer = std::move(timer)]() mutable { AddTimerInLoop(std::move(timer)); });

  return TimerId(seq);
}

void TimerQueue::Cancel(TimerId timer_id) {
  loop_->RunInLoop([this, timer_id] { CancelInLoop(timer_id); });
}

void TimerQueue::CancelInLoop(TimerId timer_id) {
  loop_->AssertInLoopThread();

  active_timers_.erase(timer_id.sequence());
}

void TimerQueue::AddTimerInLoop(Timer timer) {
  loop_->AssertInLoopThread();

  bool earliest_changed = false;
  if (timers_.empty() || timer.expiration < timers_.top().expiration) {
    earliest_changed = true;
  }

  TimePoint expiration = timer.expiration;

  active_timers_.insert(timer.sequence);
  timers_.push(std::move(timer));

  if (earliest_changed) {
    ResetTimerfd(expiration);
  }
}

void TimerQueue::HandleRead() {
  loop_->AssertInLoopThread();

  // 清零内核计数器
  uint64_t count{};
  ssize_t n = ::read(timer_fd_, &count, sizeof(count));

  if (n != sizeof(count)) {
    LOG_SYSERR << "read failed, timer_fd=" << timer_fd_;
  }

  TimePoint now = Clock::now();

  std::vector<Timer> expired{};
  while (!timers_.empty() && timers_.top().expiration <= now) {
    Timer timer = timers_.top();
    timers_.pop();

    // 是否已经被延迟删除
    if (active_timers_.count(timer.sequence)) {
      // 单次定时任务
      if (timer.interval <= Duration::zero()) {
        active_timers_.erase(timer.sequence);
      }

      expired.push_back(std::move(timer));
    }
  }

  // 到期定时任务逐个执行回调
  for (Timer& timer : expired) {
    if (timer.callback) {
      timer.callback();
    }

    // 周期定时器，而且回调执行期间没有自己 Cancel 自己
    if (timer.interval > Duration::zero() && active_timers_.count(timer.sequence)) {
      timer.expiration = now + timer.interval;
      timers_.push(std::move(timer));
    }
  }

  if (!timers_.empty()) {
    ResetTimerfd(timers_.top().expiration);
  }
}

void TimerQueue::ResetTimerfd(TimePoint expiration) {
  loop_->AssertInLoopThread();

  // 传入的是一个时间点，但是 timerfd_settime 中标志传入的是 0
  // 表示再过 x 时候后，闹钟会响，所以这里要计算 diff
  Duration diff = expiration - Clock::now();
  // itimerspec 中的 it_value 的 tv_sec 和 tv_nsec 同时为 0，timerfd_settime
  // 的语义是停掉闹钟，这里要做兜底处理
  if (diff < std::chrono::nanoseconds(100)) {
    diff = std::chrono::nanoseconds(100);
  }

  auto secs = std::chrono::duration_cast<std::chrono::seconds>(diff);
  auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(diff - secs);

  itimerspec ts{};
  ts.it_interval = {0, 0};
  ts.it_value = {secs.count(), nsecs.count()};

  if (::timerfd_settime(timer_fd_, 0, &ts, nullptr) == -1) {
    LOG_SYSERR << "timerfd_settime failed, timer_fd=" << timer_fd_;
  }
}

}  // namespace netlib
