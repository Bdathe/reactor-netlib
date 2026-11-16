#include "netlib/base/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <unistd.h>

namespace netlib {

TEST(ThreadPoolTest, TasksCompleteBeforeDestructorReturns) {
  std::atomic<int> cnt{0};
  {
    ThreadPool pool("test-threadpool");
    pool.SetMaxQueueSize(10);
    pool.Start(2);

    for (int i = 0; i < 10; ++i) {
      pool.Run([&cnt] { ++cnt; });
    }
  }
  EXPECT_EQ(cnt, 10);
}

TEST(ThreadPoolTest, RunBlocksWhileQueueIsFull) {
  // 卡住时不能无限等：工作线程停在 A 里，或 C 的 Run 没被叫醒
  ::alarm(10);

  std::mutex mu;
  std::condition_variable cv;
  bool started = false;  // A 已经占住工作线程
  bool release = false;  // 允许 A 返回

  std::atomic<int> cnt{0};
  std::atomic<bool> run_returned{false};

  {
    ThreadPool pool("test-threadpool");
    pool.SetMaxQueueSize(1);
    pool.Start(1);

    // A：唯一的工作线程取出后停在这里，不再 Take
    pool.Run([&] {
      std::unique_lock<std::mutex> lock(mu);
      started = true;
      cv.notify_one();
      cv.wait(lock, [&] { return release; });
    });

    {
      std::unique_lock<std::mutex> lock(mu);
      cv.wait(lock, [&] { return started; });
    }

    // B：占住队列里唯一的空位。队列此时是空的，这次 Run 会返回
    pool.Run([&] { ++cnt; });

    // C：队列已满，Run 睡在 not_full_ 上，不返回
    std::thread t([&] {
      pool.Run([&] { ++cnt; });
      run_returned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(run_returned.load());

    {
      std::lock_guard<std::mutex> lock(mu);
      release = true;
    }
    cv.notify_one();

    t.join();
    EXPECT_TRUE(run_returned.load());
  }
  EXPECT_EQ(cnt.load(), 2);
}

TEST(ThreadPoolTest, RunDropsTaskOnceShutdownStarts) {
  // A 最多睡 200ms；C 如果没被析构叫醒，join 会一直等
  ::alarm(10);

  std::atomic<bool> a_started{false};
  std::atomic<int> cnt{0};
  std::atomic<bool> run_returned{false};

  auto pool = std::make_unique<ThreadPool>("test-threadpool");
  pool->SetMaxQueueSize(1);
  pool->Start(1);

  // A 自己限时睡，析构里的 join 最多等这么久。不能再等测试线程放行
  pool->Run([&] {
    a_started = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  });
  while (!a_started.load()) {
    std::this_thread::yield();
  }

  // B 已经在队列里，析构时仍要执行
  pool->Run([&] { ++cnt; });

  // C 堵在满队列上。先析构池，这次 Run 看到 running_ == false 就丢掉任务
  std::thread t([&] {
    pool->Run([&] { ++cnt; });
    run_returned = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(run_returned.load());

  pool.reset();
  t.join();

  EXPECT_TRUE(run_returned.load());
  EXPECT_EQ(cnt.load(), 1);
}

}  // namespace netlib
