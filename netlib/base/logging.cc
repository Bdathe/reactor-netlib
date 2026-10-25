/**
 * @file logging.cc
 * @brief Logger 的实现
 */

#include "netlib/base/logging.h"

#include <string.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "netlib/base/timestamp.h"

namespace netlib {

namespace {
const char* kLevelNames[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};

LogLevel min_log_level = LogLevel::kInfo;
}  // namespace

Logger::Logger(const char* file, int line, LogLevel level, bool with_errno)
    : file_(file), line_(line), level_(level) {
  saved_errno_ = with_errno ? errno : 0;
  stream_ << Timestamp::Now().ToFormattedString() << " " << kLevelNames[static_cast<int>(level)]
          << " ";
}

Logger::~Logger() {
  if (saved_errno_ != 0) {
    char buf[128];
    const char* msg = strerror_r(saved_errno_, buf, sizeof(buf));
    stream_ << ": " << msg << " (errno=" << saved_errno_ << ")";
  }

  stream_ << " - " << file_ << ":" << line_ << "\n";

  std::string str = stream_.str();
  fwrite(str.data(), 1, str.size(), stderr);

  if (level_ == LogLevel::kFatal) {
    std::abort();
  }
}

LogLevel MinLogLevel() {
  return min_log_level;
}

void SetMinLogLevel(LogLevel level) {
  min_log_level = level;
}

}  // namespace netlib
