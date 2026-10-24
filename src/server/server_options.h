// 轻舟 RPC (Qingzhou RPC)
// src/server/server_options.h
//
// 职责：服务端全部可调参数。**这是一个 POD 结构体，不是"配置类"**。
//
// 为什么不做成 phxrpc 那种 ClientConfig/ServerConfig 类层级：
//   * 类层级 + DoRead 虚函数会让"加一个字段"必须改 4 处；
//   * 全局 static 配置对象让"单进程多实例"不可能，也让线程安全靠约定；
//   * POD + FromConfig 的写法：字段即文档，构造后可直接 const，天然线程安全。
//
// 线程安全：结构体本身是普通值类型；构造完成后只读使用。
#pragma once

#include <cstdint>
#include <string>

#include "common/config.h"
#include "protocol/protocol_const.h"

namespace qzrpc {
namespace rpc {

// 服务端配置。
struct ServerOptions {
  // ---- 网络 ----

  // 监听地址。"0.0.0.0" 表示全部网卡。
  std::string bind_ip = "0.0.0.0";
  // 监听端口。0 表示由系统分配（测试常用，之后用 Server::port() 取实际值）。
  std::uint16_t port = protocol::kDefaultPort;
  // listen backlog。
  int backlog = 1024;

  // ---- 线程模型 ----

  // IO 从 Reactor 线程数（每个线程一个 EventLoop，负责一批连接的读写）。
  // 建议值：min(CPU 核数, 8)。主 Reactor（accept）另有 1 个线程。
  std::size_t io_thread_count = 4;
  // 业务线程数。业务里有阻塞调用（磁盘/下游 RPC）时必须给足。
  std::size_t worker_thread_count = 16;
  // 业务队列（有界）长度上限。满了即回 OVERLOAD。
  std::size_t max_pending_tasks = 20000;

  // ---- 连接与超时 ----

  // 最大并发连接数。达到上限后新 accept 的连接被立即关闭（回 RST，因为此时还没读到帧）。
  std::size_t max_connections = 10000;
  // 单帧 body 上限（metadata + payload），超过即断开。
  std::uint32_t max_frame_size = protocol::kDefaultMaxFrameSize;
  // 连接空闲回收时间（毫秒）。0 表示不回收。
  int idle_timeout_ms = 0;
  // 单次 accept 事件里最多处理多少个连接。
  int max_accept_batch = 64;
  // 是否开启 TCP_NODELAY。
  bool enable_nodelay = true;

  // ---- 业务超时 ----

  // 业务处理超时（毫秒）。仅用于"超时后记录 metrics 并回 kTimeout"，**不强行杀死线程**
  // （C++ 无法安全中断线程）。超时响应的 seq 与原请求一致。
  int request_timeout_ms = 3000;

  // ---- 过载保护 ----

  // 是否启用基于排队时延闭环的自适应拒绝。
  bool enable_adaptive_reject = true;
  // 排队时延阈值（毫秒）：超过则开始按比例拒绝。
  int fast_reject_threshold_ms = 20;
  // 每次调节的比例步长（百分比）。越大收敛越快但越容易震荡，建议 5。
  int fast_reject_adjust_rate = 5;

  // ---- 可观测性 ----

  // 是否启用内建 metrics（Counter/Histogram 采集）。
  bool enable_metrics = true;
  // 独立的 metrics HTTP 端口（0 表示不启动）。v1 只提供极简 HTTP/1.0 GET /metrics。
  std::uint16_t metrics_port = 0;
  // metrics 监听地址。
  std::string metrics_bind_ip = "127.0.0.1";
  // 服务名，用于 metrics label 与日志。
  std::string service_name = "qzrpc-server";

  // ---- 优雅停机 ----

  // 排空在飞请求的最长等待时间（毫秒）。超时后强制关闭连接。
  int graceful_shutdown_timeout_ms = 5000;

  // 日志级别（取值同 qzrpc::Logger::Level）。
  int log_level = 2;

  // ---- 服务发现（可选，`Server` 自己实现自注册） ----

  // 注册中心地址 "ip:port"。空串表示不注册（使用静态 endpoint 的部署方式）。
  //
  // 非空时 `Server` 会起一条**独立线程**做自注册（`Server::Impl::RegistryLoop`），
  // 时序是：监听成功之后才启动（所以 `port = 0` 也能登记正确端口）→ Register
  // → 每 `registry_heartbeat_interval_ms` 一次 Heartbeat → 停机时一次 Deregister。
  // 三条刻意的行为：
  //   * 注册失败**不让服务启动失败**：只打 WARN 并按 2s 间隔重试（注册中心比我们起得晚
  //     是常态）；本机始终正常提供服务，只是新客户端发现不到它；
  //   * 心跳返回 `not_found`（TTL 到期 / 注册中心重启过）→ 立刻回到未注册态重新登记，
  //     不自愈的话我们会"自以为活着"；
  //   * 对外宣告的地址取 `bind_ip`；若它是通配地址（`0.0.0.0` / `::` / 空），
  //     **退化成 127.0.0.1 并 WARN** —— 跨机部署必须显式配 `bind_ip`。
  // 格式非法（不是 `ip:port`）时打 ERROR 并**只禁用自注册**，不影响服务。
  // 完整契约见 doc 02 §2.4.1。
  std::string registry_endpoint;

  // 心跳间隔（毫秒）。实际生效值被**夹紧到 [100, 60000]**。
  // 设计上应显著小于注册中心侧的 `instance_ttl_ms`（`Server` 用的租约是 15000），
  // 通常取 TTL 的 1/3 —— 这也是默认 5000 的由来。
  // 注意：`Validate` 不检查它与 TTL 的关系（它不知道对端 TTL 是多少），配大了会
  // "自我摘除后反复重注册"，症状是注册表里该实例周期性消失。
  int registry_heartbeat_interval_ms = 5000;

  // 从 Config 填充本结构。缺失项保留当前默认值。
  // 参数：cfg - 已加载的配置；section - 段名；err - 可选，解析失败（字段非法）时写原因。
  // 返回：true 表示全部已出现的字段都合法。
  bool FromConfig(const Config& cfg, const std::string& section = "Server",
                  std::string* err = nullptr);

  // 生成一份带注释的默认配置文本（example 与文档直接用）。
  std::string ToIniText() const;

  // 一行摘要，启动时打日志用。
  std::string ToString() const;

  // 校验参数合法性（端口范围、线程数 >= 1、frame 上限 <= 硬上限等）。
  // 返回：true 合法；false 时 err 写原因。
  bool Validate(std::string* err) const;
};

}  // namespace rpc
}  // namespace qzrpc
