// 轻舟 RPC (Qingzhou RPC)
// src/server/dispatcher.h
//
// 职责：method_id → 处理函数的**执行期路由表**。
//
// 与 ServiceRegistry 的分工（刻意拆开，避免一个类干两件事）：
//   * Dispatcher     —— 热路径用。只有 uint32 → handler 的哈希表，无锁读；
//   * ServiceRegistry—— 元信息目录。存全名、请求/响应类型名，用于启动期冲突检测、
//                        日志、metrics label、/debug 页面。冷路径用。
//
// 为什么 handler 是"无类型"的 std::function：
//   框架不应该认识任何具体的 IDL 生成类。生成的 Dispatcher 在注册时把
//   `Status EchoImpl::Handle(const void*, size_t, std::string*)` 包进 lambda，
//   内部完成反序列化 → 调业务虚函数 → 序列化。类型安全由 codegen 保证，
//   框架层只处理字节。这也是"换序列化/换 IDL 只需要换 codegen"的关键。
//
// 线程安全：Register 与 Find 可跨线程并发（读写锁）；Find 在热路径上只取共享锁。
//           注册完成后应视为只读，此时 Find 的开销与普通 unordered_map 相同。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/status.h"

namespace qzrpc {

namespace protocol {
class Metadata;
}  // namespace protocol

namespace rpc {

// 一次服务端调用的上下文。生命周期：仅在一次 Dispatch 调用期间有效，
// 处理器**不得**把指针存到调用之外。
struct ServerContext {
  // 请求帧序号，响应必须原样回填。
  std::uint32_t seq = 0;
  // 方法 id。
  std::uint32_t method_id = 0;
  // 方法全名（"demo.EchoService.Echo"），由 ServiceRegistry 提供，可能为 nullptr。
  const char* method_full_name = nullptr;
  // 对端地址（"ip:port"）。
  std::string peer_addr;
  // 帧 metadata（trace_id / timeout / deadline / idempotent 等）。非空。
  const protocol::Metadata* metadata = nullptr;
  // 服务端收到请求的单调时钟毫秒。
  std::uint64_t start_ms = 0;

  // 是否已超过调用方声明的 deadline。
  // 参数：now_ms - 当前单调时钟毫秒。
  bool deadline_exceeded(std::uint64_t now_ms) const noexcept;
};

// 未类型化的方法处理器。
// 参数：payload - 请求消息体原始字节（可能为 nullptr 且 len 为 0）；
//       payload_len - 字节数；
//       out_payload - 输出响应消息体（处理器负责 append/覆盖）；
//       ctx - 调用上下文（非空）。
// 返回：Status；非 ok 时框架回带该状态码的错误响应，并忽略 out_payload。
using MethodHandler = std::function<Status(const char* payload, std::size_t payload_len,
                                           std::string* out_payload, ServerContext* ctx)>;

// 方法路由表。
class Dispatcher {
 public:
  Dispatcher();
  ~Dispatcher();

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  // 注册一个方法。
  // 参数：method_id - 由 MethodIdOfName 计算（codegen 生成的常量）；
  //       full_name - "package.Service.Method"，用于日志与冲突报告；
  //       handler - 非空处理器。
  // 返回：true 注册成功；
  //       false 表示 method_id 已被注册，且 full_name 与之不同（**哈希冲突**）——
  //             这是必须人工介入的严重问题，调用方应把 err 打出来并终止启动。
  // 线程安全：是。
  bool Register(std::uint32_t method_id, std::string full_name, MethodHandler handler,
                std::string* err = nullptr);

  // 查找处理器。
  // 返回：找到时返回指针（生命周期同 Dispatcher，注册完成后不再变化）；
  //       未找到返回 nullptr 且 *full_name 保持不变。
  // 线程安全：是（共享锁）。
  const MethodHandler* Find(std::uint32_t method_id, const char** full_name = nullptr) const;

  // 未命中时的兜底处理器（可选）。用于回自定义错误或做网关转发。
  void SetFallback(MethodHandler handler);
  const MethodHandler* fallback() const;

  // 已注册方法数。
  std::size_t size() const;

  // 全部 method_id（升序），用于日志与自检。
  std::vector<std::uint32_t> method_ids() const;

  // 清空（仅测试用）。
  void Clear();

 private:
  struct Entry {
    std::string full_name;
    MethodHandler handler;
  };

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::uint32_t, Entry> table_;
  MethodHandler fallback_;
};

}  // namespace rpc
}  // namespace qzrpc
