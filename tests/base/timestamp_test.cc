#include "netlib/base/timestamp.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>

namespace netlib {
namespace {

void UseShanghaiTimeZone() {
  setenv("TZ", "Asia/Shanghai", 1);
  tzset();
}

TEST(TimestampTest, DefaultIsZero) {
  Timestamp t;
  EXPECT_EQ(t.microseconds_since_epoch(), 0);
}

TEST(TimestampTest, ConstructFromMicroseconds) {
  Timestamp t(123456);
  EXPECT_EQ(t.microseconds_since_epoch(), 123456);
}

TEST(TimestampTest, FormatEpoch) {
  UseShanghaiTimeZone();
  Timestamp t;
  EXPECT_EQ(t.ToFormattedString(), "1970-01-01 08:00:00.000000");
}

TEST(TimestampTest, FormatNonJanuaryDate) {
  UseShanghaiTimeZone();
  Timestamp t(1792909800123456);
  EXPECT_EQ(t.ToFormattedString(), "2026-10-25 14:30:00.123456");
}

TEST(TimestampTest, FormatPadsMicroseconds) {
  UseShanghaiTimeZone();
  Timestamp t(5);
  EXPECT_EQ(t.ToFormattedString(), "1970-01-01 08:00:00.000005");
}

TEST(TimestampTest, FormatMaxMicroseconds) {
  UseShanghaiTimeZone();
  Timestamp t(999999);
  EXPECT_EQ(t.ToFormattedString(), "1970-01-01 08:00:00.999999");
}

TEST(TimestampTest, NowMatchesSystemTime) {
  int64_t t1_seconds =
      Timestamp::Now().microseconds_since_epoch() / Timestamp::kMicrosecondsPerSecond;
  int64_t t2_seconds = time(nullptr);
  int64_t diff = std::abs(t1_seconds - t2_seconds);

  EXPECT_LE(diff, 1);
}

}  // namespace

}  // namespace netlib
