#include "netlib/net/timer_queue.h"

#include <gtest/gtest.h>

#include <thread>

#include "netlib/net/event_loop.h"

namespace netlib {

namespace {

using namespace std::chrono_literals;

TEST(TimerQueueTest, RunAfterFiresOnce) {
  EventLoop loop;

  int count = 0;
  loop.RunAfter(0ms, [&] { ++count; });  // 顺便验证 0ms 的 100ns 保底分支
  loop.RunAfter(15ms, [&] { loop.Quit(); });

  loop.Loop();

  EXPECT_EQ(count, 1);
}

TEST(TimerQueueTest, FiresInExpirationOrder) {
  EventLoop loop;

  int seq = 0;
  int task1_seq = 0;
  int task2_seq = 0;
  int task3_seq = 0;

  loop.RunAfter(30ms, [&] {
    task1_seq = ++seq;
    loop.Quit();
  });
  loop.RunAfter(10ms, [&] { task2_seq = ++seq; });
  loop.RunAfter(20ms, [&] { task3_seq = ++seq; });

  loop.Loop();

  EXPECT_EQ(task1_seq, 3);
  EXPECT_EQ(task2_seq, 1);
  EXPECT_EQ(task3_seq, 2);
}

TEST(TimerQueueTest, SameExpirationFiresInFifoOrder) {
  EventLoop loop;

  EventLoop::TimePoint when = EventLoop::Clock::now() + 10ms;
  int seq = 0;
  int task1_seq = 0;
  int task2_seq = 0;
  int task3_seq = 0;

  loop.RunAt(when, [&] { task1_seq = ++seq; });
  loop.RunAt(when, [&] { task2_seq = ++seq; });
  loop.RunAt(when, [&] {
    task3_seq = ++seq;
    loop.Quit();
  });

  loop.Loop();

  EXPECT_EQ(task1_seq, 1);
  EXPECT_EQ(task2_seq, 2);
  EXPECT_EQ(task3_seq, 3);
}

TEST(TimerQueueTest, RunEveryAndSelfCancel) {
  EventLoop loop;

  TimerId id;
  int count = 0;

  id = loop.RunEvery(10ms, [&] {
    if (++count == 3) {
      loop.Cancel(id);
    }
  });

  loop.RunAfter(55ms, [&] { loop.Quit(); });

  loop.Loop();

  EXPECT_EQ(count, 3);
}

TEST(TimerQueueTest, CancelPreventsExecution) {
  EventLoop loop;

  bool called = false;
  TimerId id = loop.RunAfter(10ms, [&] { called = true; });
  loop.Cancel(id);
  loop.Cancel(id);  // 重复取消应无副作用

  loop.RunAfter(25ms, [&] { loop.Quit(); });

  loop.Loop();

  EXPECT_FALSE(called);
}

TEST(TimerQueueTest, AddAndCancelFromOtherThread) {
  EventLoop loop;

  std::thread::id main_id = std::this_thread::get_id();
  std::thread::id ran_id{};
  bool called = false;
  std::thread t([&] {
    TimerId id = loop.RunAfter(10ms, [&] { called = true; });
    loop.Cancel(id);
    loop.RunAfter(25ms, [&] {
      ran_id = std::this_thread::get_id();
      loop.Quit();
    });
  });

  loop.Loop();

  t.join();

  EXPECT_FALSE(called);
  EXPECT_EQ(main_id, ran_id);
}

}  // namespace

}  // namespace netlib
