#include "netlib/net/inet_address.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <optional>

namespace netlib {

namespace {

TEST(InetAddressTest, PortFormatsAsAnyAddress) {
  EXPECT_EQ(InetAddress(9877).ToIpPort(), "0.0.0.0:9877");
}

TEST(InetAddressTest, PortBoundaries) {
  EXPECT_EQ(InetAddress(0).ToIpPort(), "0.0.0.0:0");
  EXPECT_EQ(InetAddress(65535).ToIpPort(), "0.0.0.0:65535");
}

TEST(InetAddressTest, PortMatchesRawSockaddr) {
  sockaddr_in expected{};
  expected.sin_family = AF_INET;
  expected.sin_port = htons(258);
  expected.sin_addr.s_addr = htonl(INADDR_ANY);

  InetAddress addr(258);
  EXPECT_EQ(std::memcmp(addr.GetSockAddr(), &expected, sizeof(expected)), 0);
}

TEST(InetAddressTest, FromSockaddrFormats) {
  sockaddr_in raw{};
  raw.sin_family = AF_INET;
  raw.sin_port = htons(40000);
  raw.sin_addr.s_addr = htonl(0xC0A80114);  // 192.168.1.20

  InetAddress addr(raw);
  EXPECT_EQ(addr.ToIpPort(), "192.168.1.20:40000");
}

TEST(InetAddressTest, FromIpPortValid) {
  auto ret_opt = InetAddress::FromIpPort("127.0.0.1", 9877);
  ASSERT_TRUE(ret_opt.has_value());
  EXPECT_EQ(ret_opt->ToIpPort(), "127.0.0.1:9877");
}

TEST(InetAddressTest, FromIpPortMatchesRawSockaddr) {
  sockaddr_in expected{};
  expected.sin_family = AF_INET;
  expected.sin_port = htons(258);
  expected.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  auto ret_opt = InetAddress::FromIpPort("127.0.0.1", 258);
  ASSERT_TRUE(ret_opt.has_value());
  EXPECT_EQ(std::memcmp(ret_opt->GetSockAddr(), &expected, sizeof(expected)), 0);
}

TEST(InetAddressTest, FromIpPortBoundaries) {
  auto ret_opt_1 = InetAddress::FromIpPort("0.0.0.0", 9877);
  auto ret_opt_2 = InetAddress::FromIpPort("255.255.255.255", 9877);

  ASSERT_TRUE(ret_opt_1.has_value());
  ASSERT_TRUE(ret_opt_2.has_value());
  EXPECT_EQ(ret_opt_1->ToIpPort(), "0.0.0.0:9877");
  EXPECT_EQ(ret_opt_2->ToIpPort(), "255.255.255.255:9877");
}

TEST(InetAddressTest, FromIpPortRejectsInvalid) {
  auto ret_opt_1 = InetAddress::FromIpPort("127.0.0.256", 9877);
  auto ret_opt_2 = InetAddress::FromIpPort("", 9877);
  auto ret_opt_3 = InetAddress::FromIpPort("localhost", 9877);
  auto ret_opt_4 = InetAddress::FromIpPort("1.2.3", 9877);
  auto ret_opt_5 = InetAddress::FromIpPort("1.2.3.4.5", 9877);

  EXPECT_FALSE(ret_opt_1.has_value());
  EXPECT_FALSE(ret_opt_2.has_value());
  EXPECT_FALSE(ret_opt_3.has_value());
  EXPECT_FALSE(ret_opt_4.has_value());
  EXPECT_FALSE(ret_opt_5.has_value());
}

}  // namespace

}  // namespace netlib
