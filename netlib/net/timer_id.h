/**
 * @file timer_id.h
 * @brief 用户的取消凭证
 */

#pragma once

#include <cstdint>

namespace netlib {

class TimerId {
public:
  TimerId() : sequence_(0) {}
  explicit TimerId(uint64_t sequence) : sequence_(sequence) {}

  uint64_t sequence() const { return sequence_; }

private:
  uint64_t sequence_;
};

}  // namespace netlib
