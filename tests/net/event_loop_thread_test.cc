#include "netlib/net/event_loop_thread.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "netlib/net/event_loop.h"

namespace netlib {

namespace {

TEST(EventLoopThreadTest, StartLoopReturnsLoopRunningInNewThread) {
  EventLoopThread loop_thread;

  EventLoop* loop = loop_thread.StartLoop();
  ASSERT_NE(loop, nullptr);

  std::promise<std::thread::id> promise;
  std::future<std::thread::id> future = promise.get_future();

  loop->RunInLoop([&] { promise.set_value(std::this_thread::get_id()); });

  std::thread::id io_id = future.get();
  EXPECT_NE(io_id, std::this_thread::get_id());
}

TEST(EventLoopThreadTest, DestructorQuitsLoopPromptly) {
  std::chrono::steady_clock::time_point start;
  {
    EventLoopThread loop_thread;
    loop_thread.StartLoop();
    start = std::chrono::steady_clock::now();
  }  // 析构：Quit + join
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(1));
}

}  // namespace

}  // namespace netlib
