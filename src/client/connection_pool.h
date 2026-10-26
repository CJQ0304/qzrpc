// 轻舟 RPC (Qingzhou RPC)
// src/client/connection_pool.h
//
// 职责：客户端长连接池 + 连接上的请求多路复用。
//
// 两个类：
//   * ClientConnection —— 一条到某 endpoint 的长连接。负责 seq 分配、帧编码、等待响应、
//                         超时清理。**它以「同步阻塞」的形态对调用方呈现**，但底层仍是
//                         非阻塞 IO + 事件循环，所以不会被一条慢连接拖住其它调用。
//   * ConnectionPool  —— 每个 endpoint 维护 N 条 ClientConnection，做借用/归还、懒创建、
//                         失效剔除、空闲回收。
//
// 为什么"同步 + 阻塞等待"是合理设计：
//   业务代码写 RPC 调用的心智负担远比 callback / future 低；而并发能力通过
//   "多线程 + 每线程借一条连接" 获得，与 go 的 net/rpc、brpc 的同步客户端一致。
//   这不是"退化成阻塞 socket"：socket 依旧非阻塞，所有 IO 在独立的事件循环线程里完成。
//
// 线程安全：
//   ClientConnection::Call 允许多线程并发（靠 seq 多路复用），但推荐一个连接同一时刻
//   只被一个线程使用（ConnectionPool 的借用语义天然保证了这一点）。
//   ConnectionPool 的全部方法线程安全。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "client/client_options.h"
#include "common/buffer.h"
#include "common/noncopyable.h"
#include "common/status.h"
#include "net/connection.h"
#include "net/socket_util.h"
#include "protocol/frame.h"

namespace qzrpc {

namespace net {
class EventLoop;
}  // namespace net

namespace rpc {

// 一条客户端长连接。
class ClientConnection : private NonCopyable {
 public:
  using Ptr = std::shared_ptr<ClientConnection>;

  // 创建并接管一个已连接的 fd。
  // 参数：loop - 归属事件循环（非空）；fd - 已连接的非阻塞 fd（所有权移交）；
  //       peer - 目标端点；max_frame_size - 响应帧上限校验。
  static Ptr Create(net::EventLoop* loop, int fd, const net::Endpoint& peer,
                    std::uint32_t max_frame_size);

  ~ClientConnection();

  // 目标端点。
  const net::Endpoint& peer() const noexcept { return peer_; }

  // 归属事件循环。
  net::EventLoop* loop() const noexcept;

  // 创建时间（单调时钟毫秒）。
  std::uint64_t created_ms() const noexcept { return created_ms_; }

  // 最近一次使用时间（单调时钟毫秒）。空闲回收依据它判断。
  std::uint64_t last_used_ms() const noexcept;

  // 标记刚刚被使用过（Touch），刷新空闲计时。
  void Touch(std::uint64_t now_ms) noexcept;

  // 当前在飞请求数（已发出未收到响应）。
  std::size_t inflight() const noexcept;

  // 健康标记（熔断/连接池使用）。被标记为不健康后池不会再借出。
  bool healthy() const noexcept;
  void MarkHealthy() noexcept;
  void MarkUnhealthy() noexcept;

  // 连续失败次数（供熔断判定）。
  std::uint32_t consecutive_failures() const noexcept;

  // 是否已关闭。
  bool closed() const noexcept;

