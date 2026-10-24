// 轻舟 RPC (Qingzhou RPC)
// src/client/registry_client.cpp
//
// 职责：注册中心客户端。周期性拉取实例列表，变化时回调通知上层（Channel 更新负载均衡）。
//
// 三个刻意的设计取舍：
//   1) **拉模式而非推模式**：注册中心不用维护长连接与回调通道，实现量减半；
//      代价是最大 registry_poll_interval_ms 的发现延迟，对"分钟级扩容"完全够用。
//   2) **拉取失败不清空列表**：网络抖动时继续用最后一次成功结果（标记 degraded），
//      避免"注册中心挂了导致全站不可用"的雪崩。这是本文件最重要的一行策略。
//   3) **不抢业务连接池**：这里用一次性的短连接（建连→一收一发→关闭），
//      控制面与数据面彻底隔离，注册中心慢不会拖累业务调用。
//
// ★ 一个容易写错的地方：socket 是**非阻塞**的（ConnectWithTimeout 的契约），
//   所以这个"同步"客户端必须自己带 deadline 轮询。这里用 1ms 睡眠退让而不是忙等，
//   帧很小（<1KB），实际几乎不会命中 EAGAIN 分支。

#include "client/registry_client.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/buffer.h"
#include "common/logger.h"
#include "common/status.h"
#include "common/time_util.h"
#include "net/socket_util.h"
#include "protocol/codec.h"
#include "protocol/frame.h"
#include "protocol/protocol_const.h"
#include "registry/registry_client_proto.h"
#include "serialize/message.h"

namespace qzrpc {
namespace rpc {

namespace {

// 单次读的块大小。FetchResponse 通常几百字节，8KB 足够一次读完多数实例列表。
constexpr std::size_t kFetchReadChunk = 8192;

// Stop() 期间检查 stopping 标志的粒度（毫秒）。用 50ms 而不是让线程睡满 3 秒，
// 否则停机要等一个完整轮询周期。
constexpr int kStopCheckGranularityMs = 50;

// fd 的 RAII 持有者：任何一条 return 路径都不会漏掉 close。
class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) net::SocketUtil::Close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const noexcept { return fd_; }
  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  int fd_{-1};
};

void SetErr(std::string* err, std::string msg) {
  if (err != nullptr) *err = std::move(msg);
}

// 解析 "ip:port" / "[::1]:port"。
bool ParseHostPort(const std::string& text, net::Endpoint* out) {
  if (text.empty() || out == nullptr) return false;
  std::string host;
  std::string port_text;

  if (text[0] == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') {
      return false;
    }
    host = text.substr(1, close - 1);
    port_text = text.substr(close + 2);
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
    host = text.substr(0, colon);
    port_text = text.substr(colon + 1);
  }
  if (host.empty() || port_text.empty()) return false;

  int port = 0;
  for (char ch : port_text) {
    if (ch < '0' || ch > '9') return false;
    port = port * 10 + (ch - '0');
    if (port > 65535) return false;
  }
  if (port <= 0) return false;

  out->ip = host;
  out->port = static_cast<std::uint16_t>(port);
  return true;
}

