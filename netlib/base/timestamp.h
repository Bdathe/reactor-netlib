/**
 * @file timestamp.h
 * @brief 时间点，精确到微秒
 *
 * 保存距 1970-01-01 00:00:00 UTC 的微秒数，用于日志时间和消息的接收时间。
 * 基于系统时钟，会随校时跳变；计算时间间隔（如定时器）要用 steady_clock。
 * 只有一个 int64_t，按值传递。
 */

#pragma once

#include <cstdint>
#include <string>

namespace netlib {

class Timestamp {
public:
  static constexpr int kMicrosecondsPerSecond = 1000 * 1000;

  Timestamp() : microseconds_since_epoch_(0) {}

  explicit Timestamp(int64_t microseconds_since_epoch)
      : microseconds_since_epoch_(microseconds_since_epoch) {}

  int64_t microseconds_since_epoch() const { return microseconds_since_epoch_; }

  /// 当前时间。基于系统时钟，可能因校时而回跳，不要用来计算时间间隔
  static Timestamp Now();

  /// 格式化为本地时间的年月日时分秒
  std::string ToFormattedString() const;

private:
  int64_t microseconds_since_epoch_;
};

}  // namespace netlib
