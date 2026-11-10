/**
 * @file event_loop.cc
 * @brief EventLoop 的实现
 */

#include "netlib/net/event_loop.h"

#include <signal.h>
#include <sys/eventfd.h>

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

class IgnoreSigPipe {
public:
  IgnoreSigPipe() { ::signal(SIGPIPE, SIG_IGN); }
};

// 收到对端 RST 之后再 write，内核会发送 SIGPIPE，默认动作是终止进程
// 忽略后，write 改为返回 -1、errno 为 EPIPE，由 SendInLoop 设置 fault_error 处理
// 放在 event_loop.cc 里：每个程序都要创建 EventLoop，链接器一定会拉入 event_loop.o，
// 这个全局对象才会在 main 之前构造（放进单独的 .o，静态库链接时不会被拉入）
IgnoreSigPipe ignore_sigpipe;

}  // namespace

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      thread_id_(std::this_thread::get_id()),
      poller_(std::make_unique<Poller>(this)),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      wakeup_channel_(std::make_unique<Channel>(this, wakeup_fd_)),
      calling_pending_functors_(false) {
  if (t_loop_in_this_thread) {
    LOG_FATAL << "cannot create a second EventLoop in this thread";
  }

  if (wakeup_fd_ == -1) {
    LOG_SYSFATAL << "eventfd failed";
  }

  t_loop_in_this_thread = this;

  wakeup_channel_->set_read_callback([this](Timestamp /*receive_time*/) { HandleWakeupRead(); });
  wakeup_channel_->EnableReading();
}

EventLoop::~EventLoop() {
  t_loop_in_this_thread = nullptr;

  wakeup_channel_->DisableAll();
  wakeup_channel_->Remove();

  if (::close(wakeup_fd_) == -1) {
    LOG_SYSERR << "close failed, wakeup_fd=" << wakeup_fd_;
  }
}

void EventLoop::Loop() {
  AssertInLoopThread();

  if (looping_) {
    LOG_FATAL << "Loop() is already running";
  }

  looping_ = true;

  while (!quit_) {
    // 清理掉上一轮的活跃 channels
    active_channels_.clear();
    Timestamp receive_time = poller_->Poll(kPollTimeMs, &active_channels_);

    for (Channel* channel : active_channels_) {
      channel->HandleEvent(receive_time);
    }

    DoPendingFunctors();
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

void EventLoop::RunInLoop(Functor cb) {
  if (IsInLoopThread()) {
    cb();
  } else {
    QueueInLoop(std::move(cb));
  }
}

void EventLoop::QueueInLoop(Functor cb) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_functors_.push_back(std::move(cb));
  }

  // 下一步会先进入 Poll 的情况都要唤醒：
  // 1. 别的线程投递：loop 线程可能正阻塞在 epoll_wait
  // 2. 还没进入 Loop（或已退出）：进入 Loop 后第一步就是 Poll
  // 3. 在 DoPendingFunctors 中投递：这一轮已经 swap 过，新任务要等下一轮
  // 在 HandleEvent 回调中投递不用唤醒：回调返回后紧接着就是 DoPendingFunctors
  // IsInLoopThread() 必须放在最前面：looping_ 和 calling_pending_functors_ 不是 atomic，
  // 靠短路求值保证只有 loop 线程会读它们
  if (!IsInLoopThread() || !looping_ || calling_pending_functors_) {
    Wakeup();
  }
}

void EventLoop::Wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));

  if (n != sizeof(one)) {
    LOG_SYSERR << "write failed, wakeup_fd=" << wakeup_fd_;
  }
}

void EventLoop::HandleWakeupRead() {
  uint64_t count{};
  ssize_t n = ::read(wakeup_fd_, &count, sizeof(count));

  if (n != sizeof(count)) {
    LOG_SYSERR << "read failed, wakeup_fd=" << wakeup_fd_;
  }
}

void EventLoop::DoPendingFunctors() {
  calling_pending_functors_ = true;

  std::vector<Functor> functors;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    functors.swap(pending_functors_);
  }

  for (const Functor& functor : functors) {
    functor();
  }

  calling_pending_functors_ = false;
}

void EventLoop::AbortNotInLoopThread() const {
  LOG_FATAL << "EventLoop used outside its owning thread, owning thread=" << thread_id_
            << ", current thread=" << std::this_thread::get_id();
}

void EventLoop::AbortChannelNotInLoop() const {
  LOG_FATAL << "channel does not belong to this EventLoop";
}

}  // namespace netlib
