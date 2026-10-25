/**
 * @file timestamp.cc
 * @brief Timestamp 的实现
 */

#include "netlib/base/timestamp.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace netlib {

Timestamp Timestamp::Now() {
  auto tp = std::chrono::system_clock::now();  // 当前时间点
  int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                   tp.time_since_epoch())  // 距离 1970 年的时长
                   .count();               // 转为整数

  return Timestamp(us);
}

std::string Timestamp::ToFormattedString() const {
  time_t seconds = microseconds_since_epoch_ / kMicrosecondsPerSecond;
  int micros = static_cast<int>(microseconds_since_epoch_ % kMicrosecondsPerSecond);

  // 秒数转为本地时间的年月日时分秒
  tm tm_time;
  // tm_time.tm_year：年份 - 1900
  // tm_time.tm_mon：月份，0～11
  // tm_time.tm_mday：日
  // tm_time.tm_hour / tm_min / tm_sec
  localtime_r(&seconds, &tm_time);

  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d", tm_time.tm_year + 1900,
           tm_time.tm_mon + 1, tm_time.tm_mday, tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
           micros);
  return buf;
}

}  // namespace netlib
