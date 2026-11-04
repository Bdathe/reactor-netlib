/**
 * @file callbacks.h
 * @brief TcpConnection & TcpServer 相关别名声明
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "netlib/base/timestamp.h"

namespace netlib {

// 前置声明
class TcpConnection;
class Buffer;

// 连接对象指针
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
// 连接上线、下线时通知用户
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
// 收到数据时通知用户，参数是输入 Buffer 和接收时间
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)>;
// 输出 Buffer 写空的时候通知用户
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
// 输出 Buffer 超过阈值的时候通知用户，参数是当前长度
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, std::size_t)>;
// 库内部使用：通知 TcpServer 把连接从连接表中删除
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

}  // namespace netlib
