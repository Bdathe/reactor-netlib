/**
 * @file inet_address.h
 * @brief IPv4 地址和端口，封装 sockaddr_in
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <optional>
#include <string>

namespace netlib {

class InetAddress {
public:
  explicit InetAddress(uint16_t port) : addr_(sockaddr_in{}) {
    addr_.sin_family = AF_INET;  // IPv4
    addr_.sin_port = htons(port);
    addr_.sin_addr.s_addr = htonl(INADDR_ANY);
  }

  explicit InetAddress(const sockaddr_in& addr) : addr_(addr) {}

  /// 将 IP 和端口格式化为 IP:port（如 127.0.0.1:9877）
  std::string ToIpPort() const;

  /// 解析点分十进制的 IPv4 地址（如 "127.0.0.1"），非法时返回 std::nullopt。不解析域名
  [[nodiscard]] static std::optional<InetAddress> FromIpPort(const std::string& ip, uint16_t port);

  const sockaddr* GetSockAddr() const { return reinterpret_cast<const sockaddr*>(&addr_); }

private:
  sockaddr_in addr_;
};

}  // namespace netlib