// 把注册中心返回的实例列表转成可直接连的 Endpoint 列表（host 可能是域名）。
// 刻意在这里做解析：Endpoint 的定义就是"已解析的 IP 字面量"，域名不该往后传，
// 否则每个业务线程都要各自踩一次 DNS。
std::vector<net::Endpoint> InstancesToEndpoints(const registry::FetchResponse& response) {
  std::vector<net::Endpoint> out;
  out.reserve(response.instances_size());
  for (const registry::ServiceInstance& inst : response.instances()) {
    if (inst.host().empty() || inst.port() == 0 || inst.port() > 65535) continue;
    const std::uint16_t port = static_cast<std::uint16_t>(inst.port());

    std::vector<net::Endpoint> resolved;
    if (!net::SocketUtil::ResolveHost(inst.host(), port, &resolved) || resolved.empty()) {
      QZ_LOG_WARN << "[RegistryClient] cannot resolve instance host '" << inst.host() << ":"
                  << port << "', skipped";
      continue;
    }
    // 一个域名解析出多个地址时只取第一个：实例列表本身已经提供了多副本冗余，
    // 在这里再乘一个 DNS 多记录维度只会让"实例数"含义变得模糊。
    const net::Endpoint& ep = resolved.front();
    bool duplicate = false;
    for (const net::Endpoint& existing : out) {
      if (existing.port == ep.port && existing.ip == ep.ip) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) out.push_back(ep);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct RegistryClient::Impl {
  explicit Impl(const ClientOptions& opts) : options(opts) {}
  ~Impl() { StopThread(); }

  // 拉取一次。成功时填充端点列表与版本号。
  bool FetchOnce(std::vector<net::Endpoint>* out, std::uint64_t* version_out,
                 std::string* err);

  // 拉取 + 应用（成功则回调上层，失败则保持旧数据并标记 degraded）。
  bool DoFetch(std::string* err);

  void Apply(const std::vector<net::Endpoint>& eps, std::uint64_t new_version) {
    UpdateCallback cb;
    {
      std::lock_guard<std::mutex> lk(data_mutex);
      endpoints = eps;
      if (new_version != 0) version = new_version;
      last_error.clear();
    }
    degraded.store(false, std::memory_order_relaxed);
    last_success_ms.store(TimeUtil::NowMs(), std::memory_order_relaxed);
    success_total.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(callback_mutex);
      cb = callback;
    }
    if (cb) cb(eps);  // 回调里不得阻塞（契约）；Channel 侧只做一次环重建。
  }

  void WorkerLoop() {
    const int interval =
        options.registry_poll_interval_ms > 0 ? options.registry_poll_interval_ms : 3000;
    while (!stopping.load(std::memory_order_acquire)) {
      for (int slept = 0; slept < interval; slept += kStopCheckGranularityMs) {
        if (stopping.load(std::memory_order_acquire)) break;
        TimeUtil::SleepMs(static_cast<std::uint32_t>(kStopCheckGranularityMs));
      }
      if (stopping.load(std::memory_order_acquire)) break;
      std::string ignore;
      DoFetch(&ignore);
    }
  }

  void StopThread() {
    stopping.store(true, std::memory_order_release);
    if (worker.joinable()) worker.join();
    running.store(false, std::memory_order_release);
  }

  ClientOptions options;

  UpdateCallback callback;
  std::mutex callback_mutex;

  std::thread worker;
  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> degraded{false};
  std::atomic<std::uint64_t> last_success_ms{0};
  std::atomic<std::uint64_t> success_total{0};
  std::atomic<std::uint64_t> failure_total{0};

  mutable std::mutex data_mutex;
  std::vector<net::Endpoint> endpoints;
  std::uint64_t version{0};
  std::string last_error;

  // 串行化"后台线程"与"RefreshNow"的并发拉取：注册中心协议是无状态的，
  // 但两条连接同时拉会让日志与失败计数难以解释，串起来更省心。
  std::mutex fetch_mutex;
};

bool RegistryClient::Impl::FetchOnce(std::vector<net::Endpoint>* out, std::uint64_t* version_out,
                                     std::string* err) {
  if (out == nullptr || version_out == nullptr) return false;
  out->clear();
  *version_out = 0;

  net::Endpoint registry_ep;
  if (!ParseHostPort(options.registry_endpoint, &registry_ep)) {
    SetErr(err, "invalid registry_endpoint '" + options.registry_endpoint + "'");
    return false;
  }

  const int timeout_ms =
      options.registry_request_timeout_ms > 0 ? options.registry_request_timeout_ms : 1000;
  const std::uint64_t deadline = TimeUtil::NowMs() + static_cast<std::uint64_t>(timeout_ms);

  std::string connect_err;
  const int fd = net::SocketUtil::ConnectWithTimeout(registry_ep, timeout_ms, &connect_err);
  if (fd < 0) {
    SetErr(err, "connect to registry " + registry_ep.ToString() + " failed: " + connect_err);
    return false;
  }
  ScopedFd guard(fd);

  // 请求帧。seq 用进程内自增（从 1 开始，0 保留），响应按 seq 校验。
  static std::atomic<std::uint32_t> next_seq{1};
  std::uint32_t seq = next_seq.fetch_add(1, std::memory_order_relaxed);
  if (seq == 0) seq = next_seq.fetch_add(1, std::memory_order_relaxed);

  registry::FetchRequest request;
  request.set_service_name(options.service_name);
  std::string payload;
  if (!request.SerializeToString(&payload)) {
    SetErr(err, "serialize FetchRequest failed");
    return false;
  }

  protocol::Frame frame;
  frame.Reset(seq, registry::MethodIdFetch(), true);
  frame.header.flags =
      static_cast<std::uint8_t>(protocol::kFlagRequest | protocol::kFlagKeepAlive);
  frame.payload = std::move(payload);

  Buffer wire;
  if (!protocol::Codec::Encode(&frame, &wire, options.max_frame_size)) {
    SetErr(err, "encode FetchRequest frame failed");
    return false;
  }

  // ---- 发送（非阻塞 fd，带 deadline 轮询）----
  std::size_t sent = 0;
  while (sent < wire.ReadableBytes()) {
    const long n = net::SocketUtil::Send(fd, wire.Peek() + sent, wire.ReadableBytes() - sent);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0) {
      const int code = net::SocketUtil::LastError();
      if (net::SocketUtil::WouldBlock(code)) {
        if (TimeUtil::NowMs() >= deadline) {
          SetErr(err, "send FetchRequest timed out");
          return false;
        }
        TimeUtil::SleepMs(1);
        continue;
      }
      SetErr(err, "send FetchRequest failed: " + net::SocketUtil::ErrorString(code));
      return false;
    }
    SetErr(err, "send FetchRequest returned 0");
    return false;
  }

  // ---- 接收：攒够整帧再解码 ----
  Buffer input;
  for (;;) {
    protocol::Frame response;
    std::string decode_err;
    const protocol::Codec::Result result =
        protocol::Codec::Decode(&input, &response, options.max_frame_size, &decode_err);
    if (result == protocol::Codec::Result::kOk) {
      if (!response.header.is_response()) {
        SetErr(err, "registry sent a non-response frame");
        return false;
      }
      if (response.header.seq != seq) {
        SetErr(err, "registry response seq mismatch (got " +
                        std::to_string(response.header.seq) + ", want " + std::to_string(seq) + ")");
        return false;
      }
      if (response.header.status != static_cast<std::uint16_t>(StatusCode::kOk)) {
        SetErr(err, std::string("registry returned status ") +
                        StatusCodeName(static_cast<StatusCode>(response.header.status)));
        return false;
      }
      registry::FetchResponse decoded;
      if (!decoded.ParseFromString(response.payload)) {
        SetErr(err, "decode FetchResponse failed");
        return false;
      }
      *out = InstancesToEndpoints(decoded);
      *version_out = decoded.version();
      return true;
    }
    if (result == protocol::Codec::Result::kError) {
      SetErr(err, "decode registry frame failed: " + decode_err);
      return false;
    }

    if (TimeUtil::NowMs() >= deadline) {
      SetErr(err, "wait for FetchResponse timed out after " + std::to_string(timeout_ms) + "ms");
      return false;
    }
    input.EnsureWritable(kFetchReadChunk);
    const long n = net::SocketUtil::Recv(fd, input.WriteBegin(), input.WritableBytes());
    if (n > 0) {
      input.HasWritten(static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      SetErr(err, "registry closed connection before sending a response");
      return false;
    }
    const int code = net::SocketUtil::LastError();
    if (net::SocketUtil::WouldBlock(code)) {
      TimeUtil::SleepMs(1);
      continue;
    }
    SetErr(err, "recv FetchResponse failed: " + net::SocketUtil::ErrorString(code));
    return false;
  }
}

bool RegistryClient::Impl::DoFetch(std::string* err) {
  std::vector<net::Endpoint> eps;
  std::uint64_t fetched_version = 0;
  std::string local_err;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lk(fetch_mutex);
    ok = FetchOnce(&eps, &fetched_version, &local_err);
    if (ok) {
      // 版本号未变且已有数据 → 不必再打扰上层（少一次环重建）。
      std::lock_guard<std::mutex> dlk(data_mutex);
      if (fetched_version != 0 && fetched_version == this->version && !endpoints.empty() &&
          endpoints.size() == eps.size()) {
        last_success_ms.store(TimeUtil::NowMs(), std::memory_order_relaxed);
        success_total.fetch_add(1, std::memory_order_relaxed);
        degraded.store(false, std::memory_order_relaxed);
        return true;
      }
    }
  }

  if (ok) {
    Apply(eps, fetched_version);
    if (!eps.empty()) {
      std::string names;
      for (std::size_t i = 0; i < eps.size() && i < 3; ++i) {
        if (!names.empty()) names += ",";
        names += eps[i].ToString();
      }
      QZ_LOG_INFO << "[RegistryClient] service '" << options.service_name << "' has " << eps.size()
                  << " instance(s): " << names << (eps.size() > 3 ? ",..." : "")
                  << " (version " << fetched_version << ")";
    } else {
      QZ_LOG_WARN << "[RegistryClient] service '" << options.service_name
                  << "' has no live instance";
    }
    return true;
  }

  // 失败：**保留旧列表**，只标记降级。
  std::size_t kept = 0;
  {
    std::lock_guard<std::mutex> lk(data_mutex);
    last_error = local_err;
    kept = endpoints.size();
  }
  degraded.store(true, std::memory_order_relaxed);
  failure_total.fetch_add(1, std::memory_order_relaxed);
  QZ_LOG_WARN << "[RegistryClient] fetch failed (keeping last known " << kept
              << " instance(s)): " << local_err;
  SetErr(err, local_err);
  return false;
}

