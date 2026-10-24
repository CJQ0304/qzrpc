// 轻舟 RPC (Qingzhou RPC)
// src/client/client_options.h
//
// 职责：客户端全部可调参数（POD 结构体，构造后只读）。
//
// 与 phxrpc 的差异：
//   phxrpc 的 ClientConfig 只支持 IP:Port 列表 + 两个超时，且是全局静态对象；
//   这里把「连接池、重试、负载均衡、注册发现、熔断」全部收进一个不可变结构体，
//   使 Client（Channel）可以按需实例化多个，互不干扰（单进程多租户）。
//
// 线程安全：普通值类型；构造完成后只读使用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net/socket_util.h"
#include "protocol/protocol_const.h"

namespace qzrpc {

class Config;

namespace rpc {

// 客户端配置。
struct ClientOptions {
  // ---- 目标 ----

  // 服务名。用于注册中心查询与日志、metrics label。
  std::string service_name = "qzrpc-service";

  // 静态 endpoint 列表。不使用注册中心时由这里指定。
  // 使用注册中心时，这里是"启动时的兜底地址"（注册中心不可用时仍能工作）。
  std::vector<net::Endpoint> endpoints;

  // 调用方标识，写入请求 metadata 的 caller 字段，便于服务端定位上游。
  std::string caller_name;

  // ---- 超时与重试 ----

  // 建立连接超时（毫秒）。
  int connect_timeout_ms = 200;

  // 单次请求默认超时（毫秒）。CallOptions::timeout_ms 可逐次覆盖。
  int request_timeout_ms = 1000;

  // 默认最大重试次数（不含首次）。0 表示不重试。
  //
  // ★ 只有三类失败才会真正重试（判据集中在 channel.cpp 的 RetryableBeforeSend /
  //   RetryableAfterSend，全框架只此一处；散落的 `if (can_retry)` 是这类系统最常见的长期缺陷）：
  //   1) 请求字节**还没发出去**：建连失败 / 建连超时。一个字节都没发 → 重试绝对安全，
  //      **不要求 idempotent**。
  //      ⚠️ 有一个例外：**连接池饱和**（`AcquireFailure::kPoolFull`）虽然也属于"没发出去"，
  //      但**不重试**——上限是本进程自己的配置，再试一次不会变大，只会把争抢放大；
  //      它会以 `kOverload` 直接返回给调用方，让对方退避（见 doc 02 §4.3.2）；
  //   2) 服务端**明确拒绝**：kOverload（队列满 / 自适应限流），以及"能拿到响应帧的"
  //      kServiceUnavailable（对端自己声明不收）。业务没被执行过 → 不要求 idempotent。
  //      kOverload 的退避时间**加倍**，避免把刚缓过来的服务再打垮；
  //   3) 已发出且结果未知：kTimeout / kCancelled。服务端可能已经执行完、只是回包丢了，
  //      所以**只有 opts.idempotent == true 才重试**。
  //   业务错误（kInvalidArgument / kNotFound / kSerializeError / kInternal）**永不重试**。
  //
  // ★ 全部尝试**共用一个 deadline**（opts.timeout_ms，缺省取 request_timeout_ms）：
  //   "配了 1000" = 整次调用最多花 1000ms，不是"每次尝试 1000ms"。
  //   每次尝试的等待与退避都被剩余时间截断，所以本地等待超时把预算耗尽后，
  //   循环顶部的 deadline 检查直接退出 —— **不会**出现"A 超时了再全量重试一轮"，
  //   也就不会把 1s 的调用拖成 5s。反之，用户配 1000 就应该能用满 1000。
  //
  // ★ 因此默认 2 是安全的：它放大的是"没发出去 / 被明确拒绝"的调用，
  //   而不是"业务已经跑过一遍"的调用。**不要**为了"更保守"改成 0 ——
  //   那只会把建连抖动直接冒到业务层，而不会减少任何真实的重复执行。
  //
  // 注意：v1 **没有**"故障转移到另一个 endpoint 且不计入 max_retries"的独立预算。
  //       每次尝试都会按 attempt 选一个（尽量不同的）endpoint，并且都算在 max_retries 里。
  //       想让多实例切换更宽容，就调大 max_retries —— 不要期待隐式的额外重试。
  int max_retries = 2;

  // 重试前的固定退避（毫秒）。实际的第 n 次重试等待 n * retry_backoff_ms。
  int retry_backoff_ms = 20;

  // 从连接池借连接的最长等待（毫秒）。
  int acquire_timeout_ms = 200;

  // ---- 连接池 ----

  // 每个 endpoint 维持的长连接数。1 表示串行，压测高并发时建议 2~4。
  std::size_t connections_per_endpoint = 2;

  // 空闲连接超过该时长（毫秒）由后台回收。0 表示不回收。
  int idle_connection_timeout_ms = 60000;

  // 是否开启 TCP_NODELAY。
  bool enable_nodelay = true;

  // 单帧 body 上限。必须与服务端一致，否则会收到 kInvalidArgument。
  std::uint32_t max_frame_size = protocol::kDefaultMaxFrameSize;

  // ---- 熔断 ----

  // 连续失败多少次后把该 endpoint 标记为不可用。
  int breaker_failure_threshold = 5;

  // 熔断冷却时间（毫秒），到期后允许一次试探。
  int breaker_cooldown_ms = 5000;

  // ---- 服务发现 ----

  // 注册中心地址（"ip:port"）。为空表示不使用注册发现，只用静态 endpoints。
  std::string registry_endpoint;

  // 拉取服务列表的周期（毫秒）。
  int registry_poll_interval_ms = 3000;

  // 单次拉取的超时（毫秒）。
  int registry_request_timeout_ms = 1000;

  // ---- 可观测性 ----

  // 是否采集客户端指标。
  bool enable_metrics = true;

  // ---- 序列化 ----

  // 自动生成 trace_id（每次调用一个随机的 16 进制串）。
  bool auto_trace_id = true;

  // ---- 方法 ----

  // 从 Config 填充（缺失项保留默认值）。
  bool FromConfig(const Config& cfg, const std::string& section = "Client",
                  std::string* err = nullptr);

  // 把 endpoints 也写进配置的能力（phxrpc 那种 [Server0..N] 写法）。
  // 参数：cfg - 配置对象（会被写入 section 下的 endpoint_count 与 endpoint_i）
  void WriteTo(Config* cfg, const std::string& section = "Client") const;

  // 生成默认配置文本。
  std::string ToIniText() const;

  // 一行摘要（日志用，ENDpoint 数量可能很多时只打前 3 个）。
  std::string ToString() const;

  // 校验：至少有一个 endpoint 或配置了 registry_endpoint；超时为正等。
  bool Validate(std::string* err) const;
};

}  // namespace rpc
}  // namespace qzrpc
