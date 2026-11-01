#include "netlib/net/buffer.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>

#include "netlib/base/noncopyable.h"

namespace netlib {

namespace {

// 测试用的管道：Write() 写入的数据可以从 read_fd() 读出；两端都是非阻塞的
class Pipe : Noncopyable {
public:
  Pipe() { EXPECT_EQ(::pipe2(fds_, O_NONBLOCK | O_CLOEXEC), 0); }
  ~Pipe() {
    CloseWriteEnd();
    ::close(fds_[0]);
  }

  int read_fd() const { return fds_[0]; }

  void Write(std::string_view data) {
    ASSERT_EQ(::write(fds_[1], data.data(), data.size()), static_cast<ssize_t>(data.size()));
  }

  // 关闭写端后，读端读完剩余数据会读到 EOF（返回 0）
  void CloseWriteEnd() {
    if (fds_[1] != -1) {
      ::close(fds_[1]);
      fds_[1] = -1;
    }
  }

private:
  int fds_[2] = {-1, -1};
};

}  // namespace

TEST(BufferTest, InitialState) {
  Buffer buf;
  EXPECT_EQ(buf.ReadableBytes(), 0);
  EXPECT_EQ(buf.WritableBytes(), Buffer::kInitialSize);
  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend);
}

TEST(BufferTest, AppendIncreasesReadable) {
  Buffer buf;
  std::string s = "hello";
  std::size_t len = s.size();

  buf.Append(s);
  EXPECT_EQ(buf.ReadableBytes(), len);
  EXPECT_EQ(buf.WritableBytes(), Buffer::kInitialSize - len);
  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend);
  EXPECT_EQ(std::string(buf.Peek(), buf.ReadableBytes()), s);
}

TEST(BufferTest, AppendMultipleTimesConcatenates) {
  Buffer buf;
  std::string s1 = "hello";
  std::string s2 = " world";
  std::size_t len1 = s1.size();
  std::size_t len2 = s2.size();

  buf.Append(s1);
  buf.Append(s2);

  EXPECT_EQ(buf.ReadableBytes(), len1 + len2);
  EXPECT_EQ(buf.WritableBytes(), Buffer::kInitialSize - len1 - len2);
  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend);

  std::string ret = buf.RetrieveAllAsString();
  EXPECT_EQ(ret, s1 + s2);
}

TEST(BufferTest, RetrievePartial) {
  Buffer buf;
  std::string s1 = "hello ";
  std::string s2 = "world";
  std::size_t len1 = s1.size();
  std::size_t len2 = s2.size();

  buf.Append(s1 + s2);

  buf.Retrieve(len1);

  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend + len1);
  EXPECT_EQ(buf.ReadableBytes(), len2);
  std::string ret = buf.RetrieveAllAsString();
  EXPECT_EQ(ret, s2);
}

TEST(BufferTest, RetrieveAllResetsIndices) {
  Buffer buf;
  std::string s1 = "hello";
  std::string s2 = " world";

  buf.Append(s1);
  buf.Append(s2);

  buf.Retrieve(buf.ReadableBytes());

  EXPECT_EQ(buf.ReadableBytes(), 0);
  EXPECT_EQ(buf.WritableBytes(), Buffer::kInitialSize);
  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend);
}

TEST(BufferTest, RetrieveAsString) {
  Buffer buf;
  std::string s1 = "hello";
  std::string s2 = " world";

  buf.Append(s1);
  buf.Append(s2);

  std::string ret1 = buf.RetrieveAsString(s1.size());
  std::string ret2 = buf.RetrieveAsString(s2.size());

  EXPECT_EQ(ret1, s1);
  EXPECT_EQ(ret2, s2);
  EXPECT_EQ(buf.ReadableBytes(), 0);
  EXPECT_EQ(buf.WritableBytes(), Buffer::kInitialSize);
  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend);
}

TEST(BufferTest, MakeSpaceByMovingData) {
  Buffer buf(16);

  std::size_t before = buf.PrependableBytes() + buf.ReadableBytes() + buf.WritableBytes();

  // 12 bytes
  buf.Append("abcdefghijkl");
  buf.Retrieve(8);
  // 10 bytes
  buf.Append("abcdefghij");

  std::size_t after = buf.PrependableBytes() + buf.ReadableBytes() + buf.WritableBytes();
  EXPECT_EQ(before, after);
  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend);

  std::string ret = buf.RetrieveAllAsString();
  EXPECT_EQ(ret, "ijklabcdefghij");
}

