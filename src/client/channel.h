// 轻舟 RPC (Qingzhou RPC)
// src/client/channel.h
//
// 职责：客户端顶层门面。把「负载均衡 + 连接池 + 注册发现 + 重试 + 超时 + metrics」
//       组装成一个对业务透明的调用入口。生成的 Stub 只持有 Channel&。
//
// 一次 Call 的完整流程（详见 doc 02 §4）：
//   1. 计算 deadline = now + timeout_ms；
//   2. for attempt in [0, max_retries]:
//        a. LoadBalancer 选 endpoint（跳过熔断中的）；
//        b. ConnectionPool 借一条连接（超时受 acquire_timeout_ms 限制）；
//        c. 序列化请求 → 构造带 seq 的帧 → 发送 → 阻塞等待响应（受剩余 deadline 限制）；
//        d. 成功则反序列化返回；失败则按错误类别 + 幂等标记决定是否重试；
//   3. 归还或失效连接；记录 metrics。
//
// 错误分类与重试策略（与 doc 02 §4.3 的矩阵、channel.cpp 的两个判据函数一致）：
//   判据分两段，**"字节有没有发出去"是分界线**：
//   * before-send（建连失败 / 建连超时）：重试一定安全，不要求 idempotent；
//     例外是**连接池饱和**（`AcquireFailure::kPoolFull`）——不重试，直接以 `kOverload` 返回，
//     因为上限是本进程配置，重试只会放大争抢（doc 02 §4.3.2）；
//   * after-send：
//       - kOverload            → 可重试，退避时间加倍（服务端明确拒绝，业务没执行）；
//       - kServiceUnavailable  → 可重试（拿到响应帧说明是对端声明不收，同样没执行）；
//       - kTimeout / kCancelled→ **只有 idempotent == true 才重试**（可能已执行，只是回包丢了）；
//       - kInvalidArgument / kNotFound / kSerializeError / kInternal → **不重试**（业务错误）。
//   所有尝试共用一个 deadline（见下），所以本地等待超时耗尽预算后不会再发起新一轮尝试。
//
// 线程安全：Channel 的全部方法线程安全，多个业务线程可共享同一个 Channel。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/client_options.h"
#include "client/load_balancer.h"
#include "common/noncopyable.h"
#include "common/status.h"
#include "net/socket_util.h"

namespace qzrpc {

namespace serde {
class Message;
}  // namespace serde

namespace rpc {

// 单次调用的可选参数。默认值来自 Channel 构造时的 ClientOptions。
struct CallOptions {
  // 本次调用超时（毫秒）。0 表示使用 ClientOptions::request_timeout_ms。
  int timeout_ms = 0;

  // 最大重试次数（不含首次）。-1 表示使用 ClientOptions::max_retries。
  int max_retries = -1;

  // 重试退避基数（毫秒）。-1 表示使用 ClientOptions::retry_backoff_ms。
  int retry_backoff_ms = -1;

  // 是否幂等。**只有幂等调用才会被自动重试**（默认 false，安全优先）。
  bool idempotent = false;

  // 一致性哈希键。为空时一致性哈希策略退化为轮询。
  std::string hash_key;

  // 指定的 trace_id。为空且 ClientOptions::auto_trace_id 为真时自动生成。
  std::string trace_id;

  // 服务端业务超时（毫秒）。0 表示不向服务端声明（服务端用自身默认值）。
  std::uint32_t server_timeout_ms = 0;
};

// 客户端通道。
class Channel : private NonCopyable {
 public:
  // 构造。参数：options - 客户端配置（按值拷贝并校验）。
  explicit Channel(const ClientOptions& options);
  ~Channel();

  // 配置是否合法（详见 ClientOptions::Validate）。
  bool valid() const noexcept;

  // 配置非法时的原因。
  const std::string& error_message() const noexcept;

  // 生效的配置。
  const ClientOptions& options() const noexcept;

  // 启动：创建事件循环线程、连接池、注册中心客户端（若配置了）。
  // 参数：err - 失败原因。
  // 返回：true 启动成功。重复调用返回 true 且不重复启动。
  bool Start(std::string* err = nullptr);

  // 停止：关闭连接池与后台线程。幂等。
  void Stop();

  // 是否已启动。
  bool started() const noexcept;

  // 发起一次同步（阻塞）RPC 调用。
  // 参数：method_id - 方法 id（codegen 生成的常量）；
  //       request - 请求消息（会被序列化）；
  //       response - 响应消息（成功时被反序列化填充；可为 nullptr 表示不关心响应体）；
  //       opts - 单次调用参数。
  // 返回：Status。这是**唯一**可能失败的返回点，不抛异常。
  // 线程安全：是。
  // 超时预算：opts.timeout_ms 是**整次调用**（含全部重试）的上限，不是单次尝试的上限——
  //           每次尝试的等待必须收敛到剩余 deadline，否则重试会把 1s 的调用拖成 5s。
  Status Call(std::uint32_t method_id, const serde::Message& request, serde::Message* response,
              const CallOptions& opts = CallOptions());

  // 便捷重载：用方法全名（"pkg.Svc.Method"）代替 method_id。
  // 内部用 protocol::MethodIdOfName 换算，等价于传常量。
  Status Call(const std::string& method_full_name, const serde::Message& request,
              serde::Message* response, const CallOptions& opts = CallOptions());

  // 替换负载均衡策略（默认轮询）。**只能在 Start() 之前调用**。
  void SetLoadBalancer(std::unique_ptr<LoadBalancer> balancer);

  // 当前使用的负载均衡器（非空，生命周期同 Channel）。
  LoadBalancer* load_balancer() const;

  // 当前可用 endpoint 列表（静态配置 + 注册中心结果的并集去重）。
  std::vector<net::Endpoint> endpoints() const;

  // 立即刷新一次服务发现（配置了注册中心时有效）。线程安全。
  bool RefreshEndpoints(std::string* err);

  // 注册客户端标准指标（幂等）。
  //   qzrpc_client_request_total      (Counter)   累计调用数（含重试）
  //   qzrpc_client_response_total     (Counter)   累计成功数
  //   qzrpc_client_error_total{code}  (Counter)   按状态码分桶（code 见 StatusCodeName）
  //   qzrpc_client_retry_total        (Counter)   累计**业务重试**次数（不含故障转移）
  //   qzrpc_client_failover_total     (Counter)   累计**故障转移**次数：建连失败/冷却中时
  //                                               立刻换实例，不睡、不占 max_retries（§4.3.3）
  //   qzrpc_client_latency_us         (Histogram) 端到端延迟
  //   qzrpc_client_pool_acquire_us    (Histogram) 借连接耗时
  //   qzrpc_client_late_response_total(Counter)   超时后到达、被就地丢弃的响应数
  //                                               （doc 03 §3.4 S-2）。
  //       ★ **当前走连接池的路径下恒为 0**：`Call` 超时即把连接标不健康 → `Lease` 析构
  //         `Invalidate` → 连接当场关闭 → 迟到响应到达时连接已不存在，那个 Inc 分支不会执行。
  //         非零只可能来自"绕过连接池直接多线程使用 `ClientConnection`"的误用。
  //         待连接池改造（超时不再作废连接、由墓碑吸收迟到响应）落地后，它才变成
  //         "有服务端慢于 deadline" 的真信号 —— 到那时请把本段删掉。
  static void RegisterDefaultMetrics();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rpc
}  // namespace qzrpc
