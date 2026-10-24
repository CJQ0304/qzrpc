// 轻舟 RPC (Qingzhou RPC)
// src/client/channel.cpp
//
// 职责：把「负载均衡 + 连接池 + 注册发现 + 重试 + 超时 + metrics」组装成一个
//       对业务透明的调用入口。生成的 Stub 只持有 Channel&，不认识下面任何一层。
//
// ★ 四个必须小心的地方：
//   1) **重试矩阵**（团队裁决）。重试只允许用在"确定请求没发出去"或"服务端明确说
//      没执行"的场景。已经写出字节之后才超时/断连的，只有 idempotent == true 才重试。
//      判据写在 RetryableBeforeSend / RetryableAfterSend 两个函数里，全文只此一处，
//      改策略只改那里 —— 散落的 `if (can_retry)` 是这类系统最常见的长期缺陷。
//   2) **响应乱序**。同一个 Channel 上的 32 个并发调用，回包顺序与请求顺序无关
//      （服务端 worker 池并发执行，谁先算完谁先回）。配对完全由 ClientConnection
//      内部的 seq 表完成，Channel 层不得假设"第 i 个响应对应第 i 个请求"。
//   3) **RAII Lease**。连接从池里借出后，任何一条 return 路径都必须归还或失效。
//      手写 pool->Release() 在这样的函数里（有十来个出口）必然漏掉某一条，
//      所以这里用栈上对象管；析构按 conn 的健康度自动选择 Release / Invalidate。
//   4) **启动/停止的幂等与顺序**。Start 幂等；Stop 是"终点"（不可重启）。
//      停止顺序：注册中心 → 连接池 → 排空 loop 任务 → Quit → join。
//      顺序错了会出现"连接池析构了但 loop 线程还在跑回调"的 use-after-free。

#include "client/channel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "client/connection_pool.h"
#include "client/registry_client.h"
#include "common/logger.h"
#include "common/metrics.h"
#include "common/time_util.h"
#include "net/event_loop.h"
#include "protocol/frame.h"
#include "protocol/protocol_const.h"
#include "serialize/message.h"

namespace qzrpc {
namespace rpc {

namespace {

// 一致性哈希的默认虚拟节点数（与 load_balancer.h 的默认参数保持一致）。
constexpr std::size_t kDefaultVirtualNodes = 160;

// 单次退避上限（毫秒）。退避本身也会吃掉调用的总预算，睡过头等于把重试机会扔掉。
constexpr std::uint64_t kMaxBackoffMs = 1000;

// 停机时等待 loop 排空残留任务的兜底上限（毫秒）。
// 不是"正常耗时"：正常情况下毫秒级就返回，这个上限只防"回调死循环"把停机卡死。
constexpr int kDrainTimeoutMs = 2000;

bool SameEndpoint(const net::Endpoint& a, const net::Endpoint& b) noexcept {
  return a.port == b.port && a.ip == b.ip;
}

void SetErr(std::string* err, std::string msg) {
  if (err != nullptr) *err = std::move(msg);
}

// 生成 16 位十六进制 trace_id。
// 不引入第三方 uuid 库；用「单调微秒 ^ 进程内自增序号 ^ 墙上毫秒」过一轮 splitmix64，
// 同一进程内绝不重复，跨进程碰撞概率可忽略（trace_id 只用于排查，不需要密码学强度）。
std::string MakeTraceId() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t x = TimeUtil::NowUs() ^ (n * 0x9E3779B97F4A7C15ULL);
  x ^= TimeUtil::UnixMs() * 0xC2B2AE3D27D4EB4FULL;
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  x ^= (x >> 31);

