/**
 * @file buffer.h
 * @brief 应用层读写缓冲区：连续内存 + 读写下标
 *
 * 内存布局：
 * +---------------+------------------+------------------+
 * |  prependable  |     readable     |     writable     |
 * +---------------+------------------+------------------+
 * 0         reader_index_      writer_index_     buffer_.size()
 */

#pragma once

#include <sys/types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace netlib {

class Buffer {
public:
  static constexpr std::size_t kCheapPrepend = 8;    // 预留区的大小
  static constexpr std::size_t kInitialSize = 1024;  // 初始可写区的大小

  explicit Buffer(std::size_t initial_size = kInitialSize)
      : buffer_(kCheapPrepend + initial_size),
        reader_index_(kCheapPrepend),
        writer_index_(kCheapPrepend) {}

  std::size_t ReadableBytes() const { return writer_index_ - reader_index_; }

  std::size_t WritableBytes() const { return buffer_.size() - writer_index_; }

  std::size_t PrependableBytes() const { return reader_index_; }

  /// 可读区的起始地址，不取走数据
  const char* Peek() const { return Begin() + reader_index_; }

  /// 可写区的起始地址，写完后要调用 HasWritten
  char* BeginWrite() { return Begin() + writer_index_; }
  const char* BeginWrite() const { return Begin() + writer_index_; }

  /// 取走前 len 个字节，如果全部取完，等价于 RetrieveAll()
  void Retrieve(std::size_t len) {
    assert(len <= ReadableBytes());

    if (ReadableBytes() == len) {
      RetrieveAll();
      return;
    }

    reader_index_ += len;
  }

  /// 取走全部数据，读写下标都回到 kCheapPrepend
  void RetrieveAll() {
    reader_index_ = kCheapPrepend;
    writer_index_ = kCheapPrepend;
  }

  /// 取走前 len 个字节，以 string 返回
  std::string RetrieveAsString(std::size_t len) {
    assert(len <= ReadableBytes());

    std::string ret(Peek(), len);

    Retrieve(len);

    return ret;
  }

  std::string RetrieveAllAsString() { return RetrieveAsString(ReadableBytes()); }

  /// data 不能指向本 Buffer 的内部
  /// 内部扩容可能会导致原来的指针失效
  void Append(const char* data, std::size_t len) {
    EnsureWritableBytes(len);
    std::copy(data, data + len, BeginWrite());
    HasWritten(len);
  }

  void Append(std::string_view data) { Append(data.data(), data.size()); }

  /// 保证可写区至少 len 字节，不够时挪动数据或扩容
  void EnsureWritableBytes(std::size_t len) {
    if (len > WritableBytes()) {
      MakeSpace(len);
    }
  }

  /// 直接往 BeginWrite() 写入 len 字节后，移动写下标
  void HasWritten(std::size_t len) {
    assert(len <= WritableBytes());
    writer_index_ += len;
  }

  /// 在可读区前面插入 len 字节（如长度头），len 不能超过 PrependableBytes()
  void Prepend(const void* data, std::size_t len) {
    assert(len <= PrependableBytes());

    reader_index_ -= len;
    const char* src = static_cast<const char*>(data);
    std::copy(src, src + len, Begin() + reader_index_);
  }

  /// 用 readv 从 fd 读一次：先填可写区，放不下的进栈上 64KB 缓冲区，再 Append 进来。
  /// 返回读到的字节数，0 表示对端关闭；出错返回 -1，errno 写入 *saved_errno
  ssize_t ReadFd(int fd, int* saved_errno);

private:
  char* Begin() { return buffer_.data(); }
  const char* Begin() const { return buffer_.data(); }

  /// 至少腾出 len 字节的可写空间
  /// 预留区 + 可写区够用时，把可读数据挪到前面；否则扩容
  void MakeSpace(std::size_t len);

private:
  std::vector<char> buffer_;  // 底层存储
  std::size_t reader_index_;  // 可读区起点
  std::size_t writer_index_;  // 可写区起点
};

}  // namespace netlib
