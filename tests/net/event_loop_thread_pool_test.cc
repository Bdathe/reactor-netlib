#include "netlib/net/event_loop_thread_pool.h"

#include <gtest/gtest.h>

#include <future>
#include <set>
#include <thread>
#include <vector>

#include "netlib/net/event_loop.h"

namespace netlib {

namespace {

TEST(EventLoopThreadPoolTest, ZeroThreadsReturnsBaseLoop) {
  EventLoop base_loop;
  EventLoopThreadPool pool(&base_loop);

  pool.SetThreadNum(0);
  pool.Start();

  ASSERT_EQ(pool.GetNextLoop(), &base_loop);
  ASSERT_EQ(pool.GetNextLoop(), &base_loop);
  ASSERT_EQ(pool.GetNextLoop(), &base_loop);
}

TEST(EventLoopThreadPoolTest, RoundRobinAcrossLoops) {
  EventLoop base_loop;
  EventLoopThreadPool pool(&base_loop);

  pool.SetThreadNum(3);
  pool.Start();

  std::vector<EventLoop*> loops;
  for (int i = 0; i < 6; ++i) {
    loops.push_back(pool.GetNextLoop());
  }

  EXPECT_NE(loops[0], &base_loop);
  EXPECT_NE(loops[1], &base_loop);
  EXPECT_NE(loops[2], &base_loop);

  EXPECT_NE(loops[0], loops[1]);
  EXPECT_NE(loops[1], loops[2]);
  EXPECT_NE(loops[2], loops[0]);

  EXPECT_EQ(loops[3], loops[0]);
  EXPECT_EQ(loops[4], loops[1]);
  EXPECT_EQ(loops[5], loops[2]);
}

TEST(EventLoopThreadPoolTest, EachLoopRunsInItsOwnThread) {
  constexpr int kThreads = 3;
  EventLoop base_loop;
  EventLoopThreadPool pool(&base_loop);
  pool.SetThreadNum(kThreads);
  pool.Start();

  // 往每个 IO loop 投递一个任务，记录它实际在哪个线程上执行
  std::vector<std::promise<std::thread::id>> promises(kThreads);
  std::vector<std::future<std::thread::id>> futures;
  for (int i = 0; i < kThreads; ++i) {
    futures.push_back(promises[i].get_future());
    EventLoop* loop = pool.GetNextLoop();
    loop->RunInLoop([&promises, i] { promises[i].set_value(std::this_thread::get_id()); });
  }

  std::set<std::thread::id> ids;
  for (auto& f : futures) {
    ids.insert(f.get());
  }

  EXPECT_EQ(ids.size(), static_cast<std::size_t>(kThreads));  // 3 个 id 两两不同
  EXPECT_EQ(ids.count(std::this_thread::get_id()), 0U);       // 都不是 base loop 线程
}

}  // namespace

}  // namespace netlib
