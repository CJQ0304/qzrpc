// 轻舟 RPC (Qingzhou RPC)
// src/server/server.cpp
//
// 职责：把「主 Reactor + N 个从 Reactor + 业务线程池 + 连接管理 + 优雅停机 + metrics」
//       组装成一个可直接 Run() 的对象。
//
// 线程清单（1 + N + M + 2）：
//   * 1 个 acceptor 线程：跑主 EventLoop，只做 accept + 把 fd 轮转分发给从 Reactor；
//   * N 个 io 线程：各跑一个 EventLoop，负责读帧 → 派发 → 写回；
//   * M 个 worker 线程：ThreadPool，跑业务；
//   * 1 个 housekeeping 线程：空闲连接回收、自适应拒绝调节、恢复被暂停的读；
//   * 1 个停机协调线程：只在 Stop() 时创建，跑完即退出。
//
// ★ 三个关键设计决策（都是"头文件没写死、由实现选定"的，改动集中在这里）：
//
//   1) 停机排空由**独立的短命协调线程**驱动，不是 Stop() 自己阻塞。
//      四步停机需要推进一串状态：等在飞请求排空（受 graceful_shutdown_timeout_ms 限制）
//      → 关连接 → 关监听 fd → Quit 所有 loop → join 全部线程。放到 housekeeping 线程里
//      会和它自己的周期任务（空闲回收、过载调节）抢同一根时间轴，停机时还得先停自己的
//      周期任务再排空，逻辑互相纠缠；放到 Wait() 里轮询则要求调用方必须调 Wait()，
//      只调 Stop() 的用法就会漏收敛。协调线程最干净：Stop() 只置原子状态 + 唤醒，
//      协调线程按状态机推进，Wait() 退化为纯 join。
//      两条约束：① Stop() 绝不阻塞在它身上（线程句柄存 Impl，由 Wait()/析构 join）；
//      ② 它自己不参与 pending 计数（否则排空条件永远不成立，自己等自己）。
//
//   2) metrics HTTP 端点**真实现**（不是在配置里假装支持）。
//      理由：ServerOptions 里有 metrics_port，静默忽略就是"配了没反应"的坑，
//      而 example 的前端页要 fetch /metrics。
//      行为被刻意收窄，避免变成攻击面：
//        * 只接受 `GET /metrics`；其余路径 404；**非 GET 方法 405**；`OPTIONS` 直接 204；
//        * HTTP/1.0 + `Connection: close`（读完请求头就回包并关连接，IO 线程里不做 HTTP
//          状态机、不解析 header/body，因此不存在"半截请求挂住线程"）；
//        * 请求头总量上限 8KB，超了直接关连接（挡住慢速/超长请求）；
//        * **无鉴权**，只应由 metrics_bind_ip 指定的回环/内网地址暴露，生产需前置网关。
//      `metrics_port == 0` 表示不启用该端点。
//
//   3) 连接数指标用 atomic 计数 + /metrics 里手写 gauge。
//      理由：server.h 把 qzrpc_server_connection_active 标成 Counter，但 Counter 只有
//      Inc()，无法表达"当前值下降"。这里用 atomic<int64> 维护真值，/metrics 输出时
//      手写一行 gauge，避免给出一个只会涨的假"活跃连接数"。
//
// 背压：业务队列满时（Submit 返回 kRejected）回 OVERLOAD 帧**并且**暂停该连接的读
//       （doc 03 §8.3 要求的做法），由 housekeeping 在队列排空后恢复。

#include "server/server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/buffer.h"
#include "common/logger.h"
#include "common/metrics.h"
#include "common/status.h"
#include "common/time_util.h"
#include "client/channel.h"
#include "client/client_options.h"
#include "net/acceptor.h"
#include "net/channel.h"
#include "net/connection.h"
#include "net/event_loop.h"
#include "net/socket_util.h"
#include "net/thread_pool.h"
#include "protocol/codec.h"
#include "protocol/frame.h"
#include "protocol/protocol_const.h"
#include "registry/registry_client_proto.h"