  // 发起一次同步 RPC。
  // 参数：method_id - 方法 id；
  //       payload - 已序列化的请求消息体；
  //       meta - 请求 metadata（trace_id / timeout / idempotent 等，会被拷贝）；
  //       timeout_ms - 本次等待上限（毫秒，>0）；
  //       resp_payload - 输出响应消息体（成功时写入，失败时清空）；
  //       resp_metadata - 输出响应 metadata（可为 nullptr）。
  // 返回：Status。失败时连接可能已被标记为不健康（kTimeout/kCancelled）。
  // 线程安全：是（不同线程用不同 seq）。
  // seq 槽位生命周期（doc 03 §3.4，四条都不能省）：
  //   * 正常完成 → 从 pending_ 删除槽位；
  //   * 超时 → **槽位保留**并标 abandoned，不能删（删了迟到响应会命中下一个同 seq 的调用，
  //     症状是"A 的响应交给 B"，只在超时+并发下偶发）；
  //   * 迟到响应命中 abandoned 槽 → 就地丢弃、删槽、late_response 计数，**不唤醒任何人**；
  //   * abandoned 槽位超过 256 个（`kMaxAbandonedSlots`）→ 主动关连接重建。
  //     **这是防御性上限，没有配套指标**，且**正常调用路径不可达**：走连接池的调用
  //     受池容量与在飞上限约束，单个连接上攒不出 256 个 abandoned 槽位。它防的是
  //     `ClientConnection` 被绕过连接池、直接多线程使用的误用 —— 真触发了说明用法
  //     已经越界，该看的是调用方代码，不是监控曲线。
  //   * 连接关闭（HandleClosed，loop 线程）→ 全部在飞调用**一次性以 kCancelled 唤醒**并清表，
  //     不能让它们各自等满 deadline，否则对端一崩业务线程池就被拖满。
  Status Call(std::uint32_t method_id, const std::string& payload,
              const protocol::Metadata& meta, int timeout_ms, std::string* resp_payload,
              protocol::Metadata* resp_metadata);

  // 关闭连接（线程安全，幂等）。
  void Close();

 private:
  ClientConnection(net::EventLoop* loop, const net::Endpoint& peer,
                   std::uint32_t max_frame_size);
  // 绑定 fd 并开始读。
  bool Attach(int fd);

  // 连接层回调（loop 线程）。
  void HandleBytes(const std::shared_ptr<net::Connection>& conn, Buffer* input);
  void HandleClosed(const std::shared_ptr<net::Connection>& conn);

  // 在飞请求记录。调用方的 Call 阻塞等待 done 变为 true。
  struct PendingCall {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    Status status;
    std::string payload;
    protocol::Metadata metadata;
  };

  net::Endpoint peer_;
  std::uint32_t max_frame_size_{0};
  std::uint64_t created_ms_{0};
  std::atomic<std::uint64_t> last_used_ms_{0};
  std::atomic<bool> closed_{false};
  std::atomic<bool> healthy_{true};
  std::atomic<std::uint32_t> consecutive_failures_{0};

  // 两个锁都是 mutable：`loop()` / `inflight()` / `closed()` 这些只读查询也要加锁，
  // 而"加锁"在语义上不改变对象的可观察状态。声明成 mutable 后 const 方法不必
  // const_cast（const_cast 去锁一个非 mutable 的锁是 UB 的边缘地带，不该出现在契约里）。
  mutable std::mutex conn_mutex_;
  std::shared_ptr<net::Connection> conn_;

  mutable std::mutex pending_mutex_;
  std::unordered_map<std::uint32_t, std::shared_ptr<PendingCall>> pending_;
  std::atomic<std::uint32_t> next_seq_{1};
};

// `Acquire` 失败原因。**调用方必须按它分流**，因为"借不到连接"有四种完全不同的含义，
// 而它们该触发完全不同的动作（重试 / 换实例 / 放弃 / 退避）：
//
// | 值 | 含义 | 上层该怎么处理 |
// | --- | --- | --- |
// | `kNone` | 成功（`out` 非空） | 正常使用 |
// | `kPoolFull` | 本进程连接数已达 `connections_per_endpoint` 上限且全部被占用，等到 deadline 无人归还 | **不要重试**：争抢只会更糟。建议映射为 `kOverload`（本地资源不足，让调用方/上游退避），而不是 `kServiceUnavailable`（那会去换实例，可实例本身没问题） |
// | `kConnectFailed` | connect 明确失败：拒绝 / 不可达 / 地址非法 | 按 before-send 重试（换实例），映射 `kServiceUnavailable` |
// | `kTimeout` | 建连把预算吃光了（connect 超时，或 deadline 在等连接时耗尽而当时并非"结构性满"） | 预算已尽 → 事实上不会再试；映射 `kTimeout` |
// | `kStopped` | 池已停 / 无 event loop / 建连期间被 `Stop()` | 映射 `kCancelled`，**不重试**（客户端在停机，重试没有意义） |
//
// 为什么要有这个枚举（而不是只给 err 文本）：`kPoolFull` 与 `kConnectFailed` 在旧接口里
// 都变成 `kServiceUnavailable`，而重试判据对它是无条件放行 —— **池越满、重试越多、池更满**，
// 这是标准的正反馈拥塞。类别必须能区分，才不会把"本地资源不足"误当成"下游不可用"。
enum class AcquireFailure {
  kNone = 0,
  kTimeout = 1,
  kPoolFull = 2,
  kConnectFailed = 3,
  kStopped = 4,
};

// 失败原因的可读名（日志/metrics label 用）。
const char* AcquireFailureName(AcquireFailure failure) noexcept;

// 连接池。
class ConnectionPool : private NonCopyable {
 public:
  // 构造。参数：loop - 客户端共享的事件循环（非空，由 Channel 持有）；
  //       options - 客户端配置（按值拷贝）。
  ConnectionPool(net::EventLoop* loop, const ClientOptions& options);
  ~ConnectionPool();

