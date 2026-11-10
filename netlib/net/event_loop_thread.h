/**
 * @file event_loop_thread.h
 * @brief 在新线程中运行一个 EventLoop，并把它的指针交给创建者
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

#include "netlib/base/noncopyable.h"

namespace netlib {

// 前置声明
class EventLoop;

class EventLoopThread : Noncopyable {
public:
  EventLoopThread();
  /// 让 loop 退出（Quit），并等待线程结束（join）
  ~EventLoopThread();

  /// 阻塞到新线程构造好 EventLoop，返回的指针在 EventLoopThread 析构前一直有效
  /// 只能调用一次
  EventLoop* StartLoop();

private:
  /// 线程的工作函数
  void ThreadFunc();

private:
  EventLoop* loop_;               // 指向新线程栈上的 EventLoop，由 mutex_ 保护，Loop 返回后置空
  std::thread thread_;            // 运行 EventLoop 的线程
  std::mutex mutex_;              // 保护 loop_
  std::condition_variable cond_;  // 新线程设置好 loop_ 后通知，StartLoop 在上面等待
};

}  // namespace netlib
