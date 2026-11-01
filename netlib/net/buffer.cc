/**
 * @file buffer.cc
 * @brief Buffer 的实现
 */

#include "netlib/net/buffer.h"

#include <sys/types.h>
#include <sys/uio.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>

namespace netlib {

void Buffer::MakeSpace(std::size_t len) {
  // 先查看预留区域 + 可写区域是否有足够的空间
  // 空间够，则把可读区域移入，扩大可写区域
  if (PrependableBytes() + WritableBytes() >= len + kCheapPrepend) {
    std::size_t readable_bytes = ReadableBytes();

    std::copy(Peek(), Peek() + readable_bytes, Begin() + kCheapPrepend);
    writer_index_ = kCheapPrepend + readable_bytes;
    reader_index_ = kCheapPrepend;
    return;
  }

  // 空间不够，直接扩容
  buffer_.resize(buffer_.size() + len);
}

ssize_t Buffer::ReadFd(int fd, int* saved_errno) {
  char extrabuf[65536];

  iovec iovs[2];
  iovs[0].iov_base = BeginWrite();
  iovs[0].iov_len = WritableBytes();
  iovs[1].iov_base = extrabuf;
  iovs[1].iov_len = sizeof(extrabuf);

  int iovcnt = WritableBytes() < sizeof(extrabuf) ? 2 : 1;
  ssize_t n = ::readv(fd, iovs, iovcnt);

  if (n == -1) {
    *saved_errno = errno;
    return -1;
  }

  if (n > static_cast<ssize_t>(WritableBytes())) {
    std::size_t writable_bytes = WritableBytes();
    HasWritten(writable_bytes);
    std::size_t extra_len = n - writable_bytes;
    Append(extrabuf, extra_len);
  } else {
    HasWritten(n);
  }

  return n;
}

}  // namespace netlib
