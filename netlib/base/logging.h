/**
 * @file logging.h
 * @brief 日志前端：Logger 类和 LOG_* 宏
 *
 * 一个 Logger 临时对象就是一行日志：构造时写入时间和级别，析构时补上文件、行号，
 * 再一次 fwrite 写到 stderr。M4 改为写入 AsyncLogging 后端。
 * 用法：LOG_INFO << "fd = " << fd;
 */

#pragma once

#include <sstream>

#include "netlib/base/noncopyable.h"

namespace netlib {

enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError, kFatal };

class Logger : Noncopyable {
public:
  Logger(const char* file, int line, LogLevel level, bool with_errno = false);
  ~Logger();

  std::ostream& stream() { return stream_; }

private:
  std::ostringstream stream_;
  const char* file_;
  int line_;
  LogLevel level_;
  int saved_errno_;
};

/// 最低输出级别：低于它的日志不输出，默认 kInfo。LOG_FATAL、LOG_SYSFATAL 不受它影响。
/// 读写没有加锁，只在启动时、创建线程之前设置
LogLevel MinLogLevel();
void SetMinLogLevel(LogLevel level);

}  // namespace netlib

// 用 if-else 而不是单独的 if：避免用户在 if/else 中使用时，外面的 else 和宏里的 if 配对
#define LOG_TRACE                                         \
  if (netlib::MinLogLevel() > netlib::LogLevel::kTrace) { \
  } else                                                  \
    netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kTrace).stream()

#define LOG_DEBUG                                         \
  if (netlib::MinLogLevel() > netlib::LogLevel::kDebug) { \
  } else                                                  \
    netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kDebug).stream()

#define LOG_INFO                                         \
  if (netlib::MinLogLevel() > netlib::LogLevel::kInfo) { \
  } else                                                 \
    netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kInfo).stream()

#define LOG_WARN                                         \
  if (netlib::MinLogLevel() > netlib::LogLevel::kWarn) { \
  } else                                                 \
    netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kWarn).stream()

#define LOG_ERROR                                         \
  if (netlib::MinLogLevel() > netlib::LogLevel::kError) { \
  } else                                                  \
    netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kError).stream()

#define LOG_SYSERR                                        \
  if (netlib::MinLogLevel() > netlib::LogLevel::kError) { \
  } else                                                  \
    netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kError, true).stream()

#define LOG_FATAL netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kFatal).stream()

#define LOG_SYSFATAL netlib::Logger(__FILE__, __LINE__, netlib::LogLevel::kFatal, true).stream()
