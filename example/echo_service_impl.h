// 轻舟 RPC 示例：Echo 服务的业务实现
// example/echo_service_impl.h
//
// 这个类的唯一职责是"证明框架能把一次 RPC 从字节流转成虚函数调用，再转回字节流"。
// 业务逻辑刻意保持在 10 行以内——多一行都会让读者怀疑"是不是框架要求这么多"。
//
// 为什么 served_by 填监听端口而不是 pid：
//   演示要在同一台机器上起两个实例（16161 / 16162），客户端需要**从响应里**分辨
//   "这次是谁服务的"。pid 也能分辨，但运维和读者都得再去查一遍进程号；
//   端口是客户端本来就握在手里的信息，直接对上，人一眼就能看懂。
//   （E-02 原文写的是 pid；改成端口是为了满足"两个实例各被调用多少次"那条验收。）

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "common/status.h"
#include "echo.qz.h"

namespace demo {

// 继承生成的服务端基类；只实现纯虚方法，不碰网络、不碰序列化。
class EchoServiceImpl final : public EchoServiceBase {
 public:
  // 参数：listening_port - 已知的监听端口（配置里 port != 0 时用）；
  //       传 0 表示"由系统分配"，此时必须在 Server::Start() 之后调
  //       set_listening_port(server.port())，否则 served_by 会报 0。
  explicit EchoServiceImpl(std::uint16_t listening_port = 0);
  ~EchoServiceImpl() override;

  // Start() 之后补上真实端口。served_by 只在处理请求时读，所以这里晚一点设置是安全的。
  void set_listening_port(std::uint16_t port) noexcept {
    port_.store(port, std::memory_order_relaxed);
  }
  std::uint16_t listening_port() const noexcept {
    return port_.load(std::memory_order_relaxed);
  }

  EchoServiceImpl(const EchoServiceImpl&) = delete;
  EchoServiceImpl& operator=(const EchoServiceImpl&) = delete;

  // 把 message 原样回显，并把 listening_port 写进 served_by。
  qzrpc::Status Echo(const EchoRequest& request, EchoResponse* response) override;

  // 只记一条日志（演示"非幂等方法"的默认行为：客户端不会自动重试）。
  qzrpc::Status Notify(const EchoRequest& request, EchoResponse* response) override;

  // 本实例被调用次数（server 退出时打印，便于核对负载均衡是否真的在分发）。
  std::uint64_t echo_total() const noexcept { return echo_total_.load(std::memory_order_relaxed); }
  std::uint64_t notify_total() const noexcept {
    return notify_total_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<std::uint16_t> port_{0};
  std::atomic<std::uint64_t> echo_total_{0};
  std::atomic<std::uint64_t> notify_total_{0};
};

}  // namespace demo
