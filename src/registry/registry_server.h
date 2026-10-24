// 轻舟 RPC (Qingzhou RPC)
// src/registry/registry_server.h
//
// 职责：内置的极简服务注册中心。它**本身就是一个 qzrpc::rpc::Server**，
//       只是把 4 个 RPC 方法（Register / Heartbeat / Fetch / Deregister）注册进去，
//       外加一个 TTL 清扫线程。这让"框架自举"成为可验证的事实：
//       注册中心跑不通，说明框架本身有问题。
//
// 为什么不引入 etcd / consul / zookeeper：
//   * 三者都需要外部部署与运维，简历项目里"跑起来要 3 个容器"是减分项；
//   * 自研的 200 行注册中心能覆盖"注册 / 心跳 / 拉取 / TTL 过期"全部核心语义，
//     足够展示对服务发现本质的理解；
//   * 且它是"用自己框架写的第一个真实服务"，是端到端可用性的最强证据。
//
// 一致性模型：**最终一致 + 客户端拉取**。
//   实例注册后最多 registry_poll_interval_ms 才被消费者看到；实例宕机后最多
//   instance_ttl_ms 才被摘除。RPC 场景下这个延迟完全可接受，换来的是极简实现。
//
// 线程安全：Start/Run/Stop/running/port/ListInstances 全部线程安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/noncopyable.h"
#include "registry/registry_client_proto.h"
#include "server/server_options.h"

namespace qzrpc {
namespace registry {

// 注册中心配置。
struct RegistryServerOptions {
  // 复用的服务端配置。port 默认应设为 protocol::kDefaultRegistryPort。
  rpc::ServerOptions server;

  // 实例租约时长（毫秒）。超过该时长没有心跳即被摘除。
  int instance_ttl_ms = 15000;

  // 清扫线程周期（毫秒）。
  int sweep_interval_ms = 3000;

  // 是否允许同一 (service, host, port) 重复注册（覆盖而非报错）。
  bool allow_overwrite = true;

  RegistryServerOptions();

  // 校验（委托 server.Validate 并检查 ttl/sweep 为正）。
  bool Validate(std::string* err) const;
};

// 注册中心服务端。
class RegistryServer : private NonCopyable {
 public:
  RegistryServer();

  // 构造。参数：options - 配置（非法时 valid() 为 false）。
  explicit RegistryServer(const RegistryServerOptions& options);

  ~RegistryServer();

  // 配置是否合法。
  bool valid() const noexcept;

  // 配置非法原因。
  const std::string& error_message() const noexcept;

  // 启动监听与 RPC 服务（非阻塞）。
  bool Start(std::string* err = nullptr);

  // 阻塞运行直到 Stop()。
  void Run();

  // 优雅停机。
  void Stop();

  // 是否正在运行。
  bool running() const noexcept;

  // 实际监听端口。
  std::uint16_t port() const noexcept;

  // ---- 只读视图（供调试/测试/示例页面） ----

  // 某服务当前全部健康实例（已按 TTL 过滤）。
  std::vector<ServiceInstance> ListInstances(const std::string& service_name) const;

  // 已注册的服务名列表（升序）。
  std::vector<std::string> ListServices() const;

  // 服务数量。
  std::size_t service_count() const;

  // 实例总数。
  std::size_t instance_count() const;

  // 累计 注册/心跳/拉取/过期 次数（metrics 用）。
  std::uint64_t register_total() const noexcept;
  std::uint64_t heartbeat_total() const noexcept;
  std::uint64_t fetch_total() const noexcept;
  std::uint64_t expired_total() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace registry
}  // namespace qzrpc
