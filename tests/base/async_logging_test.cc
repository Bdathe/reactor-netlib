#include "netlib/base/async_logging.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "netlib/base/logging.h"

namespace netlib {

namespace {

using namespace std::chrono_literals;

class AsyncLoggingTest : public ::testing::Test {
protected:
  void SetUp() override {
    file_path_ = "/tmp/netlib_async_log_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(++case_id_) + ".log";
    ::unlink(file_path_.c_str());
  }

  void TearDown() override {
    Logger::SetLogOutput(nullptr);
    Logger::SetLogFlush(nullptr);
    ::unlink(file_path_.c_str());
  }

  std::string ReadLogFile() const {
    std::ifstream ifs(file_path_, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
  }

  static std::size_t CountLines(const std::string& content) {
    return static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n'));
  }

  std::string file_path_;
  inline static int case_id_ = 0;
};

TEST_F(AsyncLoggingTest, PeriodicFlushWritesPartialBuffer) {
  AsyncLogging async_log(file_path_, 30ms, 4096);
  async_log.Start();

  std::string msg = "hello periodic flush\n";
  async_log.Append(msg.data(), msg.size());

  // 轮询等待后台定时刷盘（最多等 1s）：普通构建约 30ms 即通过，TSan 变慢时也不会误报
  auto deadline = std::chrono::steady_clock::now() + 1s;
  while (ReadLogFile().empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }

  EXPECT_EQ(ReadLogFile(), msg);
}

TEST_F(AsyncLoggingTest, MultiThreadFullBuffersNoLoss) {
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kLinesPerThread = 250;
  const std::string line = "async multi-thread log\n";  // 23 Byte

  AsyncLogging async_log(file_path_, 3000ms, 4096);
  async_log.Start();

  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      for (std::size_t j = 0; j < kLinesPerThread; ++j) {
        async_log.Append(line.data(), line.size());
      }
    });
  }

  for (auto& t : threads) {
    if (t.joinable()) {
      t.join();
    }
  }

  async_log.Stop();

  std::string content = ReadLogFile();

  EXPECT_EQ(CountLines(content), kThreads * kLinesPerThread);
  EXPECT_EQ(content.size(), kThreads * kLinesPerThread * line.size());
}

TEST_F(AsyncLoggingTest, LoggerHookRedirectsToAsyncFile) {
  AsyncLogging async_log(file_path_, 30ms, 4096);
  async_log.Start();

  Logger::SetLogOutput([&](const char* data, std::size_t len) { async_log.Append(data, len); });

  LOG_INFO << "async hook test, request_id=42";
  LOG_WARN << "disk water mark high";

  Logger::SetLogOutput(nullptr);

  async_log.Stop();

  std::string content = ReadLogFile();
  EXPECT_EQ(CountLines(content), 2);
  EXPECT_NE(content.find("request_id=42"), std::string::npos);
  EXPECT_NE(content.find("disk water mark high"), std::string::npos);
}

}  // namespace

}  // namespace netlib
