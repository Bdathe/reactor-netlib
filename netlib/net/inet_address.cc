/**
 * @file inet_address.cc
 * @brief InetAddress 的实现
 */

#include "netlib/net/inet_address.h"

#include "netlib/base/logging.h"

namespace netlib {

std::optional<InetAddress> InetAddress::FromIpPort(const std::string& ip, uint16_t port) {
  InetAddress address(port);
  int ret = inet_pton(AF_INET, ip.c_str(), &address.addr_.sin_addr);

  if (ret == 0) {
    return std::nullopt;
  } else if (ret == -1) {
    LOG_SYSFATAL << "inet_pton failed";
  }

  return address;
}

std::string InetAddress::ToIpPort() const {
  char ip_buf[INET_ADDRSTRLEN];  // IPv4 字符串的最大长度（含 '\0'），值是 16
  if (!inet_ntop(AF_INET, &addr_.sin_addr, ip_buf, sizeof(ip_buf))) {  // network to presentation
    LOG_SYSFATAL << "inet_ntop failed";
  }

  return std::string(ip_buf) + ":" + std::to_string(ntohs(addr_.sin_port));
}

}  // namespace netlib
