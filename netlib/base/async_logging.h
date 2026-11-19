/**
 * @file async_logging.h
 * @brief 双缓冲异步日志后端：前端写内存缓冲，后台线程定时或写满时批量刷盘
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "netlib/base/noncopyable.h"

namespace netlib {

class AsyncLogging : Noncopyable {
public:
  // 4 MB
  static constexpr std::size_t kDefaultBufferSize = 4 * 1000 * 1000;

  using Duration = std::chrono::milliseconds;

  /// 指定日志文件路径、定时刷盘间隔与单块缓冲区大小（此时尚未启动后台线程）
  explicit AsyncLogging(std::string file_path, Duration flush_interval = Duration(3000),
                        std::size_t buffer_size = kDefaultBufferSize);
  /// 若后台线程仍在运行，自动调用 Stop() 刷完剩余日志后再析构
  ~AsyncLogging();

  /// 启动后台日志刷盘线程（只调用一次）
  void Start();
  /// 通知后台线程停止、刷完所有残留缓冲区并 join
  void Stop();

  /// 前端写入入口：追加到 current_buffer_，写满时切到 next_buffer_ 并唤醒后端
  /// 任意线程都可以调用
  void Append(const char* logline, std::size_t len);

private:
  /// 后台日志线程主循环：wait_for 等待唤醒或超时，交换缓冲区后写文件
  void ThreadFunc();

private:
  class Buffer : Noncopyable {
  public:
    explicit Buffer(std::size_t capacity)
        : data_(std::make_unique<char[]>(capacity)), capacity_(capacity), size_(0) {}

    /// 返回剩余可用字节数
    std::size_t Avail() const { return capacity_ - size_; }

    /// 往 Buffer 中追加数据
    void Append(const char* buf, std::size_t len) {
      if (len > 0 && Avail() >= len) {
        std::memcpy(data_.get() + size_, buf, len);
        size_ += len;
      }
    }

    /// 返回缓冲区首地址
    const char* data() const { return data_.get(); }

    /// 返回缓冲区当前大小
    std::size_t size() const { return size_; }

    /// 重置缓冲区
    void Reset() { size_ = 0; }

  private:
    std::unique_ptr<char[]> data_;
    const std::size_t capacity_;
    std::size_t size_;
  };

  using BufferPtr = std::unique_ptr<Buffer>;
  using BufferVector = std::vector<BufferPtr>;

  const std::string file_path_;    // 输出日志文件路径
  const Duration flush_interval_;  // 定时刷盘间隔（如 3000ms）
  const std::size_t buffer_size_;  // 单块缓冲区容量（默认 4MB）
  std::atomic<bool> running_;      // 后台线程运行标志
  std::thread thread_;             // 后台日志刷盘线程
  std::mutex mutex_;               // 保护 current_buffer_、next_buffer_、buffers_
  std::condition_variable cond_;   // 前端写满或 Stop 时通知后台线程
  BufferPtr current_buffer_;       // 当前正在追加的前端缓冲区
  BufferPtr next_buffer_;          // 预先备好的空闲备用缓冲区（避免锁内分配内存）
  BufferVector buffers_;           // 已写满、等待后台线程 swap 取走的缓冲区列表
};

}  // namespace netlib