  static const char kHex[] = "0123456789abcdef";
  char buf[16];
  for (int i = 0; i < 16; ++i) {
    buf[i] = kHex[(x >> ((15 - i) * 4)) & 0x0F];
  }
  return std::string(buf, sizeof(buf));
}

// 请求字节**尚未**写出的失败：重试绝对安全，不要求 idempotent。
bool RetryableBeforeSend(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kServiceUnavailable:  // 借连接失败 / 连不上目标
    case StatusCode::kTimeout:             // 建连超时，一个字节都没发
    case StatusCode::kCancelled:           // 借连接期间 Channel 被停机
      return true;
    default:
      return false;
  }
}

// 请求字节可能已经被对端看到的失败：只有幂等调用才允许重试。
bool RetryableAfterSend(StatusCode code, bool idempotent) noexcept {
  switch (code) {
    case StatusCode::kOverload:
      // 服务端队列满 / 自适应限流时**明确拒绝**，业务逻辑没被执行过 → 非幂等也能重试。
      return true;
    case StatusCode::kServiceUnavailable:
      // 走到这里说明拿到过一帧响应（连不上在 before-send 分支就返回了），
      // 即服务端自己声明"我这边不收"，同样没有执行 → 可以重试。
      return true;
    case StatusCode::kTimeout:
    case StatusCode::kCancelled:
      // 超时 / 连接中途断开：服务端可能已经执行完只是回包丢了。**只允许幂等重试。**
      return idempotent;
    default:
      // 业务错误（kInvalidArgument / kNotFound / kSerializeError / kInternal）重试无意义。
      return false;
  }
}

// 退避睡眠：第 n 次重试等 n * base；kOverload 再翻倍（避免打垮刚缓过来的服务）。
// 所有睡眠都被 deadline 截断 —— 睡过头就是白白把预算耗光。
void SleepBeforeRetry(int attempt, int base_ms, bool doubled, std::uint64_t deadline_ms) {
  if (base_ms <= 0) return;
  std::uint64_t wait = static_cast<std::uint64_t>(base_ms) * static_cast<std::uint64_t>(attempt + 1);
  if (doubled) wait *= 2;
  if (wait > kMaxBackoffMs) wait = kMaxBackoffMs;

  const std::uint64_t now = TimeUtil::NowMs();
  if (now >= deadline_ms) return;
  const std::uint64_t left = deadline_ms - now;
  if (wait > left) wait = left;
  TimeUtil::SleepMs(static_cast<std::uint32_t>(wait));
}

// ---- 客户端标准指标 ----
// 一次性注册并缓存指针（MetricsRegistry 返回的指针生命周期同进程，可在热路径直接用）。
// 名字按团队裁决用下划线风格：点分名只是内部注册键，导出时也会被清洗成下划线，
// 面板里看到的名字必须和这里一致，否则"代码里搜得到、面板上查不到"。
struct ClientMetrics {
  Counter* request_total{nullptr};
  Counter* response_total{nullptr};
  Counter* retry_total{nullptr};
  Counter* failover_total{nullptr};
  Histogram* latency_us{nullptr};
  Histogram* pool_acquire_us{nullptr};
};

const ClientMetrics& EnsureMetrics() {
  static const ClientMetrics metrics = [] {
    ClientMetrics m;
    MetricsRegistry& reg = MetricsRegistry::Instance();
    m.request_total = reg.GetCounter("qzrpc_client_request_total");
    m.response_total = reg.GetCounter("qzrpc_client_response_total");
    m.retry_total = reg.GetCounter("qzrpc_client_retry_total");
    // 与 retry_total 分开计：故障转移（一个字节都没发出去）和业务重试（可能已执行）
    // 是两种完全不同的风险，混成一个计数器会让"成功率一半"变得无法归因。
    m.failover_total = reg.GetCounter("qzrpc_client_failover_total");
    m.latency_us = reg.GetHistogram("qzrpc_client_latency_us");
    m.pool_acquire_us = reg.GetHistogram("qzrpc_client_pool_acquire_us");
    return m;
  }();
  return metrics;
}

// 按状态码分桶的错误计数。错误码只有 9 个，且这里是冷路径（失败才走），
// 直接用注册表的带 label 名字取指针，不做缓存。
void IncErrorCounter(StatusCode code) {
  const std::string name =
      std::string("qzrpc_client_error_total{code=\"") + StatusCodeName(code) + "\"}";
  MetricsRegistry::Instance().GetCounter(name)->Inc();
}

// 连接的 RAII 借用凭证。
// 析构时按健康度决定归还还是失效 —— 这样"失败后忘了 Invalidate"这类泄漏不可能发生。
//
// ★ 为什么持有 shared_ptr<ConnectionPool> 而不是裸指针（团队成员审计出的 UAF）：
//   Channel::Stop() 会 pool->Stop()，把阻塞在 cv 上的调用方唤醒成 kCancelled；
//   这些调用方随即从 Call 返回、Lease 析构，而此刻 Stop() 可能已经走到"销毁池"那一步。
//   裸指针在这里必现 use-after-free，而且窗口正是"停机时还有在飞调用"——
//   pool->Stop() 存在的意义就是支持这个场景，不是边角缺陷。
//   持有 shared_ptr 后，最后一个 Lease 释放时池才真正析构；同时 Channel 侧
//   **不再** pool.reset()（见 Stop 的注释），两边合起来保证池活得比所有借用者长。
class Lease {
 public:
  Lease(std::shared_ptr<ConnectionPool> pool, ClientConnection::Ptr conn)
      : pool_(std::move(pool)), conn_(std::move(conn)) {}

  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

