// 轻舟 RPC (Qingzhou RPC)
// src/client/registry_client.h
//
// 职责：注册中心客户端。周期性向 RegistryServer 拉取某个服务的实例列表，
//       变化时回调通知上层（Channel 据此更新 LoadBalancer）。
//
// 设计取舍：
//   * **拉模式（poll）而不是推模式**：注册中心不需要维护长连接与回调通道，
//     服务端实现量减少一半；代价是最大 registry_poll_interval_ms 的发现延迟，
//     对"每分钟扩容一次"的常规业务完全够用。这是非常有意识的取舍。
//   * **拉取失败不清空列表**：网络抖动时继续用最后一次成功的结果（标记 degraded），
//     避免"注册中心挂了导致全站不可用"的雪崩。
//   * **不抢 Channel 的连接池**：RegistryClient 内部自建一个极简的同步 HTTP-less 客户端
//     （实际上是独立的一次性短连接 + Codec），避免和业务调用互相拖累。
//
// 线程安全：Start/Stop/RefreshNow/endpoints/state 全部线程安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/client_options.h"
#include "common/noncopyable.h"
#include "net/socket_util.h"

namespace qzrpc {
namespace rpc {

// 注册中心客户端。
class RegistryClient : private NonCopyable {
 public:
  // 实例列表更新回调（在内部工作线程执行）。
  // 参数：endpoints - 最新实例地址列表（可能为空）。
  // 约束：回调内不得阻塞（不要在里面建连接或做 DNS），只应更新 LoadBalancer。
  using UpdateCallback = std::function<void(const std::vector<net::Endpoint>& endpoints)>;

  // 构造。参数：options - 复用 ClientOptions 中的 registry_* 字段与超时配置。
  explicit RegistryClient(const ClientOptions& options);
  ~RegistryClient();

  // 设置更新回调。**必须在 Start() 之前设置**。
  void SetUpdateCallback(UpdateCallback cb);

  // 启动后台拉取线程，并立即拉取一次。
  // 返回：true 表示首次拉取成功（失败也返回 false 但后台线程会继续重试）。
  bool Start(std::string* err);

  // 停止后台线程。幂等。
  void Stop();

  // 立即同步拉取一次（用于测试与手动刷新）。线程安全。
  bool RefreshNow(std::string* err);

  // 最近一次成功拉取到的实例列表。从未成功过时返回空。
  std::vector<net::Endpoint> endpoints() const;

  // 是否处于降级状态（最近一次拉取失败，正在用旧数据或空数据）。
  bool degraded() const noexcept;

  // 是否正在运行。
  bool running() const noexcept;

  // 最近一次成功拉取的时间（单调时钟毫秒）；从未成功为 0。
  std::uint64_t last_success_ms() const noexcept;

  // 累计拉取成功/失败次数（metrics 用）。
  std::uint64_t fetch_success_total() const noexcept;
  std::uint64_t fetch_failure_total() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rpc
}  // namespace qzrpc
