// 轻舟 RPC (Qingzhou RPC)
// src/server/service_registry.h
//
// 职责：进程级的**服务/方法元信息目录**。由 codegen 生成的 RegisterXxxService() 在
//       进程启动时写入，供三处消费：
//         1) 启动期自检 —— 重算 method_id 是否与 full_name 一致、是否发生哈希冲突；
//         2) 运行期诊断 —— ERROR 日志里把 4 字节 method_id 翻回可读全名；
//         3) metrics —— 每个方法一个 label，避免指标变成"一锅粥"。
//
// 它**不参与热路径路由**（那是 Dispatcher 的职责），所以内部用读写锁完全够用。
//
// 线程安全：全部方法线程安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qzrpc {
namespace rpc {

// 一个 RPC 方法的元信息。
struct MethodInfo {
  // method_id = MethodIdOfName(full_name)。
  std::uint32_t method_id = 0;
  // "package.Service.Method"；无 package 时是 "Service.Method"。
  std::string full_name;
  // IDL 里的 package 名（可为空）。
  std::string package_name;
  // 服务名（不含 package）。
  std::string service_name;
  // 方法名。
  std::string method_name;
  // 请求消息类型全名。
  std::string request_type;
  // 响应消息类型全名。
  std::string response_type;
  // IDL 中是否把该方法声明为幂等（影响客户端是否允许自动重试）。
  bool idempotent = false;
  // method_id 是否由 IDL 用 `[method_id = 0x...]` 显式指定。
  //
  // 为什么需要这个标志：RegisterMethod 的 L2 校验会重算 MethodIdOfName(full_name)
  // 并与 method_id 比对，哈希不一致就报错——这对"哈希算出来的 id"是对的，
  // 但会把"显式指定 id"的合法场景误判成篡改。
  // 因此约定：
  //   * false（默认，哈希派生）→ 重算结果必须相等，不等即 ERROR；
  //   * true（IDL 显式指定）  → 跳过该比对，但仍参与冲突检测（同 id 不同名仍是 ERROR）。
  bool method_id_overridden = false;
};

// 方法元信息目录（单例）。
class ServiceRegistry {
 public:
  // 取全局实例。线程安全。
  static ServiceRegistry& Instance();

  // 登记一个方法。
  // 参数：info - 元信息（method_id 会被重新计算并与传入值比对）；
  //       err - 可选，失败原因。
  // 返回：true 成功；
  //       false 表示 (a) method_id 与 full_name 计算结果不一致（codegen bug），
  //                 或 (b) method_id 已被**另一个不同** full_name 占用（哈希冲突）。
  bool RegisterMethod(const MethodInfo& info, std::string* err = nullptr);

  // 按 method_id 查信息。未登记返回 nullptr。
  const MethodInfo* FindMethod(std::uint32_t method_id) const;

  // 按全名查信息。未登记返回 nullptr。
  const MethodInfo* FindByName(const std::string& full_name) const;

  // 把 method_id 翻译成可读名；未登记时返回 "unknown#0x1234abcd" 形式的串。
  std::string DescribeMethod(std::uint32_t method_id) const;

  // 全部方法（按 method_id 升序）。
  std::vector<MethodInfo> AllMethods() const;

  // 全部服务名（去重、升序）。
  std::vector<std::string> ServiceNames() const;

  // 已登记方法数。
  std::size_t size() const;

  // 一致性自检。
  // 返回：问题描述列表；空表示全部一致。启动时应打印非空结果并据此决定是否退出。
  std::vector<std::string> SelfCheck() const;

  // 清空（仅测试用）。
  void Clear();

 private:
  ServiceRegistry() = default;

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::uint32_t, MethodInfo> by_id_;
  std::unordered_map<std::string, std::uint32_t> by_name_;
};

}  // namespace rpc
}  // namespace qzrpc