  ~Lease() {
    if (pool_ == nullptr || !conn_) return;
    if (conn_->healthy() && !conn_->closed()) {
      pool_->Release(std::move(conn_));
    } else {
      // 不健康 / 已关闭：关掉且不进空闲队列，下一次 Acquire 会重建。
      pool_->Invalidate(std::move(conn_));
    }
  }

  ClientConnection* operator->() const noexcept { return conn_.get(); }
  ClientConnection& operator*() const noexcept { return *conn_; }

 private:
  std::shared_ptr<ConnectionPool> pool_;
  ClientConnection::Ptr conn_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Channel::Impl {
  explicit Impl(const ClientOptions& opts) : options(opts) {
    valid = options.Validate(&error_message);
    // 默认轮询；ClientOptions 里没有策略字段是刻意的 —— 策略是有状态对象，
    // 用 SetLoadBalancer 注入比从一个字符串再解析回来更直接。
    balancer = CreateLoadBalancer(LoadBalancerType::kRoundRobin, kDefaultVirtualNodes);
  }

  // 静态配置 + 注册中心结果的并集去重（静态在前：兜底地址优先被复用）。
  std::vector<net::Endpoint> MergedEndpoints() const {
    std::vector<net::Endpoint> out = options.endpoints;
    std::lock_guard<std::mutex> lk(registry_mutex);
    for (const net::Endpoint& ep : registry_endpoints) {
      if (std::find_if(out.begin(), out.end(), [&ep](const net::Endpoint& e) {
            return SameEndpoint(e, ep);
          }) == out.end()) {
        out.push_back(ep);
      }
    }
    return out;
  }

  // 注册中心回调（在注册中心的工作线程执行）。
  // 约束：不得阻塞。这里只做一次 vector 拷贝 + 一次环重建。
  void ApplyRegistryUpdate(const std::vector<net::Endpoint>& eps) {
    {
      std::lock_guard<std::mutex> lk(registry_mutex);
      registry_endpoints = eps;
    }
    const std::vector<net::Endpoint> merged = MergedEndpoints();
    balancer->UpdateEndpoints(merged);

    // 刻意**不**在这里对"消失的 endpoint"调用 ConnectionPool::RemoveEndpoint：
    // 那会把正在承载业务的连接直接掐断，把一次正常的缩容变成一批 kCancelled。
    // 正确的收敛方式是"不再被选中 + 空闲超时回收"，见 ConnectionPool::Start 的回收定时器。
    std::string names;
    for (std::size_t i = 0; i < merged.size() && i < 3; ++i) {
      if (!names.empty()) names += ",";
      names += merged[i].ToString();
    }
    QZ_LOG_INFO << "[Channel] endpoints updated: " << merged.size() << " candidate(s): " << names
                << (merged.size() > 3 ? ",..." : "");
  }

  // 配置（构造后只读）。
  ClientOptions options;
  std::string error_message;
  bool valid{false};

  // 生命周期（Start/Stop 与懒启动共用一把锁）。
  std::mutex lifecycle_mutex;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{false};

  // 以下三个成员在 started 之后只读，因此热路径无需加锁。
  std::unique_ptr<net::EventLoop> loop;
  std::thread loop_thread;
  // shared_ptr 而不是 unique_ptr：Lease（借用凭证）要在停机竞态里让池活得比自己久。
  std::shared_ptr<ConnectionPool> pool;
  std::unique_ptr<RegistryClient> registry;

  // 负载均衡器。契约：只能在 Start() 之前替换，替换后生命周期内不再变化，
  // 因此 Select 可以不持锁（多线程并发 Select 由 LoadBalancer 自己保证）。
  std::unique_ptr<LoadBalancer> balancer;

  // 注册中心结果（与 options.endpoints 合并后喂给 balancer）。
  mutable std::mutex registry_mutex;
  std::vector<net::Endpoint> registry_endpoints;

  // 停机时把 loop 队列排空用的一次性屏障。
  void DrainLoopTasks() {
    if (!loop || !loop->running() || loop->IsInLoopThread()) return;
    std::shared_ptr<std::promise<void>> done = std::make_shared<std::promise<void>>();
    std::future<void> fut = done->get_future();
    // 闭包按值持有 shared_ptr：wait 返回后 promise 也必须活着（曾被这个竞态咬过）。
    loop->QueueInLoop([done] {
      if (done) done->set_value();
    });
    fut.wait_for(std::chrono::milliseconds(kDrainTimeoutMs));
  }
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

Channel::Channel(const ClientOptions& options) : impl_(new Impl(options)) {
  if (!impl_->valid) {
    QZ_LOG_ERROR << "[Channel] invalid client options: " << impl_->error_message;
  } else {
    QZ_LOG_INFO << "[Channel] created: " << impl_->options.ToString();
  }
}

Channel::~Channel() { Stop(); }

bool Channel::valid() const noexcept { return impl_->valid; }

const std::string& Channel::error_message() const noexcept { return impl_->error_message; }

const ClientOptions& Channel::options() const noexcept { return impl_->options; }

bool Channel::started() const noexcept { return impl_->started.load(std::memory_order_acquire); }

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool Channel::Start(std::string* err) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.lifecycle_mutex);

