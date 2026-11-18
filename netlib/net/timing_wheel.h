/**
 * @file timing_wheel.h
 * @brief 基于环形桶和 shared_ptr 引用计数的时间轮，O(1) 踢除空闲连接
          每个 IO 线程维护一个 TimingWheel 管理自己的连接
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_set>

#include "netlib/base/noncopyable.h"
#include "netlib/net/callbacks.h"
#include "netlib/net/timer_id.h"

namespace netlib {

// 前置声明
class EventLoop;

class TimingWheel : Noncopyable {
public:
  using Duration = std::chrono::steady_clock::duration;

  /// 创建含 num_buckets 个桶的时间轮，每隔 tick_interval 转动一格
  /// 空闲约 num_buckets * tick_interval 未收数据的连接会被自动踢断，只能在 loop 线程构造与析构
  TimingWheel(EventLoop* loop, std::size_t num_buckets,
              Duration tick_interval = std::chrono::seconds(1));
  ~TimingWheel();

  /// 连接建立时创建 Entry 放入队尾桶，并把 weak_ptr<Entry> 存入 conn->context_
  /// 只能在 loop 线程调用
  void OnConnection(const TcpConnectionPtr& conn);

  /// 收到信息时往队尾追加桶，延长连接生命周期
  /// 只能在 loop 线程调用
  void OnMessage(const TcpConnectionPtr& conn);

private:
  /// 每次定时触发转动一格：弹出最老的队头桶，并在队尾补充新空桶
  void OnTick();

private:
  struct Entry {
    std::weak_ptr<TcpConnection> weak_conn;

    explicit Entry(const TcpConnectionPtr& conn) : weak_conn(conn) {}
    ~Entry();
  };

  using EntryPtr = std::shared_ptr<Entry>;
  using WeakEntryPtr = std::weak_ptr<Entry>;
  using Bucket = std::unordered_set<EntryPtr>;

  EventLoop* loop_;             // 所属 loop
  std::deque<Bucket> buckets_;  // 环形桶队列，front 最老，back 当前
  TimerId timer_id_;            // 驱动每秒转动的周期定时器凭证
};

}  // namespace netlib
