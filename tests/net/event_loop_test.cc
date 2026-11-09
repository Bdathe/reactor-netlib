#include "netlib/net/event_loop.h"

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "netlib/base/noncopyable.h"
#include "netlib/base/timestamp.h"
#include "netlib/net/channel.h"

namespace netlib {

namespace {

// 测试用的 eventfd：Notify() 后变为可读，Drain() 读出计数并恢复为不可读
class EventFd : Noncopyable {
public:
  EventFd() : fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
  ~EventFd() { ::close(fd_); }

  int fd() const { return fd_; }

  void Notify() {
    uint64_t one = 1;
    ASSERT_EQ(::write(fd_, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
  }

  uint64_t Drain() {
    uint64_t value = 0;
    return ::read(fd_, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)) ? value : 0;
  }

private:
  const int fd_;
};

TEST(EventLoopTest, ReadCallbackInvokedWhenReadable) {
  // EnableReading → epoll_ctl(ADD) → Notify → epoll_wait 返回
  //               → FillActiveChannels → HandleEvent → read 回调函数
  EventLoop loop;

  EventFd efd;
  Channel channel(&loop, efd.fd());

  bool called = false;
  channel.set_read_callback([&](Timestamp) {
    called = true;
    efd.Drain();  // 读取数据
    loop.Quit();  // Loop 返回
  });

  channel.EnableReading();  // 登记可读
  efd.Notify();             // efd 可读
  loop.Loop();

  EXPECT_TRUE(called);
  channel.DisableAll();
  channel.Remove();
}

TEST(EventLoopTest, NoCallbackAfterDisableReading) {
  EventLoop loop;

  EventFd target;
  EventFd stopper;
  Channel target_channel(&loop, target.fd());
  Channel stopper_channel(&loop, stopper.fd());

  bool called = false;
  target_channel.set_read_callback([&](Timestamp) { called = true; });

  stopper_channel.set_read_callback([&](Timestamp) {
    stopper.Drain();
    loop.Quit();
  });

  target_channel.EnableReading();
  stopper_channel.EnableReading();
  target_channel.DisableReading();

  target.Notify();
  stopper.Notify();

  loop.Loop();

  EXPECT_FALSE(called);

  target_channel.DisableAll();
  target_channel.Remove();
  stopper_channel.DisableAll();
  stopper_channel.Remove();
}

TEST(EventLoopTest, HasChannelTracksRegistration) {
  EventLoop loop;

  EventFd efd;
  Channel channel(&loop, efd.fd());

  channel.EnableReading();
  EXPECT_TRUE(loop.HasChannel(&channel));
  EXPECT_EQ(channel.poller_state(), Channel::PollerState::kAdded);

  channel.DisableAll();
  EXPECT_TRUE(loop.HasChannel(&channel));
  EXPECT_EQ(channel.poller_state(), Channel::PollerState::kDeleted);

  channel.Remove();
  EXPECT_FALSE(loop.HasChannel(&channel));
  EXPECT_EQ(channel.poller_state(), Channel::PollerState::kNew);
}

TEST(EventLoopTest, RunInLoopInLoopThreadRunsImmediately) {
  EventLoop loop;

  bool called = false;
  loop.RunInLoop([&] { called = true; });

  EXPECT_TRUE(called);  // 在 loop 线程中调用，RunInLoop 返回前已经执行
}

TEST(EventLoopTest, QueueInLoopDefersUntilLoop) {
  EventLoop loop;

  bool called = false;
  loop.QueueInLoop([&] {
    called = true;
    loop.Quit();
  });

  EXPECT_FALSE(called);  // 只是放进队列，还没有执行

  loop.Loop();

  EXPECT_TRUE(called);  // 在 Loop 的 DoPendingFunctors 中执行
}

TEST(EventLoopTest, RunInLoopFromOtherThreadRunsInLoopThread) {
  const std::thread::id main_id = std::this_thread::get_id();
  EventLoop loop;

  std::thread::id ran_in;
  std::thread t([&] {
    loop.RunInLoop([&] {
      ran_in = std::this_thread::get_id();
      loop.Quit();
    });
  });

  loop.Loop();
  t.join();

  EXPECT_EQ(ran_in, main_id);  // 由线程 t 投递，在 loop 线程中执行
}

TEST(EventLoopTest, QueueInLoopFromPendingFunctorRunsNextRound) {
  EventLoop loop;

  bool a_called = false;
  bool b_called = false;
  loop.QueueInLoop([&] {
    a_called = true;
    // 在 DoPendingFunctors 执行期间投递：如果持锁执行任务，这里会死锁
    loop.QueueInLoop([&] {
      b_called = true;
      loop.Quit();
    });
  });

  loop.Loop();

  EXPECT_TRUE(a_called);
  EXPECT_TRUE(b_called);
}

TEST(EventLoopTest, ConcurrentQueueInLoopRunsAllTasks) {
  constexpr int kThreads = 4;
  constexpr int kTasksPerThread = 1000;
  EventLoop loop;

  int count = 0;  // 不加锁：只在 loop 线程中访问，TSan 不应报告
  std::vector<std::thread> producers;
  for (int i = 0; i < kThreads; ++i) {
    producers.emplace_back([&] {
      for (int j = 0; j < kTasksPerThread; ++j) {
        loop.QueueInLoop([&] {
          if (++count == kThreads * kTasksPerThread) {
            loop.Quit();
          }
        });
      }
    });
  }

  loop.Loop();
  for (std::thread& t : producers) {
    t.join();
  }

  EXPECT_EQ(count, kThreads * kTasksPerThread);
}

TEST(EventLoopDeathTest, SecondLoopInSameThreadAborts) {
  EXPECT_DEATH(
      {
        EventLoop loop1;
        EventLoop loop2;
      },
      "second EventLoop");
}

TEST(EventLoopDeathTest, AssertInLoopThreadAbortsInOtherThread) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  EventLoop loop;

  EXPECT_DEATH(
      {
        std::thread t([&] { loop.AssertInLoopThread(); });
        t.join();
      },
      "owning thread");
}

}  // namespace

}  // namespace netlib
