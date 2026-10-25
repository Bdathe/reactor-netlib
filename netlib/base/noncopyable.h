/**
 * @file noncopyable.h
 * @brief 禁止拷贝的基类
 *
 * 持有资源（fd 等）的类继承它，避免拷贝后两个对象重复释放同一份资源。
 * 用法：class Socket : Noncopyable { ... };
 */

#pragma once

namespace netlib {

class Noncopyable {
public:
  Noncopyable(const Noncopyable&) = delete;
  Noncopyable& operator=(const Noncopyable&) = delete;

protected:
  Noncopyable() = default;
  ~Noncopyable() = default;
};

}  // namespace netlib