// ---------------------------------------------------------------------------
// RegistryClient
// ---------------------------------------------------------------------------

RegistryClient::RegistryClient(const ClientOptions& options) : impl_(new Impl(options)) {}

RegistryClient::~RegistryClient() { Stop(); }

void RegistryClient::SetUpdateCallback(UpdateCallback cb) {
  std::lock_guard<std::mutex> lk(impl_->callback_mutex);
  impl_->callback = std::move(cb);
}

bool RegistryClient::Start(std::string* err) {
  if (impl_->running.load(std::memory_order_acquire)) return true;
  if (impl_->options.registry_endpoint.empty()) {
    SetErr(err, "registry_endpoint is empty");
    return false;
  }

  impl_->stopping.store(false, std::memory_order_release);
  impl_->running.store(true, std::memory_order_release);

  // 先同步拉一次：Start 返回 true 就代表"已经有可用列表"，调用方不必等第一个轮询周期。
  std::string first_err;
  const bool first_ok = impl_->DoFetch(&first_err);
  if (!first_ok) SetErr(err, first_err);

  impl_->worker = std::thread([impl = impl_.get()] { impl->WorkerLoop(); });
  return first_ok;
}

void RegistryClient::Stop() { impl_->StopThread(); }

bool RegistryClient::RefreshNow(std::string* err) {
  if (impl_->options.registry_endpoint.empty()) {
    SetErr(err, "registry_endpoint is empty");
    return false;
  }
  return impl_->DoFetch(err);
}

std::vector<net::Endpoint> RegistryClient::endpoints() const {
  std::lock_guard<std::mutex> lk(impl_->data_mutex);
  return impl_->endpoints;
}

bool RegistryClient::degraded() const noexcept {
  return impl_->degraded.load(std::memory_order_relaxed);
}

bool RegistryClient::running() const noexcept {
  return impl_->running.load(std::memory_order_acquire);
}

std::uint64_t RegistryClient::last_success_ms() const noexcept {
  return impl_->last_success_ms.load(std::memory_order_relaxed);
}

std::uint64_t RegistryClient::fetch_success_total() const noexcept {
  return impl_->success_total.load(std::memory_order_relaxed);
}

std::uint64_t RegistryClient::fetch_failure_total() const noexcept {
  return impl_->failure_total.load(std::memory_order_relaxed);
}

}  // namespace rpc
}  // namespace qzrpc