  if (im.started.load(std::memory_order_acquire)) return true;  // 幂等
  if (im.stopped.load(std::memory_order_acquire)) {
    SetErr(err, "channel has been stopped and cannot be restarted");
    return false;
  }
  if (!im.valid) {
    SetErr(err, im.error_message);
    return false;
  }

  // 1) 事件循环 + 专属线程。所有连接的 IO 都在这条线程上完成，
  //    业务线程只在 Call 里阻塞等待，不参与 IO。
  im.loop.reset(new net::EventLoop());
  net::EventLoop* loop = im.loop.get();
  im.loop_thread = std::thread([loop] {
    QZ_LOG_INFO << "[Channel] io loop thread started";
    loop->Loop();
    QZ_LOG_INFO << "[Channel] io loop thread exited";
  });

  // 2) 连接池（依赖已存在的 loop）。
  im.pool = std::make_shared<ConnectionPool>(loop, im.options);
  std::string pool_err;
  if (!im.pool->Start(&pool_err)) {
    SetErr(err, "connection pool start failed: " + pool_err);
    loop->Quit();
    if (im.loop_thread.joinable()) im.loop_thread.join();
    im.pool.reset();
    im.loop.reset();
    return false;
  }

  // 3) 先把静态 endpoint 灌进负载均衡器，保证注册中心不可用时也能工作。
  im.balancer->UpdateEndpoints(im.MergedEndpoints());

  // 4) 注册中心（可选）。回调捕获 Impl 裸指针，安全性由 Stop 的顺序保证：
  //    Stop 里先 registry->Stop()（join 拉取线程，此后不会再有回调），才轮到 Impl 析构。
  if (!im.options.registry_endpoint.empty()) {
    im.registry.reset(new RegistryClient(im.options));
    Impl* raw = &im;
    im.registry->SetUpdateCallback([raw](const std::vector<net::Endpoint>& eps) {
      raw->ApplyRegistryUpdate(eps);
    });
    std::string reg_err;
    // 首次拉取失败不算启动失败：静态 endpoint 仍然可用，后台线程会继续重试。
    if (!im.registry->Start(&reg_err)) {
      QZ_LOG_WARN << "[Channel] registry first fetch failed (degraded, will retry): " << reg_err;
    }
  }

