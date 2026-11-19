/**
 * @file async_logging.cc
 * @brief AsyncLogging 的实现
 */

#include "netlib/base/async_logging.h"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace netlib {

AsyncLogging::AsyncLogging(std::string file_path, Duration flush_interval, std::size_t buffer_size)
    : file_path_(std::move(file_path)),
      flush_interval_(flush_interval),
      buffer_size_(buffer_size),
      running_(false),
      current_buffer_(std::make_unique<AsyncLogging::Buffer>(buffer_size)),
      next_buffer_(std::make_unique<AsyncLogging::Buffer>(buffer_size)) {
  // 预留指针槽位
  buffers_.reserve(16);
}

AsyncLogging::~AsyncLogging() {
  if (running_) {
    Stop();
  }
}

void AsyncLogging::Start() {
  // 防止重复启动
  if (running_.exchange(true)) {
    return;
  }

  thread_ = std::thread([this] { ThreadFunc(); });
}

void AsyncLogging::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }

    running_ = false;
  }

  // 唤醒线程把剩余内容写入磁盘
  cond_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void AsyncLogging::Append(const char* logline, std::size_t len) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_buffer_->Avail() >= len) {
      current_buffer_->Append(logline, len);
    } else {
      buffers_.push_back(std::move(current_buffer_));
      if (next_buffer_) {
        current_buffer_ = std::move(next_buffer_);
      } else {
        current_buffer_ = std::make_unique<Buffer>(buffer_size_);
      }

      current_buffer_->Append(logline, len);
      cond_.notify_one();
    }
  }
}

void AsyncLogging::ThreadFunc() {
  // a 是追加
  // e 是 O_CLOEXEC
  std::FILE* fp = std::fopen(file_path_.c_str(), "ae");
  if (!fp) {
    int saved_errno = errno;
    char err_buf[128];
    const char* msg = ::strerror_r(saved_errno, err_buf, sizeof(err_buf));
    std::fprintf(stderr, "fopen failed, path=%s: %s (errno=%d)\n", file_path_.c_str(), msg,
                 saved_errno);
    std::abort();
  }

  // 创建两块后端私有的干净缓冲区
  BufferPtr new_buffer1 = std::make_unique<Buffer>(buffer_size_);
  BufferPtr new_buffer2 = std::make_unique<Buffer>(buffer_size_);
  BufferVector buffers_to_write{};

  while (running_) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // 不会无限制睡眠，超时会被唤醒
      // 谓词为假时睡眠，即
      // buffers 为空而且还在运行，则睡眠
      cond_.wait_for(lock, flush_interval_, [this] { return !buffers_.empty() || !running_; });

      // 1. 当前没写完的 current_buffer_ 收入 buffers_
      buffers_.push_back(std::move(current_buffer_));
      // 2. 把空桶 1 换给前端当新的 current_buffer_
      current_buffer_ = std::move(new_buffer1);
      // 3. 把整串待写列表换到局部 buffers_to_write
      buffers_.swap(buffers_to_write);
      // 4. 如果前端的备用缓冲区被用了，把空桶 2 补给它
      if (!next_buffer_) {
        next_buffer_ = std::move(new_buffer2);
      }
    }

    // 超过 25 块丢弃，即 25 * 4MB = 100 MB
    if (buffers_to_write.size() > 25) {
      char drop_msg[128];
      std::snprintf(drop_msg, sizeof(drop_msg), "AsyncLogging dropped %zu log buffers\n",
                    buffers_to_write.size() - 2);
      std::fputs(drop_msg, stderr);
      std::fputs(drop_msg, fp);
      // 只留前两块
      buffers_to_write.erase(buffers_to_write.begin() + 2, buffers_to_write.end());
    }

    // 遍历写文件
    for (const BufferPtr& buf : buffers_to_write) {
      if (buf->size() > 0) {
        std::fwrite(buf->data(), 1, buf->size(), fp);
      }
    }

    // 回收两块桶
    if (!new_buffer1) {
      new_buffer1 = std::move(buffers_to_write.back());
      buffers_to_write.pop_back();
      new_buffer1->Reset();  // 清零 size
    }

    if (!new_buffer2) {
      new_buffer2 = std::move(buffers_to_write.back());
      buffers_to_write.pop_back();
      new_buffer2->Reset();  // 清零 size
    }

    buffers_to_write.clear();
    std::fflush(fp);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_buffer_ && current_buffer_->size() > 0) {
      buffers_.push_back(std::move(current_buffer_));
    }

    buffers_.swap(buffers_to_write);
  }

  // 遍历写文件
  for (const BufferPtr& buf : buffers_to_write) {
    if (buf->size() > 0) {
      std::fwrite(buf->data(), 1, buf->size(), fp);
    }
  }

  std::fflush(fp);
  std::fclose(fp);
}

}  // namespace netlib