namespace qzrpc {
namespace rpc {

namespace {

// ---- 指标名（与 server.h 的注释逐字一致，改了就是破坏看板）----
constexpr const char* kMetricConnTotal = "qzrpc_server_connection_total";
constexpr const char* kMetricRequestTotal = "qzrpc_server_request_total";
constexpr const char* kMetricResponseTotal = "qzrpc_server_response_total";
constexpr const char* kMetricRejectedTotal = "qzrpc_server_rejected_total";
constexpr const char* kMetricErrorTotal = "qzrpc_server_error_total";
constexpr const char* kMetricLatencyUs = "qzrpc_server_request_latency_us";
constexpr const char* kMetricQueueWaitUs = "qzrpc_server_queue_wait_us";
// 业务执行耗时**总和**（微秒），是个只增不减的 Counter。
// 名字必须带 `_us_total`：`_busy` 在 Prometheus 语义里读起来像 Gauge，
// 做看板的人会直接画折线（看到一条只涨不跌的曲线），或者拿它算"占用率"得出荒谬结果。
// 计数器的惯例就是 `_total` 后缀（单位写在前面），这条不是审美问题。
constexpr const char* kMetricWorkerBusyUsTotal = "qzrpc_server_worker_busy_us_total";
constexpr const char* kMetricRegistryRegisterTotal = "qzrpc_server_registry_register_total";
constexpr const char* kMetricRegistryHeartbeatTotal = "qzrpc_server_registry_heartbeat_total";
constexpr const char* kMetricRegistryErrorsTotal = "qzrpc_server_registry_errors_total";
constexpr const char* kMetricMetricsConnRejectedTotal =
    "qzrpc_server_metrics_conn_rejected_total";

// ---- 自注册（服务发现的生产侧）的常量 ----
// 注册/心跳都是"小请求 + 幂等"，失败下一轮重来即可，所以不用重试机制（max_retries = 0）。
constexpr int kRegistryRequestTimeoutMs = 1000;
// 注册失败后的重试间隔。注册中心可能比我们起得晚（演示脚本就是先起 server 再起 registry）。
constexpr int kRegistryRetryIntervalMs = 2000;
// 停机等待的粒度：让 Stop() 之后 100ms 内本线程就能醒来退出。
constexpr int kRegistryWaitSliceMs = 100;
// 申请的租约时长（毫秒）。必须与注册中心侧的默认 instance_ttl_ms 一致（15000），
// 否则会出现"我们以为还有 30s，注册中心 15s 就把我们摘了"的错位。
constexpr std::uint32_t kRegistryLeaseTtlMs = 15000;

// 单次 Read 事件里最多解多少帧就主动让出？（0 表示不限制）
// 说明：Connection 用的是水平触发，message_cb 返回后如果 input_ 里还有残留字节，
// 不会再有新的可读事件（数据已经在用户态缓冲里），因此这里**必须**一直解到 kNeedMore，
// 否则会静默卡住连接。防饿死的杠杆用"队列满则暂停读"，不是在这里截断。
constexpr std::size_t kMaxMetricsHttpRequestBytes = 8192;

// /metrics 端点的**并发连接上限**（独立于 max_connections）。
// 为什么不复用 max_connections（默认 10000）：metrics 是运维端点，正常抓取是"一条短连接读一次"，
// 8~16 条就足够；而按业务端口的上限放开，等于给一个**无鉴权**端点开了 1 万个长连接的额度，
// 任何人把它 bind 到 0.0.0.0（跨机抓取是最自然的需求）就变成远程 fd/内存耗尽。
constexpr std::size_t kMaxMetricsConnections = 16;

// 每连接的用户态状态（挂在 Connection::context 上）。
struct ConnState {
  std::atomic<bool> read_paused{false};
};

// 极轻量的线程局部伪随机（避免 <random> 的构造开销与共享状态）。
std::uint32_t FastRand() {
  static thread_local std::uint32_t state = 0x9E3779B9U;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

std::uint8_t WithFlag(std::uint8_t flags, protocol::FrameFlags bit, bool on) {
  const std::uint8_t mask = static_cast<std::uint8_t>(bit);
  const std::uint8_t next = on ? static_cast<std::uint8_t>(flags | mask)
                               : static_cast<std::uint8_t>(flags & ~mask);
  return next;
}

// "ip:port" → Endpoint（只切最后一个冒号，兼容 "[::1]:16160"）。
// 与 server_options.cpp 的校验保持同一套规则：只认 IP 字面量，不做域名解析
// （注册中心地址写错必须在启动前报出来，而不是解析到某个意外的主机）。
bool ParseIpPort(const std::string& text, net::Endpoint* out) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
  std::string host = text.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  const long parsed = std::strtol(text.c_str() + colon + 1, nullptr, 10);
  if (parsed <= 0 || parsed > 65535) return false;
  if (out != nullptr) {
    out->ip = host;
    out->port = static_cast<std::uint16_t>(parsed);
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Server::Impl {
  explicit Impl(const ServerOptions& opts) : options(opts) {}

  // ---- 配置与路由（Start 前写入，之后只读）----
  ServerOptions options;
  bool valid = false;
  std::string error_message;
  Dispatcher dispatcher;

  // ---- 主 Reactor（声明顺序 = 销毁逆序：acceptor 先亡，loop 后亡）----
  std::unique_ptr<net::EventLoop> main_loop;
  std::unique_ptr<net::Acceptor> acceptor;
  std::thread main_thread;

  struct IoWorker {
    // 声明顺序很重要：conns 必须最先销毁（~Connection 会回访 loop_），
    // 而 loop 最后销毁。C++ 的成员销毁顺序是声明的逆序，正好满足。
    std::unique_ptr<net::EventLoop> loop;
    std::thread thread;
    std::unordered_map<int, std::shared_ptr<net::Connection>> conns;
  };
  std::vector<std::unique_ptr<IoWorker>> io_workers;
  std::atomic<std::size_t> next_io{0};

  std::unique_ptr<net::ThreadPool> workers;

  // ---- metrics HTTP（可选）----
  std::unique_ptr<net::EventLoop> metrics_loop;
  std::unique_ptr<net::Acceptor> metrics_acceptor;
  std::thread metrics_thread;
  std::unordered_map<int, std::shared_ptr<net::Connection>> metrics_conns;

  // ---- 后台线程 ----
  std::thread housekeeping_thread;
  std::thread coordinator_thread;
  // 自注册线程（registry_endpoint 非空时才起）。
  std::thread registry_thread;
  std::mutex registry_mutex;
  std::condition_variable registry_cv;
  std::atomic<bool> registry_registered{false};

  // ---- 状态 ----
  std::atomic<bool> started{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> finished{false};
  std::atomic<std::int64_t> conn_count{0};
  std::atomic<std::int64_t> global_pending{0};
  std::atomic<int> reject_rate{0};
  std::atomic<std::uint16_t> listen_port{0};

  // 自适应拒绝的滑动窗口（用 sum/count 而不是直方图：累计直方图无法反映"恢复"）
  std::atomic<std::uint64_t> win_wait_sum_us{0};
  std::atomic<std::uint64_t> win_wait_count{0};

  std::mutex run_mutex;
  std::condition_variable run_cv;
  std::mutex finish_mutex;
  std::condition_variable finish_cv;
  std::mutex housekeeping_mutex;
  std::condition_variable housekeeping_cv;

  // ---- 指标指针（进程级常驻，热路径直接解引用）----
  Counter* m_conn_total = nullptr;
  Counter* m_request_total = nullptr;
  Counter* m_response_total = nullptr;
  Counter* m_rejected_total = nullptr;
  Counter* m_error_total = nullptr;
  Counter* m_worker_busy = nullptr;
  Counter* m_registry_register = nullptr;
  Counter* m_registry_heartbeat = nullptr;
  Counter* m_registry_errors = nullptr;
  Counter* m_metrics_conn_rejected = nullptr;
  Histogram* m_latency = nullptr;
  Histogram* m_queue_wait = nullptr;

  // ---- 方法声明 ----
  bool ValidateOptions();
  void RegisterMetricPointers();

  bool StartAll(std::string* err);
  void MainLoopBody(const std::shared_ptr<std::promise<bool>>& ready,
                    const std::shared_ptr<std::string>& listen_err);
  void MetricsLoopBody(const std::shared_ptr<std::promise<bool>>& ready,
                       const std::shared_ptr<std::string>& listen_err);
  void MetricsConnectionInLoop(const std::shared_ptr<net::Connection>& conn);
  void WriteMetricsResponse(const std::shared_ptr<net::Connection>& conn, const std::string& request);

  void HousekeepingLoop();
  void CoordinatorLoop();
  void RegistryLoop();
  void AdjustRejectRate();
  void ReapIdleConnections();
  void ResumePausedReads();

  void OnNewConnection(int fd, const net::Endpoint& peer);
  void AttachConnectionInLoop(IoWorker* worker, int fd, const std::string& peer_text);
  void HandleMessage(IoWorker* worker, const std::shared_ptr<net::Connection>& conn, Buffer* input);
  void ReplyErrorInLoop(const std::shared_ptr<net::Connection>& conn, std::uint32_t seq,
                        std::uint32_t method_id, StatusCode code, bool keep_alive);
  // recv_us 是**收帧时刻（微秒）**：排队/业务/端到端三个指标都从它派生，
  // 详见 HandleRequestInWorker 里的时间戳分工。
  void HandleRequestInWorker(std::shared_ptr<net::Connection> conn, protocol::Frame frame,
                             bool one_way, bool keep_alive, std::uint64_t recv_us);
  bool ShouldReject() const;

  void CloseAllConnections();
  void JoinThreads();
  void RequestStop();
  bool WaitStop(int timeout_ms);
  void DropConnectionTables();
};

// ---------------------------------------------------------------------------
// 配置与指标
// ---------------------------------------------------------------------------

bool Server::Impl::ValidateOptions() {
  std::string err;
  if (!options.Validate(&err)) {
    valid = false;
    error_message = err;
    return false;
  }
  valid = true;
  error_message.clear();
  return true;
}

void Server::Impl::RegisterMetricPointers() {
  Server::RegisterDefaultMetrics();
  MetricsRegistry& registry = MetricsRegistry::Instance();
  m_conn_total = registry.GetCounter(kMetricConnTotal);
  m_request_total = registry.GetCounter(kMetricRequestTotal);
  m_response_total = registry.GetCounter(kMetricResponseTotal);
  m_rejected_total = registry.GetCounter(kMetricRejectedTotal);
  m_error_total = registry.GetCounter(kMetricErrorTotal);
  m_worker_busy = registry.GetCounter(kMetricWorkerBusyUsTotal);
  m_registry_register = registry.GetCounter(kMetricRegistryRegisterTotal);
  m_registry_heartbeat = registry.GetCounter(kMetricRegistryHeartbeatTotal);
  m_registry_errors = registry.GetCounter(kMetricRegistryErrorsTotal);
  m_metrics_conn_rejected = registry.GetCounter(kMetricMetricsConnRejectedTotal);
  m_latency = registry.GetHistogram(kMetricLatencyUs);
  m_queue_wait = registry.GetHistogram(kMetricQueueWaitUs);
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------

bool Server::Impl::StartAll(std::string* err) {
  if (started.load(std::memory_order_acquire)) return true;
  if (!ValidateOptions()) {
    if (err != nullptr) *err = error_message;
    QZ_LOG_ERROR << "server: invalid options: " << error_message;
    return false;
  }
  Logger::Instance().SetLevel(static_cast<Logger::Level>(options.log_level));
  RegisterMetricPointers();
  QZ_LOG_INFO << "server: starting " << options.ToString();

  // 业务线程池
  net::ThreadPool::Options pool_options;
  pool_options.thread_count = options.worker_thread_count;
  pool_options.max_queue_size = options.max_pending_tasks;
  pool_options.name = "qzrpc-worker";
  pool_options.allow_caller_run_on_reject = false;  // IO 线程绝不能被业务占用
  workers.reset(new net::ThreadPool(pool_options));
  workers->Start();

  // 从 Reactor：先建 loop（此时还没有任何线程在跑，构造里的 poller 注册是安全的），
  // 再起线程跑 Loop()。
  io_workers.reserve(options.io_thread_count);
  for (std::size_t i = 0; i < options.io_thread_count; ++i) {
    std::unique_ptr<IoWorker> worker(new IoWorker());
    worker->loop.reset(new net::EventLoop());
    IoWorker* raw = worker.get();
    io_workers.push_back(std::move(worker));
    raw->thread = std::thread([raw] { raw->loop->Loop(); });
  }

  // housekeeping
  housekeeping_thread = std::thread([this] { HousekeepingLoop(); });

  // metrics HTTP（可选）
  if (options.metrics_port != 0) {
    metrics_loop.reset(new net::EventLoop());
    auto ready = std::make_shared<std::promise<bool>>();
    auto listen_err = std::make_shared<std::string>();
    std::future<bool> future = ready->get_future();
    metrics_thread = std::thread([this, ready, listen_err] { MetricsLoopBody(ready, listen_err); });
    if (!future.get()) {
      if (err != nullptr) *err = "metrics listen failed: " + *listen_err;
      QZ_LOG_ERROR << "server: metrics endpoint failed: " << *listen_err;
      // 已经起来的线程由析构统一收尾（Stop/Wait 是幂等的）
      started.store(true, std::memory_order_release);
      RequestStop();
      return false;
    }
  }

  // 主 Reactor：accept 与 Listen 都必须在主 loop 线程里做（Channel 的线程约束）
  main_loop.reset(new net::EventLoop());
  auto ready = std::make_shared<std::promise<bool>>();
  auto listen_err = std::make_shared<std::string>();
  std::future<bool> future = ready->get_future();
  // 先置 started：这样后面任何失败路径都能走统一的 Stop()/Wait() 收尾
  started.store(true, std::memory_order_release);
  main_thread = std::thread([this, ready, listen_err] { MainLoopBody(ready, listen_err); });
  if (!future.get()) {
    if (err != nullptr) *err = *listen_err;
    QZ_LOG_ERROR << "server: listen failed: " << *listen_err;
    RequestStop();
    return false;
  }
  QZ_LOG_INFO << "server: listening on " << options.bind_ip << ":"
              << listen_port.load(std::memory_order_relaxed) << " io_threads="
              << options.io_thread_count << " workers=" << options.worker_thread_count;

  // 自注册线程：**在监听成功之后**才起（--port 0 时只有此刻才知道真实端口）。
  // 它起不来 / 注册失败都不影响本实例提供服务（注册中心挂了不该让全网停摆）。
  if (!options.registry_endpoint.empty()) {
    registry_thread = std::thread([this] { RegistryLoop(); });
  }
  return true;
}

void Server::Impl::MainLoopBody(const std::shared_ptr<std::promise<bool>>& ready,
                                const std::shared_ptr<std::string>& listen_err) {
  acceptor.reset(new net::Acceptor(main_loop.get(), options.bind_ip, options.port));
  std::string err;
  if (!acceptor->Listen(options.backlog, &err)) {
    *listen_err = err.empty() ? std::string("listen failed") : err;
    ready->set_value(false);
    return;
  }
  acceptor->set_accept_batch_size(options.max_accept_batch);
  acceptor->SetNewConnectionCallback(
      [this](int fd, const net::Endpoint& peer) { OnNewConnection(fd, peer); });
  acceptor->Start();
  listen_port.store(acceptor->local_port(), std::memory_order_release);
  ready->set_value(true);
  // 从这里开始本线程成为 loop 线程，直到 Quit()
  main_loop->Loop();
}

void Server::Impl::MetricsLoopBody(const std::shared_ptr<std::promise<bool>>& ready,
                                   const std::shared_ptr<std::string>& listen_err) {
  metrics_acceptor.reset(
      new net::Acceptor(metrics_loop.get(), options.metrics_bind_ip, options.metrics_port));
  std::string err;
  if (!metrics_acceptor->Listen(128, &err)) {
    *listen_err = err.empty() ? std::string("listen failed") : err;
    ready->set_value(false);
    return;
  }
  metrics_acceptor->set_accept_batch_size(32);
  net::EventLoop* loop = metrics_loop.get();
  metrics_acceptor->SetNewConnectionCallback(
      [this, loop](int fd, const net::Endpoint& peer) {
        // 上限判定必须在这里（accept 回调，loop 线程）：metrics_conns 只被本线程访问，
        // 所以不需要锁。超限直接关 fd —— 此时还没读任何字节，是最便宜的拒绝点。
        if (metrics_conns.size() >= kMaxMetricsConnections) {
          if (m_metrics_conn_rejected != nullptr) m_metrics_conn_rejected->Inc();
          QZ_LOG_WARN << "server: metrics connection limit reached (" << kMaxMetricsConnections
                      << "), refusing " << peer.ToString();
          net::SocketUtil::Close(fd);
          return;
        }
        std::shared_ptr<net::Connection> conn = net::Connection::Create(loop, fd, peer.ToString());
        if (!conn) {
          net::SocketUtil::Close(fd);
          return;
        }
        MetricsConnectionInLoop(conn);
      });
  metrics_acceptor->Start();
  ready->set_value(true);
  metrics_loop->Loop();
}

// ---------------------------------------------------------------------------
// 连接生命周期
// ---------------------------------------------------------------------------

void Server::Impl::OnNewConnection(int fd, const net::Endpoint& peer) {
  // 运行在主 loop 线程：这里只做"快速判定 + 轮转分发"，不碰任何 Connection。
  const std::int64_t current = conn_count.load(std::memory_order_relaxed);
  if (current >= static_cast<std::int64_t>(options.max_connections)) {
    // 此时还没读到帧，能做的只有直接关闭（客户端会收到 RST）。这是 doc 03 §9
    // 里"连接数超上限"的唯一合法处理方式。
    QZ_LOG_WARN << "server: connection limit reached (" << options.max_connections
                << "), refusing " << peer.ToString();
    net::SocketUtil::Close(fd);
    return;
  }
  if (io_workers.empty()) {
    net::SocketUtil::Close(fd);
    return;
  }
  const std::size_t index =
      next_io.fetch_add(1, std::memory_order_relaxed) % io_workers.size();
  IoWorker* worker = io_workers[index].get();
  net::EventLoop* loop = worker->loop.get();
  const std::string peer_text = peer.ToString();
  loop->RunInLoop([this, worker, fd, peer_text] {
    AttachConnectionInLoop(worker, fd, peer_text);
  });
}

void Server::Impl::AttachConnectionInLoop(IoWorker* worker, int fd,
                                          const std::string& peer_text) {
  // 运行在目标从 Reactor 线程：Connection 的全部非线程安全成员只在这里访问。
  std::shared_ptr<net::Connection> conn =
      net::Connection::Create(worker->loop.get(), fd, peer_text);
  if (!conn) {
    net::SocketUtil::Close(fd);
    QZ_LOG_ERROR << "server: failed to create connection for " << peer_text;
    return;
  }
  auto state = std::make_shared<ConnState>();
  conn->set_context(state);
  conn->SetMessageCallback(
      [this, worker, state](const std::shared_ptr<net::Connection>& c, Buffer* input) {
        if (state->read_paused.load(std::memory_order_relaxed)) {
          // 已暂停读：本轮回调仍可能有缓冲里的旧帧，照常处理但不额外读（见 HandleMessage）
        }
        HandleMessage(worker, c, input);
      });
  conn->SetCloseCallback([this, worker, fd](const std::shared_ptr<net::Connection>& c) {
    // 按指针身份删除：fd 可能在本回调被延迟执行之前就被新连接复用了，
    // 只按 fd 删会误删别人的连接。
    const auto it = worker->conns.find(fd);
    if (it != worker->conns.end() && it->second.get() == c.get()) {
      worker->conns.erase(it);
    }
    conn_count.fetch_sub(1, std::memory_order_relaxed);
  });
  worker->conns.emplace(fd, conn);
  conn_count.fetch_add(1, std::memory_order_relaxed);
  if (m_conn_total != nullptr) m_conn_total->Inc();
  conn->SetReadEnabled(true);
}

void Server::Impl::HandleMessage(IoWorker* worker, const std::shared_ptr<net::Connection>& conn,
                                 Buffer* input) {
  for (;;) {
    protocol::Frame frame;
    std::string err;
    const protocol::Codec::Result result =
        protocol::Codec::Decode(input, &frame, options.max_frame_size, &err);
    if (result == protocol::Codec::Result::kNeedMore) return;  // 半包，等下次可读
    if (result == protocol::Codec::Result::kError) {
      QZ_LOG_WARN << "server: protocol error from " << conn->peer_addr() << ": " << err;
      if (m_error_total != nullptr) m_error_total->Inc();
      conn->ForceClose();  // 二进制协议不做重同步
      return;
    }

    if (m_request_total != nullptr) m_request_total->Inc();
    const bool keep_alive = frame.header.keep_alive();
    const bool one_way = frame.header.one_way();

    // 服务端只接受请求帧。收到响应帧说明对端把方向搞反了（或是个扫描器）。
    if (!frame.header.is_request()) {
      QZ_LOG_WARN << "server: non-request frame from " << conn->peer_addr() << ", closing";
      conn->ForceClose();
      return;
    }

    // doc 03 §14 第 6 项：请求帧 status 必须为 0。这里按 INVALID_ARGUMENT 回错误，
    // **不**断连接 —— 能读到帧的场景永远回帧（这正是相对 phxrpc 的改进点之一）。
    if (frame.header.status != 0) {
      if (!one_way) {
        ReplyErrorInLoop(conn, frame.header.seq, frame.header.method_id,
                         StatusCode::kInvalidArgument, keep_alive);
      }
      continue;
    }

    // 停机中：新请求直接回 SERVICE_UNAVAILABLE（并在响应里置 kFlagDraining 提示换连接）
    if (stopping.load(std::memory_order_relaxed)) {
      if (!one_way) {
        ReplyErrorInLoop(conn, frame.header.seq, frame.header.method_id,
                         StatusCode::kServiceUnavailable, keep_alive);
      }
      continue;
    }

    // 自适应限流
    if (options.enable_adaptive_reject && ShouldReject()) {
      if (m_rejected_total != nullptr) m_rejected_total->Inc();
      if (!one_way) {
        ReplyErrorInLoop(conn, frame.header.seq, frame.header.method_id, StatusCode::kOverload,
                         keep_alive);
      }
      continue;
    }

    // 派发给业务线程池
    const std::uint32_t seq = frame.header.seq;
    const std::uint32_t method_id = frame.header.method_id;
    // 微秒精度：排队时延经常是几十微秒，用毫秒取整会让 queue_wait 全是 0 或 1000。
    const std::uint64_t recv_us = TimeUtil::NowUs();
    conn->IncrementPendingOps();
    global_pending.fetch_add(1, std::memory_order_relaxed);

    auto task = [this, conn, frame = std::move(frame), one_way, keep_alive, recv_us]() mutable {
      HandleRequestInWorker(std::move(conn), std::move(frame), one_way, keep_alive, recv_us);
    };
    const net::ThreadPool::SubmitResult submitted = workers->Submit(std::move(task));
    if (submitted != net::ThreadPool::SubmitResult::kOk) {
      // 队列满：回 OVERLOAD 帧（保持连接），并按 doc 03 §8.3 暂停本连接的读，
      // 让 TCP 接收窗口自然收窄，而不是把数据读进内存再丢。
      global_pending.fetch_sub(1, std::memory_order_relaxed);
      conn->DecrementPendingOps();
      if (m_rejected_total != nullptr) m_rejected_total->Inc();
      if (!one_way) ReplyErrorInLoop(conn, seq, method_id, StatusCode::kOverload, keep_alive);
      const auto state = std::static_pointer_cast<ConnState>(conn->context());
      if (state && !state->read_paused.exchange(true, std::memory_order_relaxed)) {
        conn->SetReadEnabled(false);
      }
    }
    (void)worker;
  }
}

void Server::Impl::ReplyErrorInLoop(const std::shared_ptr<net::Connection>& conn,
                                    std::uint32_t seq, std::uint32_t method_id, StatusCode code,
                                    bool keep_alive) {
  // 运行在 loop 线程：只组一个空 payload 的错误帧，成本极低。
  protocol::Frame response;
  response.ResetResponse(seq, method_id, static_cast<std::uint16_t>(code));
  response.header.flags = WithFlag(response.header.flags, protocol::kFlagKeepAlive, keep_alive);
  response.header.flags =
      WithFlag(response.header.flags, protocol::kFlagDraining, stopping.load());

  Buffer out;
  if (!protocol::Codec::Encode(&response, &out, options.max_frame_size)) {
    QZ_LOG_ERROR << "server: failed to encode error response for " << conn->peer_addr();
    conn->ForceClose();
    return;
  }
  conn->Send(out.Peek(), out.ReadableBytes());
  if (m_response_total != nullptr) m_response_total->Inc();
  if (m_error_total != nullptr) m_error_total->Inc();
}

bool Server::Impl::ShouldReject() const {
  const int rate = reject_rate.load(std::memory_order_relaxed);
  if (rate <= 0) return false;
  if (rate >= 100) return true;
  return static_cast<int>(FastRand() % 100U) < rate;
}

void Server::Impl::HandleRequestInWorker(std::shared_ptr<net::Connection> conn,
                                        protocol::Frame frame, bool one_way, bool keep_alive,
                                        std::uint64_t recv_us) {
  // 运行在业务线程：绝不碰 Connection 的非线程安全成员，也绝不碰任何 loop 的连接表。
  //
  // ★ 三个时间戳的分工（**互不重叠、近似可加**，别混用 —— 混用就是看板系统性说谎）：
  //   recv_us  = 收帧时刻（loop 线程取，一路传进来）
  //   begin_us = 本函数开始执行的时刻
  //     queue_wait_us        = begin_us - recv_us     纯排队
  //     worker_busy_us_total = handler 返回 - 进入      纯业务
  //     request_latency_us   = 响应入发送缓冲 - recv_us 端到端（不含内核真正发出的时间）
  //
  // 以前 waited_us 是在 handler **返回之后**才用 NowMs() - start_ms 算的，等于把业务执行
  // 时间算成排队时间；而 queue_wait 是自适应拒绝（AdjustRejectRate）的**唯一输入**，
  // 于是任何一个超过 fast_reject_threshold_ms（默认 20ms）的方法都会把全进程 reject_rate
  // 一路推到 99% —— 不分方法，等于服务自己 DDoS 自己。所以这里的采样点必须在 handler 之前。
  const std::uint64_t begin_us = TimeUtil::NowUs();
  const std::uint64_t waited_us = begin_us > recv_us ? begin_us - recv_us : 0;

  if (m_queue_wait != nullptr) m_queue_wait->Observe(waited_us);
  win_wait_sum_us.fetch_add(waited_us, std::memory_order_relaxed);
  win_wait_count.fetch_add(1, std::memory_order_relaxed);

  Status status = Status::Ok();
  std::string out_payload;
  const char* full_name = nullptr;

  // ① 排队超时：用"收帧 → 现在"的**真实排队时间**判定（不是端到端）。
  if (waited_us / 1000ULL > static_cast<std::uint64_t>(options.request_timeout_ms)) {
    status = Status(StatusCode::kTimeout);
  } else {
    // ② 路由
    const MethodHandler* handler = dispatcher.Find(frame.header.method_id, &full_name);
    if (handler == nullptr) handler = dispatcher.fallback();
    if (handler == nullptr) {
      status = Status(StatusCode::kNotFound);
    } else {
      ServerContext ctx;
      ctx.seq = frame.header.seq;
      ctx.method_id = frame.header.method_id;
      ctx.method_full_name = full_name;
      ctx.peer_addr = conn->peer_addr();
      ctx.metadata = &frame.metadata;  // frame 在本函数返回前一直存活
      ctx.start_ms = recv_us / 1000ULL;
      const std::uint64_t handler_begin_us = TimeUtil::NowUs();
      status = (*handler)(frame.payload.data(), frame.payload.size(), &out_payload, &ctx);
      if (m_worker_busy != nullptr) m_worker_busy->Inc(TimeUtil::NowUs() - handler_begin_us);
    }
  }
  if (!status.ok() && m_error_total != nullptr) m_error_total->Inc();

  net::EventLoop* loop = conn->loop();
  if (one_way) {
    // 单向调用：服务端不回包，只把在飞计数还回去（keep_alive=false 时同样要收尾断开）。
    loop->RunInLoop([this, conn, keep_alive] {
      conn->DecrementPendingOps();
      global_pending.fetch_sub(1, std::memory_order_relaxed);
      if (!keep_alive) conn->Shutdown();
    });
    return;
  }

  protocol::Frame response;
  response.ResetResponse(frame.header.seq, frame.header.method_id,
                         static_cast<std::uint16_t>(status.code()));
  if (status.ok()) response.payload = std::move(out_payload);
  response.header.flags = WithFlag(response.header.flags, protocol::kFlagKeepAlive, keep_alive);
  const bool draining = stopping.load(std::memory_order_relaxed);
  response.header.flags = WithFlag(response.header.flags, protocol::kFlagDraining, draining);

  loop->RunInLoop([this, conn, response = std::move(response), recv_us, keep_alive]() mutable {
    Buffer out;
    if (protocol::Codec::Encode(&response, &out, options.max_frame_size)) {
      conn->Send(out.Peek(), out.ReadableBytes());
      if (m_response_total != nullptr) m_response_total->Inc();
      // 端到端：收帧 → 响应**进入发送缓冲**。Send 只是入队，所以这里不含内核真正发出的时间；
      // 要含那一段得等写完成回调，而那时调用方可能早已读到响应了，反而失真。
      if (m_latency != nullptr) m_latency->Observe(TimeUtil::NowUs() - recv_us);
      // keep_alive=false：回完这一帧就断。
      // 必须用 Shutdown()（"输出缓冲排空后关闭"）而不是 ForceClose()（会丢掉刚入队的响应）。
      // 以前只清了响应帧的 KeepAlive 位、全文件没有任何断开动作 —— 客户端等 FIN 等到超时，
      // 而非幂等调用拿到 kTimeout 按矩阵不重试，一次本该正常的调用就变成了失败。
      if (!keep_alive) conn->Shutdown();
    } else {
      QZ_LOG_ERROR << "server: failed to encode response for " << conn->peer_addr();
      conn->ForceClose();
    }
    conn->DecrementPendingOps();
    global_pending.fetch_sub(1, std::memory_order_relaxed);
  });
}

// ---------------------------------------------------------------------------
// 后台维护
// ---------------------------------------------------------------------------

void Server::Impl::HousekeepingLoop() {
  std::unique_lock<std::mutex> lock(housekeeping_mutex);
  while (!stopping.load(std::memory_order_acquire)) {
    // 用 wait_for 而不是 sleep：停机时必须能被立刻叫醒（doc 02 对周期任务的要求）。
    housekeeping_cv.wait_for(lock, std::chrono::milliseconds(500));
    if (stopping.load(std::memory_order_acquire)) break;
    lock.unlock();
    AdjustRejectRate();
    ReapIdleConnections();
    ResumePausedReads();
    lock.lock();
  }
}

// ---------------------------------------------------------------------------
// 自注册（服务发现的生产侧）
// ---------------------------------------------------------------------------
//
// 为什么必须有这一段：`RegistryServer` 提供了 Register/Heartbeat/Fetch/Deregister 四个方法，
// 而 `RegistryClient` 只会 Fetch。如果没有任何进程调用 Register，注册表就永远是空的 ——
// 文档里写着"服务发现"，代码里却只有查询侧、没有生产侧，真实部署里等于不存在。
//
// 线程模型与失败策略：
//   * **独立线程**（不并进 housekeeping）：注册必须在 Start() 之后（`--port 0` 时只有
//     那时才知道真实端口），而且这是网络调用（最长 1s），塞进 housekeeping 会拖住
//     空闲回收与过载调节；
//   * **注册不上不影响本机提供服务**：只打 WARN 并按固定间隔重试。注册中心比我们起得晚
//     是常态（演示脚本就是先起 server 再起 registry），不能因此让服务起不来；
//   * Stop() 时发一次 Deregister（尽力而为，失败只打 WARN），失败也有注册中心的 TTL 兜底；
//   * 用 `rpc::Channel` 而不是手写 socket：注册中心就是一个普通的 qzrpc 服务，
//     复用同一套 Codec / 连接池 / 超时，框架自举，不存在"第二套协议"。
void Server::Impl::RegistryLoop() {
  using namespace qzrpc::registry;  // 仅本函数内：避免给整个 server.cpp 引入别名

  net::Endpoint registry_ep;
  if (!ParseIpPort(options.registry_endpoint, &registry_ep)) {
    QZ_LOG_ERROR << "server: invalid registry_endpoint '" << options.registry_endpoint
                 << "', self-registration disabled";
    return;
  }

  // 对外宣告的地址：bind_ip 是通配地址时客户端连不上它，退化成回环地址并明确告警。
  // 跨机部署必须显式配置 bind_ip（或 --bind 指定本机内网地址）。
  std::string advertise_host = options.bind_ip;
  if (advertise_host.empty() || advertise_host == "0.0.0.0" || advertise_host == "::") {
    advertise_host = "127.0.0.1";
    QZ_LOG_WARN << "server: bind_ip is a wildcard ('" << options.bind_ip
                << "'), registering with 127.0.0.1 — set an explicit bind_ip for cross-host access";
  }
  const std::uint16_t advertise_port = listen_port.load(std::memory_order_acquire);
  if (advertise_port == 0) {
    QZ_LOG_ERROR << "server: listen port unknown, self-registration disabled";
    return;
  }

  rpc::ClientOptions client_options;
  client_options.service_name = kRegistryServiceFullName;
  client_options.caller_name = options.service_name;  // 注册中心日志里能看出是谁在注册
  client_options.endpoints.push_back(registry_ep);
  client_options.connections_per_endpoint = 1;
  client_options.connect_timeout_ms = 500;
  client_options.request_timeout_ms = kRegistryRequestTimeoutMs;
  client_options.acquire_timeout_ms = 200;
  client_options.max_retries = 0;  // 幂等小请求，失败下一轮重来即可

  std::string cfg_err;
  if (!client_options.Validate(&cfg_err)) {
    QZ_LOG_ERROR << "server: registry client options invalid: " << cfg_err
                 << ", self-registration disabled";
    return;
  }

  // Channel 放在栈上：本线程退出时析构会 Stop()（join 它自己的 IO 线程），
  // 因此 Impl 不需要持有它，也就不存在"线程还在跑但 Channel 已被销毁"的窗口。
  rpc::Channel channel(client_options);
  std::string start_err;
  if (!channel.Start(&start_err)) {
    QZ_LOG_ERROR << "server: registry channel start failed (" << start_err
                 << "), self-registration disabled";
    return;
  }

  CallOptions call_options;
  call_options.timeout_ms = kRegistryRequestTimeoutMs;
  call_options.server_timeout_ms = static_cast<std::uint32_t>(kRegistryRequestTimeoutMs / 2);

  const int heartbeat_ms = std::max(100, std::min(options.registry_heartbeat_interval_ms, 60000));
  const std::uint32_t lease_ttl_ms = kRegistryLeaseTtlMs;

  bool registered = false;
  std::string instance_id;
  std::uint64_t next_heartbeat_ms = 0;

  while (!stopping.load(std::memory_order_acquire)) {
    const std::uint64_t now = TimeUtil::NowMs();
    if (!registered) {
      RegisterRequest request;
      ServiceInstance* instance = request.mutable_instance();
      instance->set_service_name(options.service_name);
      instance->set_host(advertise_host);
      instance->set_port(static_cast<std::uint32_t>(advertise_port));
      instance->set_weight(1);
      instance->set_ttl_ms(lease_ttl_ms);
      request.set_lease_ttl_ms(lease_ttl_ms);

      RegisterResponse response;
      const Status status =
          channel.Call(MethodIdRegister(), request, &response, call_options);
      if (status.ok() && response.ok()) {
        registered = true;
        registry_registered.store(true, std::memory_order_release);
        // 注册中心可能分配 instance_id（请求里没带时用 "host:port"），必须用它发心跳。
        instance_id = response.assigned_instance_id().empty()
                          ? (advertise_host + ":" + std::to_string(advertise_port))
                          : response.assigned_instance_id();
        next_heartbeat_ms = now + static_cast<std::uint64_t>(heartbeat_ms);
        if (m_registry_register != nullptr) m_registry_register->Inc();
        QZ_LOG_INFO << "server: registered service '" << options.service_name << "' as "
                    << instance_id << " (heartbeat " << heartbeat_ms << "ms, ttl " << lease_ttl_ms
                    << "ms)";
      } else {
        if (m_registry_errors != nullptr) m_registry_errors->Inc();
        QZ_LOG_WARN << "server: register to " << options.registry_endpoint
                    << " failed (retry in " << kRegistryRetryIntervalMs << "ms): "
                    << (status.ok() ? response.error() : status.ToString());
        std::unique_lock<std::mutex> lock(registry_mutex);
        registry_cv.wait_for(lock, std::chrono::milliseconds(kRegistryRetryIntervalMs),
                             [this] { return stopping.load(std::memory_order_acquire); });
        continue;
      }
    } else if (now >= next_heartbeat_ms) {
      HeartbeatRequest request;
      request.set_service_name(options.service_name);
      request.set_instance_id(instance_id);

      HeartbeatResponse response;
      const Status status =
          channel.Call(MethodIdHeartbeat(), request, &response, call_options);
      if (status.ok() && response.ok() && !response.not_found()) {
        if (m_registry_heartbeat != nullptr) m_registry_heartbeat->Inc();
        next_heartbeat_ms = now + static_cast<std::uint64_t>(heartbeat_ms);
      } else if (status.ok() && response.not_found()) {
        // 被 TTL 摘掉了（或注册中心重启过）：回到未注册状态重新登记。
        // 不这么做的话我们会一直"自以为活着"，而客户端早已拉不到我们。
        registered = false;
        registry_registered.store(false, std::memory_order_release);
        QZ_LOG_WARN << "server: instance " << instance_id
                    << " is no longer in the registry, re-registering";
      } else {
        if (m_registry_errors != nullptr) m_registry_errors->Inc();
        QZ_LOG_WARN << "server: heartbeat to " << options.registry_endpoint
                    << " failed: " << status.ToString();
        // 别把心跳打成高频重试：失败后退到一个重试间隔再发。
        next_heartbeat_ms = now + static_cast<std::uint64_t>(kRegistryRetryIntervalMs);
      }
    }

    // 100ms 粒度：Stop() 之后本线程最迟 100ms 醒来退出，不拖住 Wait()。
    std::unique_lock<std::mutex> lock(registry_mutex);
    registry_cv.wait_for(lock, std::chrono::milliseconds(kRegistryWaitSliceMs),
                         [this] { return stopping.load(std::memory_order_acquire); });
  }

  if (registered) {
    DeregisterRequest request;
    request.set_service_name(options.service_name);
    request.set_instance_id(instance_id);
    const Status status = channel.Call(MethodIdDeregister(), request, nullptr, call_options);
    if (status.ok()) {
      QZ_LOG_INFO << "server: deregistered " << instance_id;
    } else {
      // 尽力而为：Deregister 失败不影响停机，实例会在 TTL 到期后被清扫掉。
      QZ_LOG_WARN << "server: deregister " << instance_id
                  << " failed (will expire by TTL): " << status.ToString();
    }
  }
  registry_registered.store(false, std::memory_order_release);
}

void Server::Impl::AdjustRejectRate() {
  if (!options.enable_adaptive_reject) {
    reject_rate.store(0, std::memory_order_relaxed);
    return;
  }
  const std::uint64_t count = win_wait_count.exchange(0, std::memory_order_relaxed);
  const std::uint64_t sum = win_wait_sum_us.exchange(0, std::memory_order_relaxed);
  const int step = std::max(1, options.fast_reject_adjust_rate);
  const std::uint64_t threshold_us =
      static_cast<std::uint64_t>(std::max(0, options.fast_reject_threshold_ms)) * 1000ULL;

  int rate = reject_rate.load(std::memory_order_relaxed);
  const int previous = rate;
  if (count == 0) {
    rate = std::max(0, rate - step);  // 没流量 → 缓慢退让
  } else {
    const std::uint64_t mean_us = sum / count;
    if (mean_us > threshold_us) {
      rate = std::min(99, rate + step);
    } else {
      rate = std::max(0, rate - step);
    }
  }
  reject_rate.store(rate, std::memory_order_relaxed);
  if (rate != previous && (rate == 0 || previous == 0 || rate % 10 == 0)) {
    QZ_LOG_INFO << "server: adaptive reject rate " << previous << "% -> " << rate << "%";
  }
}

void Server::Impl::ReapIdleConnections() {
  if (options.idle_timeout_ms <= 0) return;
  const std::uint64_t now = TimeUtil::NowMs();
  const std::uint64_t timeout = static_cast<std::uint64_t>(options.idle_timeout_ms);
  // metrics 端点同样要回收：以前只扫 io_workers，metrics_conns 永不回收 ——
  // "连上、发 1 个字节、不发 \r\n\r\n"的每一条都会常驻（~64KB + 1 fd），且没有上限。
  if (metrics_loop) {
    net::EventLoop* ml = metrics_loop.get();
    ml->RunInLoop([this, now, timeout] {
      for (auto it = metrics_conns.begin(); it != metrics_conns.end();) {
        const std::shared_ptr<net::Connection>& conn = it->second;
        const std::uint64_t last = conn ? conn->last_active_ms() : 0;
        if (!conn || last == 0 || (now > last && now - last > timeout)) {
          if (conn) conn->ForceClose();
          it = metrics_conns.erase(it);
        } else {
          ++it;
        }
      }
    });
  }

  for (auto& worker : io_workers) {
    net::EventLoop* loop = worker->loop.get();
    IoWorker* raw = worker.get();
    loop->RunInLoop([raw, now, timeout] {
      // 运行在本 loop 线程：可以安全遍历连接表。
      std::vector<std::shared_ptr<net::Connection>> victims;
      for (const auto& item : raw->conns) {
        const std::shared_ptr<net::Connection>& conn = item.second;
        if (conn->pending_ops() > 0) continue;  // 有在飞请求就不回收
        const std::uint64_t last = conn->last_active_ms();
        if (last != 0 && now > last && (now - last) > timeout) victims.push_back(conn);
      }
      for (const auto& conn : victims) conn->ForceClose();
    });
  }
}

void Server::Impl::ResumePausedReads() {
  // 队列排空到一半以下才恢复读，避免"恢复 → 立刻又满 → 再暂停"的抖动。
  const std::size_t threshold =
      options.max_pending_tasks == 0 ? 0 : options.max_pending_tasks / 2;
  if (workers && workers->queue_size() > threshold) return;
  for (auto& worker : io_workers) {
    IoWorker* raw = worker.get();
    raw->loop->RunInLoop([raw] {
      for (const auto& item : raw->conns) {
        const std::shared_ptr<net::Connection>& conn = item.second;
        const auto state = std::static_pointer_cast<ConnState>(conn->context());
        if (!state) continue;
        if (state->read_paused.load(std::memory_order_relaxed) &&
            conn->state() == net::Connection::State::kConnected) {
          state->read_paused.store(false, std::memory_order_relaxed);
          conn->SetReadEnabled(true);
        }
      }
    });
  }
}

// ---------------------------------------------------------------------------
// 停机
// ---------------------------------------------------------------------------

void Server::Impl::CloseAllConnections() {
  for (auto& worker : io_workers) {
    IoWorker* raw = worker.get();
    net::EventLoop* loop = worker->loop.get();
    loop->RunInLoop([raw] {
      std::vector<std::shared_ptr<net::Connection>> victims;
      victims.reserve(raw->conns.size());
      for (auto& item : raw->conns) victims.push_back(item.second);
      raw->conns.clear();  // 先清表：close 回调里的身份检查自然失败，计数照常递减
      for (auto& conn : victims) conn->ForceClose();
    });
  }
  if (metrics_loop) {
    net::EventLoop* loop = metrics_loop.get();
    loop->RunInLoop([this] {
      std::vector<std::shared_ptr<net::Connection>> victims;
      victims.reserve(metrics_conns.size());
      for (auto& item : metrics_conns) victims.push_back(item.second);
      metrics_conns.clear();
      for (auto& conn : victims) conn->ForceClose();
    });
  }
}

void Server::Impl::CoordinatorLoop() {
  QZ_LOG_INFO << "server: draining (up to " << options.graceful_shutdown_timeout_ms << "ms)";
  // 步骤 3：等在飞请求排空（步骤 1 的停 accept 与步骤 2 的拒绝新请求已分别在
  // Stop()/HandleMessage 里完成）。
  const std::uint64_t deadline =
      TimeUtil::NowMs() +
      static_cast<std::uint64_t>(std::max(0, options.graceful_shutdown_timeout_ms));
  while (TimeUtil::NowMs() < deadline) {
    if (global_pending.load(std::memory_order_relaxed) <= 0) break;
    TimeUtil::SleepMs(5);
  }
  const std::int64_t leftover = global_pending.load(std::memory_order_relaxed);
  if (leftover > 0) {
    QZ_LOG_WARN << "server: graceful drain timeout with " << leftover
                << " request(s) still in flight, closing anyway";
  }

  // 步骤 4：关连接（先把所有连接关掉，避免 Quit 之后还有半开连接占着 fd）
  CloseAllConnections();

  // 停 housekeeping
  housekeeping_cv.notify_all();

  // 真正关掉监听 fd：Acceptor::Stop() 只把 fd 从 Poller 摘除（那是为了"停止接新连接
  // 但不回 RST"的优雅降级），**不关 fd**。如果一直不关，本进程重启时会因为端口仍被
  // 自己占用而 bind 失败。所以收尾阶段必须在主 loop 线程里析构 Acceptor。
  // （析构走 RunInLoop，由 Loop() 退出前的队列排空保证一定执行。）
  if (main_loop) {
    main_loop->RunInLoop([this] { acceptor.reset(); });
  }
  if (metrics_loop) {
    metrics_loop->RunInLoop([this] { metrics_acceptor.reset(); });
  }

  // Quit 所有 loop：Quit 会唤醒各自的 Loop()
  for (auto& worker : io_workers) {
    if (worker->loop) worker->loop->Quit();
  }
  if (main_loop) main_loop->Quit();
  if (metrics_loop) metrics_loop->Quit();

  // 业务线程池：drain = true，让已经入队的任务跑完（它们会往已关闭的连接上写，
  // Connection::Send 对 kClosed 是安全的空操作）
  if (workers) workers->Stop(true);

  QZ_LOG_INFO << "server: stopped";
  {
    std::lock_guard<std::mutex> lock(finish_mutex);
    finished.store(true, std::memory_order_release);
  }
  finish_cv.notify_all();
  run_cv.notify_all();
}

// 线程全部退出后，在**非 loop 线程**里把连接表清空。
// 此时没有任何线程在跑，所以 ~Connection → Channel::Remove → Poller::Del 不存在并发；
// 顺序很关键：连接表必须在 EventLoop 之前清掉，否则 ~Connection 会回访已析构的 loop_。
void Server::Impl::DropConnectionTables() {
  for (auto& worker : io_workers) {
    if (worker) worker->conns.clear();
  }
  metrics_conns.clear();
}

void Server::Impl::JoinThreads() {
  auto join_one = [](std::thread& thread) {
    if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
  };
  join_one(main_thread);
  for (auto& worker : io_workers) join_one(worker->thread);
  join_one(metrics_thread);
  join_one(housekeeping_thread);
  join_one(coordinator_thread);
  // 自注册线程必须在 Impl 析构前 join：它持有栈上的 Channel，退出时会用 Impl 的
  // stopping / metrics 指针 —— 先 join 再销毁 Impl，顺序反了就是 use-after-free。
  join_one(registry_thread);
}

// ---------------------------------------------------------------------------
// metrics HTTP
// ---------------------------------------------------------------------------

void Server::Impl::MetricsConnectionInLoop(const std::shared_ptr<net::Connection>& conn) {
  const int fd = conn->fd();
  auto request = std::make_shared<std::string>();
  conn->SetMessageCallback([this, request](const std::shared_ptr<net::Connection>& c,
                                           Buffer* input) {
    // ★ 先判大小再 append：单次可读事件最多能灌进 ~1MB（16×64KB），
    //   先 append 再判等于允许"每条连接瞬时 1MB × 无上限连接数"。
    if (request->size() + input->ReadableBytes() > kMaxMetricsHttpRequestBytes) {
      c->ForceClose();
      return;
    }
    request->append(input->PeekView());
    input->RetrieveAll();
    if (request->find("\r\n\r\n") == std::string::npos &&
        request->find("\n\n") == std::string::npos) {
      return;  // 请求头还没收全
    }
    WriteMetricsResponse(c, *request);
  });
  conn->SetCloseCallback([this, fd](const std::shared_ptr<net::Connection>& c) {
    const auto it = metrics_conns.find(fd);
    if (it != metrics_conns.end() && it->second == c) metrics_conns.erase(it);
  });
  metrics_conns.emplace(fd, conn);
  conn->SetReadEnabled(true);
}

void Server::Impl::WriteMetricsResponse(const std::shared_ptr<net::Connection>& conn,
                                       const std::string& request) {
  // 极简 HTTP：只解析"请求行"，不解析 header、不解析 body。
  // 语义固定为 HTTP/1.0 + Connection: close（读完请求头就回包并关连接），
  // 这样 IO 线程里不需要任何 HTTP 状态机，也就没有"半截请求挂住线程"的风险。
  //
  // 安全说明：本端点**无鉴权**，只应由 metrics_bind_ip 指定的回环/内网地址暴露；
  // 生产环境必须前置网关或反向代理做访问控制，不要直接开在公网上。
  const std::size_t line_end = request.find_first_of("\r\n");
  const std::string request_line = request.substr(0, line_end);
  const std::size_t first_space = request_line.find(' ');
  std::string method = first_space == std::string::npos ? request_line
                                                        : request_line.substr(0, first_space);
  std::string path;
  if (first_space != std::string::npos) {
    const std::size_t second_space = request_line.find(' ', first_space + 1);
    path = request_line.substr(first_space + 1, second_space == std::string::npos
                                                    ? std::string::npos
                                                    : second_space - first_space - 1);
  }
  const std::size_t query = path.find('?');
  if (query != std::string::npos) path.resize(query);

  std::string body;
  int status_code = 200;
  std::string status_text = "OK";
  bool no_body = false;
  bool allow_header = false;

  if (method == "OPTIONS") {
    // CORS 预检：直接 204 无正文（某些浏览器会先发预检再发真正的 GET）
    status_code = 204;
    status_text = "No Content";
    no_body = true;
  } else if (method != "GET") {
    status_code = 405;
    status_text = "Method Not Allowed";
    body = "405 method not allowed, only GET /metrics\n";
    allow_header = true;
  } else if (path != "/metrics") {
    status_code = 404;
    status_text = "Not Found";
    body = "404 only GET /metrics is supported\n";
  } else {
    body = MetricsRegistry::Instance().DumpText();
    // 活跃连接数用 gauge 手写：Counter 只能 Inc()，给不出会下降的"当前值"，
    // 硬塞一个只会涨的假活跃数比不报更坏（见文件头决策 3）。
    const std::int64_t active = conn_count.load(std::memory_order_relaxed);
    char line[160];
    std::snprintf(line, sizeof(line),
                  "# HELP qzrpc_server_connection_active Current established connections.\n"
                  "# TYPE qzrpc_server_connection_active gauge\n"
                  "qzrpc_server_connection_active %lld\n",
                  static_cast<long long>(active < 0 ? 0 : active));
    body.append(line);
  }

  // CORS：仅为本地演示放宽（example/web/index.html 是 file:// 页面，fetch 到
  // http://127.0.0.1:<metrics_port> 属于跨源请求）。读接口无凭据、无副作用，
  // 加这个头是安全的；**生产不要照抄**，请用网关按域名白名单收紧。
  std::string head;
  head.reserve(512);
  head += "HTTP/1.0 " + std::to_string(status_code) + " " + status_text + "\r\n";
  head += "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
  head += "Access-Control-Allow-Origin: *\r\n";
  head += "Access-Control-Allow-Methods: GET, OPTIONS\r\n";
  if (allow_header) head += "Allow: GET, OPTIONS\r\n";
  head += "Content-Length: " + std::to_string(no_body ? 0 : body.size()) + "\r\n";
  head += "Connection: close\r\n\r\n";
  if (!no_body) head += body;

  conn->Send(head);
  conn->Shutdown();  // 写完即关（HTTP/1.0 + Connection: close 语义）
}

// ---------------------------------------------------------------------------
// Server 门面
// ---------------------------------------------------------------------------

Server::Server() : Server(ServerOptions()) {}

Server::Server(const ServerOptions& options) : impl_(new Impl(options)) {
  impl_->ValidateOptions();
}

Server::~Server() {
  if (impl_) {
    impl_->RequestStop();
    impl_->WaitStop(-1);
  }
}

bool Server::valid() const noexcept { return impl_ && impl_->valid; }

const std::string& Server::error_message() const noexcept {
  static const std::string kEmpty;
  return impl_ ? impl_->error_message : kEmpty;
}

const ServerOptions& Server::options() const noexcept { return impl_->options; }

Dispatcher* Server::mutable_dispatcher() { return impl_ ? &impl_->dispatcher : nullptr; }

bool Server::Start(std::string* err) { return impl_ ? impl_->StartAll(err) : false; }

void Server::Run() {
  if (!impl_) return;
  std::unique_lock<std::mutex> lock(impl_->run_mutex);
  // 只做 wait，不做轮询：Stop() 会通知；也已经停完（或从未启动）就直接返回。
  impl_->run_cv.wait(lock, [this] {
    return impl_->stopping.load(std::memory_order_acquire) || impl_->finished.load();
  });
}

void Server::Impl::RequestStop() {
  bool expected = false;
  if (!stopping.compare_exchange_strong(expected, true)) return;  // 幂等
  QZ_LOG_INFO << "server: stop requested";

  // 步骤 1：停止 accept（在主 loop 线程执行，只摘事件不关 fd）
  if (main_loop) {
    Impl* self = this;
    main_loop->RunInLoop([self] {
      if (self->acceptor) self->acceptor->Stop();
    });
  }
  // 唤醒 Run()
  run_cv.notify_all();
  // 唤醒自注册线程（它在 cv 上按 100ms 粒度等），让它尽快走完 Deregister 并退出。
  registry_cv.notify_all();

  if (!started.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lock(finish_mutex);
      finished.store(true, std::memory_order_release);
    }
    finish_cv.notify_all();
    return;
  }

  // 步骤 2~4 交给协调线程：Stop() 本身不阻塞（server.h 的明确要求）
  coordinator_thread = std::thread([this] { CoordinatorLoop(); });
}

void Server::Stop() {
  if (impl_) impl_->RequestStop();
}

bool Server::Impl::WaitStop(int timeout_ms) {
  {
    std::unique_lock<std::mutex> lock(finish_mutex);
    if (!finished.load(std::memory_order_acquire)) {
      if (timeout_ms < 0) {
        finish_cv.wait(lock, [this] { return finished.load(); });
      } else {
        finish_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return finished.load(); });
      }
    }
    if (!finished.load(std::memory_order_acquire)) return false;
  }
  JoinThreads();
  DropConnectionTables();
  started.store(false, std::memory_order_release);
  return true;
}

bool Server::Wait(int timeout_ms) { return impl_ ? impl_->WaitStop(timeout_ms) : true; }

bool Server::running() const noexcept {
  return impl_ && impl_->started.load(std::memory_order_acquire) &&
         !impl_->finished.load(std::memory_order_acquire);
}

std::uint16_t Server::port() const noexcept {
  return impl_ ? impl_->listen_port.load(std::memory_order_acquire) : 0;
}

std::size_t Server::connection_count() const noexcept {
  if (!impl_) return 0;
  const std::int64_t count = impl_->conn_count.load(std::memory_order_relaxed);
  return count < 0 ? 0 : static_cast<std::size_t>(count);
}

void Server::RegisterDefaultMetrics() {
  // 幂等：GetCounter/GetHistogram 已存在时直接返回原指针。
  MetricsRegistry& registry = MetricsRegistry::Instance();
  registry.GetCounter(kMetricConnTotal);
  registry.GetCounter(kMetricRequestTotal);
  registry.GetCounter(kMetricResponseTotal);
  registry.GetCounter(kMetricRejectedTotal);
  registry.GetCounter(kMetricErrorTotal);
  registry.GetCounter(kMetricWorkerBusyUsTotal);
  registry.GetCounter(kMetricRegistryRegisterTotal);
  registry.GetCounter(kMetricRegistryHeartbeatTotal);
  registry.GetCounter(kMetricRegistryErrorsTotal);
  registry.GetCounter(kMetricMetricsConnRejectedTotal);
  registry.GetHistogram(kMetricLatencyUs);
  registry.GetHistogram(kMetricQueueWaitUs);
}

}  // namespace rpc
}  // namespace qzrpc
