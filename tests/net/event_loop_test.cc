#include "netlib/net/event_loop.h"

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>
#include <thread>

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