  im.started.store(true, std::memory_order_release);
  QZ_LOG_INFO << "[Channel] started, strategy=" << im.balancer->name()
              << ", candidates=" << im.balancer->size();
  return true;
}

void Channel::Stop() {
  Impl& im = *impl_;
  {
    std::lock_guard<std::mutex> lk(im.lifecycle_mutex);
    if (im.stopped.exchange(true, std::memory_order_acq_rel)) return;  // 幂等
  }

  // 顺序不可调换：
  //   a. **先置 started=false**：这是"新调用不得再进来"的栅栏。放在 pool->Stop() 之后
  //      会出现"检查 started 已通过、正准备借连接"的线程合法踩到正在被拆的池。
  //   b. 注册中心停 —— 否则它的工作线程还可能在 Impl 上用回调；
  //   c. 连接池停 —— 关闭所有连接，把阻塞在 Call 里的线程唤醒为 kCancelled；
  //   d. 排空 loop 队列 —— ForceClose 是异步投递的，不排空就 Quit 会漏掉关闭回调；
  //   e. 最后 Quit + join。
  im.started.store(false, std::memory_order_release);
  if (im.registry) im.registry->Stop();
  if (im.pool) im.pool->Stop();
  im.DrainLoopTasks();
  if (im.loop) im.loop->Quit();
  if (im.loop_thread.joinable()) im.loop_thread.join();

  // ★ 这里**不** pool.reset()：
  //   被 pool->Stop() 唤醒的调用方此刻可能还没从 Call 返回，它们手里的 Lease 持有
  //   shared_ptr<ConnectionPool>；提前 reset 等于把池从 Channel 手里放掉，
  //   而 Impl 的析构（连同 loop 的销毁）会与"最后一个 Lease 归还"竞争。
  //   让 pool 随 Impl 一起析构最安全：那时 loop 线程已 join，调用方由 shared_ptr 保活。
  im.registry.reset();
  im.loop.reset();
  QZ_LOG_INFO << "[Channel] stopped";
}

// ---------------------------------------------------------------------------
// 调用
// ---------------------------------------------------------------------------

Status Channel::Call(std::uint32_t method_id, const serde::Message& request,
                     serde::Message* response, const CallOptions& opts) {
  Impl& im = *impl_;
  if (!im.valid) return Status::InvalidArgument(im.error_message);

  // 懒启动：Stub 的用法是「构造 Channel → 直接 Call」，让用户为了一行 Start 报错不值得。
  // 显式 Start 仍然是推荐姿势（可以把首次建连/首次服务发现的耗时挪出业务路径）。
  if (!im.started.load(std::memory_order_acquire)) {
    std::string start_err;
    if (!Start(&start_err)) {
      if (im.stopped.load(std::memory_order_acquire)) {
        return Status::Cancelled("channel already stopped");
      }
      return Status::ServiceUnavailable("channel start failed: " + start_err);
    }
  }

  int timeout_ms = opts.timeout_ms > 0 ? opts.timeout_ms : im.options.request_timeout_ms;
  if (timeout_ms <= 0) {
    return Status::InvalidArgument("timeout_ms must be > 0 (got " + std::to_string(timeout_ms) +
                                   ")");
  }
  int max_retries = opts.max_retries >= 0 ? opts.max_retries : im.options.max_retries;
  if (max_retries < 0) max_retries = 0;
  const int backoff_ms = opts.retry_backoff_ms >= 0 ? opts.retry_backoff_ms
                                                    : im.options.retry_backoff_ms;

  // 序列化只做一次：重试必须发出**逐字节相同**的请求体，否则服务端去重/审计都对不上。
  std::string payload;
  if (!request.SerializeToString(&payload)) {
    return Status::SerializeError(std::string("serialize request failed: ") + request.TypeName());
  }

  // metadata 里与"第几次尝试"无关的部分只构造一次。
  protocol::Metadata meta;
  if (!opts.trace_id.empty()) {
    meta.SetTraceId(opts.trace_id);
  } else if (im.options.auto_trace_id) {
    meta.SetTraceId(MakeTraceId());
  }
  if (!im.options.caller_name.empty()) meta.SetCaller(im.options.caller_name);
  meta.SetCompression(0);  // v1 唯一合法值
  meta.SetIdempotent(opts.idempotent);

  const bool metrics_on = im.options.enable_metrics;
  const ClientMetrics& metrics = EnsureMetrics();
  const std::uint64_t t0_us = TimeUtil::NowUs();
  const std::uint64_t start_ms = TimeUtil::NowMs();
  const std::uint64_t deadline_ms = start_ms + static_cast<std::uint64_t>(timeout_ms);

  Status last = Status::Internal("no attempt was made");
  int attempt = 0;   // 业务重试轮次（吃 max_retries 预算）
  int tries = 0;     // 实际"借到连接并发出请求"的次数（日志与指标用）

  // ---- 故障转移（failover）与业务重试（retry）是两件事 ----
  //   * 故障转移：请求**一个字节都没发出去**（建连失败 / endpoint 冷却中），
  //     换个实例立即再试。它不会产生任何副作用，所以**即使 max_retries == 0 也必须做**
  //     ——这一层不消耗 max_retries 预算。
  //   * 业务重试：请求可能已经被对端看到，只有 before-send / 服务端明确拒绝 /
  //     幂等调用才允许，且受 max_retries 限制。判据在 RetryableBeforeSend / AfterSend。
  //   ！把两者合成一个计数器是常见错误：那样"两个实例挂一个"会被算成业务重试，
  //     用户配 max_retries=0 就只能拿到一半成功率，而监控上看不出原因。
  // 上限 3：候选再多，瓶颈也不在这里；而 3 个候选全连不上时，
  // 最坏耗时 = 3 * connect_timeout（默认 200ms → 600ms），仍在 1s 预算内。
  const int max_candidates =
      std::max(1, std::min<int>(3, static_cast<int>(im.balancer->size())));

  // 本轮内"连接阶段就失败"的 endpoint。跨轮清空：下一轮重试允许重新试探它们
  // （实例重启后需要第二次机会，否则冷却到期也永远试不到）。
  std::vector<std::string> failed_endpoints;
  bool finished = false;  // 结论已定（kOverload / kCancelled），不再尝试

  for (;;) {
    const std::uint64_t now_ms = TimeUtil::NowMs();
    if (now_ms >= deadline_ms) {
      // ★ 不要用一个信息量更低的字符串覆盖上一轮的诊断结果：
      //   排查时最想知道的是"哪个 endpoint 挂住了"（"no response within 800ms from
      //   10.0.0.7:16161"），而不是一句通用的 "deadline exhausted"。
      if (last.ok()) {
        last = Status::Timeout("deadline exhausted before attempt " + std::to_string(attempt));
      } else {
        last.set_message(last.message() + "; deadline exhausted before the next attempt");
      }
      break;
    }
    if (attempt > max_retries) break;

    // 本轮候选都用完了 → 按 before-send 矩阵决定要不要再开一轮（这次算业务重试）。
    if (static_cast<int>(failed_endpoints.size()) >= max_candidates) {
      if (!RetryableBeforeSend(last.code()) || attempt >= max_retries) break;
      if (metrics_on && metrics.retry_total != nullptr) metrics.retry_total->Inc();
      SleepBeforeRetry(attempt, backoff_ms, false, deadline_ms);
      ++attempt;
      failed_endpoints.clear();
      continue;
    }

    // ---- 选一个本轮还没试过的 endpoint ----
    // ⚠️ 这里**不能**再额外传一个"每次调用都自增"的偏移进去。
    //    RoundRobin 的取号本身就是自增的（index = ticket + attempt），
    //    再叠加一个自增偏移会让下标每次跳 2 —— 两个实例时永远落回同一个实例，
    //    症状是"故障转移看起来没生效、每两次失败一次"。实测踩过。
    //    正确做法：偏移只放"轮次"，换实例靠下面的 failed_endpoints 去重。
    net::Endpoint ep;
    bool picked = false;
    for (int probe = 0; probe < max_candidates * 2 + 2; ++probe) {
      if (!im.balancer->Select(opts.hash_key, static_cast<std::size_t>(attempt), &ep)) {
        break;  // 候选集为空
      }
      const std::string probe_key = ep.ToString();
      if (std::find(failed_endpoints.begin(), failed_endpoints.end(), probe_key) ==
          failed_endpoints.end()) {
        picked = true;
        break;
      }
    }
    if (!picked) {
      if (failed_endpoints.empty()) {
        // 候选集为空：再重试也不会凭空变出实例，直接给出明确结论。
        last = Status::ServiceUnavailable("no available endpoint for service '" +
                                         im.options.service_name + "'");
      } else if (!RetryableBeforeSend(last.code()) || attempt >= max_retries) {
        // 已试过的候选全在 connect 阶段失败，且没有重试预算。
      } else {
        if (metrics_on && metrics.retry_total != nullptr) metrics.retry_total->Inc();
        SleepBeforeRetry(attempt, backoff_ms, false, deadline_ms);
        ++attempt;
        failed_endpoints.clear();
        continue;
      }
      break;
    }

    // 本次尝试是不是"为了绕过刚失败的实例而换的"（用于 failover 计数）。
    const bool is_failover = !failed_endpoints.empty();
    if (is_failover && metrics_on && metrics.failover_total != nullptr) {
      metrics.failover_total->Inc();
    }
    ++tries;

    // ---- 借连接（before-send：失败一定没发出去）----
    const std::uint64_t before_acquire_ms = TimeUtil::NowMs();
    const int remain_before_acquire =
        before_acquire_ms >= deadline_ms ? 1 : static_cast<int>(deadline_ms - before_acquire_ms);
    int acquire_ms = im.options.acquire_timeout_ms > 0 ? im.options.acquire_timeout_ms
                                                       : remain_before_acquire;
    if (acquire_ms > remain_before_acquire) acquire_ms = remain_before_acquire;
    if (acquire_ms <= 0) acquire_ms = 1;

    const std::uint64_t acquire_start_us = TimeUtil::NowUs();
    std::string acquire_err;
    AcquireFailure acquire_failure = AcquireFailure::kNone;
    ClientConnection::Ptr conn =
        im.pool->Acquire(ep, acquire_ms, &acquire_err, &acquire_failure);
    if (metrics_on && metrics.pool_acquire_us != nullptr) {
      metrics.pool_acquire_us->Observe(TimeUtil::NowUs() - acquire_start_us);
    }
    if (!conn) {
      // 四类失败、四种处置（见 connection_pool.h 的 AcquireFailure 表格）。
      // 这一层分流是"正反馈拥塞"的唯一防线：池满时去重试/换实例只会把争抢放大。
      switch (acquire_failure) {
        case AcquireFailure::kPoolFull:
          // 池饱和：**不重试、不换实例**。上限是本进程配置，换实例没用、重试只会更糟。
          last = Status::Overload("connection pool saturated for " + ep.ToString() +
                                  " (raise connections_per_endpoint): " + acquire_err);
          finished = true;
          break;
        case AcquireFailure::kStopped:
          // 客户端正在停机：重试没有意义。
          last = Status::Cancelled("client is stopping: " + acquire_err);
          finished = true;
          break;
        case AcquireFailure::kNone:
        case AcquireFailure::kConnectFailed:
        case AcquireFailure::kTimeout:
          // 建连阶段失败（含 endpoint 冷却中）：一个字节都没发出去 → 换一个**没试过**的
          // 实例立即再试。不睡、不计 max_retries —— 这是"两个实例挂一个"时吞吐不腰斩、
          // 延迟不出现周期性尖刺的关键动作。
          last =
              acquire_failure == AcquireFailure::kTimeout
                  ? Status::Timeout("connect to " + ep.ToString() + " timed out: " + acquire_err)
                  : Status::ServiceUnavailable("acquire connection to " + ep.ToString() +
                                               " failed: " + acquire_err);
          failed_endpoints.push_back(ep.ToString());
          break;
      }
      if (finished) break;
      continue;
    }
    Lease lease(im.pool, std::move(conn));

    // ---- 发送 + 等待（可能已经发出，属 after-send 区）----
    const std::uint64_t now2 = TimeUtil::NowMs();
    if (now2 >= deadline_ms) {
      last = Status::Timeout("deadline exhausted before send to " + ep.ToString());
      break;
    }
    const int remain_ms = static_cast<int>(deadline_ms - now2);
    if (opts.server_timeout_ms > 0) meta.SetTimeoutMs(opts.server_timeout_ms);
    meta.SetDeadlineUnixMs(TimeUtil::UnixMs() + static_cast<std::uint64_t>(remain_ms));
    meta.SetRetryCount(static_cast<std::uint32_t>(attempt));

    if (metrics_on && metrics.request_total != nullptr) metrics.request_total->Inc();

    std::string resp_payload;
    protocol::Metadata resp_metadata;
    Status status =
        lease->Call(method_id, payload, meta, remain_ms, &resp_payload, &resp_metadata);

    if (status.ok() && response != nullptr && !resp_payload.empty()) {
      // 空 payload 视为"响应体为空"（例如服务端返回空消息），不当成错误。
      if (!response->ParseFromString(resp_payload)) {
        status = Status::SerializeError(std::string("parse response failed: ") +
                                       response->TypeName());
      }
    }

    if (status.ok()) {
      if (metrics_on) {
        if (metrics.response_total != nullptr) metrics.response_total->Inc();
        if (metrics.latency_us != nullptr) metrics.latency_us->Observe(TimeUtil::NowUs() - t0_us);
      }
      return Status::Ok();
    }

    last = status;
    if (!RetryableAfterSend(status.code(), opts.idempotent) || attempt >= max_retries) break;
    if (metrics_on && metrics.retry_total != nullptr) metrics.retry_total->Inc();
    QZ_LOG_DEBUG << "[Channel] retry " << (attempt + 1) << "/" << max_retries << " method=0x"
                 << std::hex << method_id << std::dec << " endpoint=" << ep.ToString()
                 << " last=" << last.ToString();
    SleepBeforeRetry(attempt, backoff_ms, status.code() == StatusCode::kOverload, deadline_ms);
    ++attempt;
    failed_endpoints.clear();
  }  // for (;;)

  if (metrics_on) {
    if (metrics.latency_us != nullptr) metrics.latency_us->Observe(TimeUtil::NowUs() - t0_us);
    IncErrorCounter(last.code());
  }
  if (last.code() == StatusCode::kTimeout) {
    QZ_LOG_WARN << "[Channel] call failed after " << tries << " attempt(s) in " << attempt
                << " retry round(s): " << last.ToString();
  }
  return last;
}

Status Channel::Call(const std::string& method_full_name, const serde::Message& request,
                     serde::Message* response, const CallOptions& opts) {
  return Call(protocol::MethodIdOfName(method_full_name), request, response, opts);
}

// ---------------------------------------------------------------------------
// 配置与观测
// ---------------------------------------------------------------------------

void Channel::SetLoadBalancer(std::unique_ptr<LoadBalancer> balancer) {
  if (!balancer) return;  // 契约：load_balancer() 永远非空
  Impl& im = *impl_;
  if (im.started.load(std::memory_order_acquire)) {
    // 已经开始 Select 了，此时替换会让其它线程读到半个对象（unique_ptr 读写不是原子的）。
    QZ_LOG_ERROR << "[Channel] SetLoadBalancer must be called before Start(); ignored";
    return;
  }
  im.balancer = std::move(balancer);
  im.balancer->UpdateEndpoints(im.MergedEndpoints());
}

LoadBalancer* Channel::load_balancer() const { return impl_->balancer.get(); }

std::vector<net::Endpoint> Channel::endpoints() const { return impl_->MergedEndpoints(); }

bool Channel::RefreshEndpoints(std::string* err) {
  Impl& im = *impl_;
  if (!im.valid) {
    SetErr(err, im.error_message);
    return false;
  }

  std::lock_guard<std::mutex> lk(im.lifecycle_mutex);
  if (im.registry) {
    if (!im.registry->RefreshNow(err)) return false;
    // 注册中心内部也会回调 ApplyRegistryUpdate；这里再显式同步一次，
    // 保证 RefreshEndpoints 返回 true 时 balancer 一定已经看到新列表。
    im.ApplyRegistryUpdate(im.registry->endpoints());
    return true;
  }

  const std::size_t before = im.balancer->size();
  std::vector<net::Endpoint> merged = im.MergedEndpoints();
  im.balancer->UpdateEndpoints(merged);
  QZ_LOG_DEBUG << "[Channel] endpoints refreshed (static only): " << before << " -> "
               << im.balancer->size();
  if (merged.empty()) {
    SetErr(err, "no endpoint configured and no registry_endpoint set");
    return false;
  }
  return true;
}

void Channel::RegisterDefaultMetrics() { (void)EnsureMetrics(); }

}  // namespace rpc
}  // namespace qzrpc
