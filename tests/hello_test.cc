// M0 冒烟测试：只用来验证 GoogleTest 接入和 Makefile 是否正常工作
#include <gtest/gtest.h>

#include <string>

TEST(HelloTest, Arithmetic) {
  EXPECT_EQ(1 + 1, 2);
}

TEST(HelloTest, String) {
  std::string s = "netlib";
  EXPECT_EQ(s.size(), 6u);
}