  // 启动（安装空闲回收定时器）。返回是否成功。
  bool Start(std::string* err);

  // 停止：关闭全部连接。幂等。
  void Stop();

  // 借出一条到 ep 的可用连接。
  //
  // 行为（三步，都不在事件循环线程上做 IO）：
  //   1) 有空闲连接 → 逐条校验（closed / unhealthy / 超过 idle_connection_timeout_ms 的直接换掉），
  //      合格的就地借出；
  //   2) 未达 connections_per_endpoint 上限 → **在调用方线程同步建连**
  //      （`SocketUtil::ConnectWithTimeout`，用 connect_timeout_ms 与剩余预算的较小者封顶）。
  //      注意：这是**阻塞调用方线程**的同步 connect，不是"扔给 IO 线程异步建连再回填"。
  //      设计阶段的"异步建连"方案被否掉，原因是它需要 loop 线程与调用方共享连接对象，
  //      在 Channel 停机路径上会撞上延迟析构（shared_ptr 最后一个引用必须在 loop 线程释放），
  //      反而更容易出问题；
  //   3) 已达上限 → 在条件变量上等，直到有人 Release / 池停 / 超时。
  //      **这是等待而不是忙等**：`cv.wait_for` 睡眠让出 CPU，没有轮询、没有 yield 循环。
  //
  // 参数：ep - 目标端点；timeout_ms - 最长等待毫秒（<=0 视为 1）；
  //       err - 失败原因文本（给人看，可能为空）；
  //       failure - 失败类别（给代码看，可为 nullptr）。
  //         写入规则见上面 `AcquireFailure` 的表格；成功时写 `kNone`。
  //         即使调用方不关心，也**建议**传：`Channel` 靠它决定"要不要重试"。
  // 返回：非空指针表示成功（调用方用完必须 Release 或 Invalidate）；nullptr 表示失败。
  //
  // ⚠️ 失败时**必须**同时填 `failure` 与 `err`：前者用于分流，后者用于日志。
  //    只填 err 的旧风格会让上层不得不去解析文本，那是上一版的正反馈拥塞来源。
  ClientConnection::Ptr Acquire(const net::Endpoint& ep, int timeout_ms, std::string* err,
                                AcquireFailure* failure = nullptr);

  // 归还一条仍然健康的连接。
  void Release(ClientConnection::Ptr conn);

  // 归还并判定失效：关闭它，不进入空闲队列。后续 Acquire 会重建。
  void Invalidate(ClientConnection::Ptr conn);

  // 某 endpoint 不再是候选（注册中心下线 / 熔断）时，关闭其全部连接。
  void RemoveEndpoint(const net::Endpoint& ep);

  // 某 endpoint 的当前空闲连接数。
  std::size_t idle_count(const net::Endpoint& ep) const;

  // 全部 endpoint 的空闲 + 使用中连接总数。
  std::size_t total_count() const;

  // 正在等待借出（阻塞中）的调用方数量，用于观测池容量是否不足。
  std::size_t pending_acquire_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rpc
}  // namespace qzrpc