TEST(BufferTest, MakeSpaceByGrowing) {
  Buffer buf(16);
  // 12 bytes
  std::string s1 = "abcdefghijkl";
  // 88 bytes
  std::string s2 =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{};:,.<>?/";

  std::size_t before = buf.PrependableBytes() + buf.ReadableBytes() + buf.WritableBytes();

  buf.Append(s1);
  buf.Append(s2);

  std::size_t after = buf.PrependableBytes() + buf.ReadableBytes() + buf.WritableBytes();
  EXPECT_LT(before, after);

  EXPECT_EQ(buf.ReadableBytes(), s1.size() + s2.size());

  std::string ret = buf.RetrieveAllAsString();
  EXPECT_EQ(ret, s1 + s2);
}

TEST(BufferDeathTest, RetrieveBeyondReadableAborts) {
  Buffer buf;
  buf.Append("hello");

  EXPECT_DEBUG_DEATH({ buf.Retrieve(6); }, "");
}

TEST(BufferTest, PrependBeforeReadable) {
  Buffer buf;
  buf.Append("world");
  buf.Prepend("hello ", 6);

  EXPECT_EQ(std::string(buf.Peek(), buf.ReadableBytes()), "hello world");
  EXPECT_EQ(buf.PrependableBytes(), Buffer::kCheapPrepend - 6);
  EXPECT_EQ(buf.ReadableBytes(), 11);
}

TEST(BufferTest, PrependFillsReservedArea) {
  Buffer buf;

  std::string s(Buffer::kCheapPrepend, 'x');
  buf.Prepend(s.data(), Buffer::kCheapPrepend);

  EXPECT_EQ(std::string(buf.Peek(), buf.ReadableBytes()), s);
  EXPECT_EQ(buf.PrependableBytes(), 0);
  EXPECT_EQ(buf.ReadableBytes(), Buffer::kCheapPrepend);
}

TEST(BufferDeathTest, PrependBeyondPrependableAborts) {
  Buffer buf;

  std::string s(Buffer::kCheapPrepend + 1, 'x');

#ifdef NDEBUG
  GTEST_SKIP() << "assert is disabled in release";
#endif

  EXPECT_DEATH({ buf.Prepend(s.data(), Buffer::kCheapPrepend + 1); }, "");
}

TEST(BufferTest, ReadFdFitsInWritable) {
  Buffer buf;
  Pipe pipe;

  int saved_errno = 0;
  pipe.Write("hello");
  ssize_t n = buf.ReadFd(pipe.read_fd(), &saved_errno);

  EXPECT_EQ(n, 5);
  EXPECT_EQ(saved_errno, 0);
  EXPECT_EQ(std::string(buf.Peek(), buf.ReadableBytes()), "hello");
}

TEST(BufferTest, ReadFdOverflowsIntoExtraBuffer) {
  Buffer buf(16);
  Pipe pipe;

  std::string s;
  for (int i = 0; i < 100; ++i) {
    char c = 'a' + i % 26;
    s.push_back(c);
  }

  int saved_errno = 0;
  pipe.Write(s);
  ssize_t n = buf.ReadFd(pipe.read_fd(), &saved_errno);

  EXPECT_EQ(n, 100);
  EXPECT_EQ(saved_errno, 0);
  EXPECT_EQ(std::string(buf.Peek(), buf.ReadableBytes()), s);
}

TEST(BufferTest, ReadFdReturnsZeroOnEof) {
  Buffer buf;
  Pipe pipe;

  int saved_errno = 0;
  pipe.CloseWriteEnd();
  ssize_t n = buf.ReadFd(pipe.read_fd(), &saved_errno);

  EXPECT_EQ(n, 0);
  EXPECT_EQ(buf.ReadableBytes(), 0);
}

TEST(BufferTest, ReadFdReturnsErrorWhenNoData) {
  Buffer buf;
  Pipe pipe;

  int saved_errno = 0;
  ssize_t n = buf.ReadFd(pipe.read_fd(), &saved_errno);

  EXPECT_EQ(n, -1);
  EXPECT_EQ(saved_errno, EAGAIN);
  EXPECT_EQ(buf.ReadableBytes(), 0);
}

}  // namespace netlib
