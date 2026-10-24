// 轻舟 RPC (Qingzhou RPC)
// src/registry/registry_server.cpp
//
// 职责：内置的极简服务注册中心（Register / Heartbeat / Fetch / Deregister + TTL 清扫）。
//       它本身就是一个 qzrpc::rpc::Server —— 框架跑不通，注册中心就跑不通，
//       这是"自举"最强的可用性证据。
//
// 一致性模型：**最终一致 + 客户端拉取**。
//   * 实例注册后最多 registry_poll_interval_ms 才被消费者看到；
//   * 实例宕机后最多 instance_ttl_ms 才被摘除。
//   RPC 场景下这个延迟完全可接受，换来的是不到 300 行的实现。
//
// ★ 四个设计决定：
//   1) **instance_id 缺省用 "host:port"**。不用随机 id：随机 id 会让"进程重启后重新注册"
//      在列表里留下两条同地址记录（老的那条要等 TTL 才消失），消费端看到重复实例。
//      用地址当身份，重启即覆盖，语义最干净。显式传了 instance_id 就尊重它。
//   2) **心跳只续期，不新增**。心跳带的是 (service, instance_id)，找不到就回 not_found
//      让客户端重新 Register —— 而不是悄悄替它注册，那样实现不了 TTL 摘除。
//   3) **版本号单调递增**。任何变更都 +1，客户端可据此跳过无变化的环重建（省 CPU 也省日志）。
//   4) **清扫线程独立于 Server 的 housekeeping**。TTL 是注册中心自己的语义，
//      不该让业务框架的定时器去理解它；代价是多一个线程，收益是两者可以分别调优。
//
// 线程安全：Start/Run/Stop/ListInstances/ListServices/计数 全部线程安全；
//           4 个 handler 在 Server 的 worker 线程里并发执行，靠一把 mutex 串行化（见注释）。

#include "registry/registry_server.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/metrics.h"
#include "common/status.h"
#include "common/time_util.h"
#include "protocol/protocol_const.h"
#include "serialize/message.h"
#include "server/dispatcher.h"
#include "server/server.h"

namespace qzrpc {
namespace registry {

namespace {

// 默认租约（毫秒）。15 秒 = 5 个默认心跳周期（客户端 3 秒拉一次，心跳同频），
// 能容忍连续 4 次心跳丢失而不被误摘 —— 对"偶发 GC/网络抖动"足够宽容。
constexpr int kDefaultTtlMs = 15000;

// 清扫周期上限。TTL 很小时清扫也不该比 200ms 更密（纯浪费 CPU）。
constexpr int kMinSweepIntervalMs = 200;

void SetErr(std::string* err, std::string msg) {
  if (err != nullptr) *err = std::move(msg);
}

}  // namespace

// ---------------------------------------------------------------------------
// RegistryServerOptions
// ---------------------------------------------------------------------------

RegistryServerOptions::RegistryServerOptions() {
  // 注册中心默认监听 16162（业务服务默认 16161），避免和业务服务抢端口。
  server.port = protocol::kDefaultRegistryPort;
  server.service_name = kRegistryServiceFullName;
  server.worker_thread_count = 4;  // 注册中心是控制面，不需要 16 个 worker
  instance_ttl_ms = kDefaultTtlMs;
  sweep_interval_ms = 3000;
  allow_overwrite = true;
}

bool RegistryServerOptions::Validate(std::string* err) const {
  if (!server.Validate(err)) return false;
  if (instance_ttl_ms <= 0) {
    SetErr(err, "instance_ttl_ms must be > 0");
    return false;
  }
  if (sweep_interval_ms <= 0) {
    SetErr(err, "sweep_interval_ms must be > 0");
    return false;
  }
  if (sweep_interval_ms > instance_ttl_ms) {
    // 不是错误，但会让"宕机实例"最长被多留一个清扫周期，值得提醒。
    QZ_LOG_WARN << "registry: sweep_interval_ms (" << sweep_interval_ms
                << ") > instance_ttl_ms (" << instance_ttl_ms
                << "), dead instances may linger longer than expected";
  }
  return true;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct RegistryServer::Impl {
  explicit Impl(const RegistryServerOptions& opts) : options(opts) {
    valid = options.Validate(&error_message);
    if (!valid) return;

    server.reset(new rpc::Server(options.server));
    rpc::Dispatcher* dispatcher = server->mutable_dispatcher();
    if (dispatcher == nullptr) {
      valid = false;
      error_message = "registry: server dispatcher is null";
      return;
    }

    // 4 个方法全部在构造期注册完，运行期路由表只读 —— 热路径不需要任何注册锁。
    // Register 返回 false 只可能是 method_id 哈希碰撞（不同全名算出同一个 id），
    // 那是必须人工介入的严重问题：宁可启动失败，也不要线上随机把请求路由到别的方法。
    const std::string full_name = kRegistryServiceFullName;
    std::string dispatch_err;
    bool ok = dispatcher->Register(MethodIdRegister(), full_name + ".Register",
                                   [this](const char* payload, std::size_t len, std::string* out,
                                          rpc::ServerContext* ctx) {
                                     return HandleRegister(payload, len, out, ctx);
                                   },
                                   &dispatch_err);
    ok = dispatcher->Register(MethodIdHeartbeat(), full_name + ".Heartbeat",
                              [this](const char* payload, std::size_t len, std::string* out,
                                     rpc::ServerContext* ctx) {
                                return HandleHeartbeat(payload, len, out, ctx);
                              },
                              &dispatch_err) &&
         ok;
    ok = dispatcher->Register(MethodIdFetch(), full_name + ".Fetch",
                              [this](const char* payload, std::size_t len, std::string* out,
                                     rpc::ServerContext* ctx) {
                                return HandleFetch(payload, len, out, ctx);
                              },
                              &dispatch_err) &&
         ok;
    ok = dispatcher->Register(MethodIdDeregister(), full_name + ".Deregister",
                              [this](const char* payload, std::size_t len, std::string* out,
                                     rpc::ServerContext* ctx) {
                                return HandleDeregister(payload, len, out, ctx);
                              },
                              &dispatch_err) &&
         ok;
    if (!ok) {
      valid = false;
      error_message = dispatch_err.empty() ? "registry: method registration failed"
                                           : dispatch_err;
      return;
    }

    RegisterMetricPointers();
  }

  ~Impl() { StopSweeper(); }

  void RegisterMetricPointers() {
    MetricsRegistry& reg = MetricsRegistry::Instance();
    m_register_total = reg.GetCounter("qzrpc_registry_register_total");
    m_heartbeat_total = reg.GetCounter("qzrpc_registry_heartbeat_total");
    m_fetch_total = reg.GetCounter("qzrpc_registry_fetch_total");
    m_expired_total = reg.GetCounter("qzrpc_registry_expired_total");
  }

  // ---- 数据模型 ----

  struct InstanceRecord {
    ServiceInstance info;             // 回给客户端的实例描述（含 weight / ttl_ms）
    std::uint64_t last_seen_ms{0};    // 最近一次注册或心跳的**单调**时钟
    std::uint64_t ttl_ms{0};
  };

  bool IsAlive(const InstanceRecord& record, std::uint64_t now_ms) const {
    if (record.ttl_ms == 0) return true;  // 0 = 永不过期（显式配置才会出现）
    return now_ms <= record.last_seen_ms || now_ms - record.last_seen_ms <= record.ttl_ms;
  }

  // ---- 4 个 handler ----

  Status HandleRegister(const char* payload, std::size_t len, std::string* out,
                        rpc::ServerContext* ctx) {
    (void)ctx;
    RegisterRequest request;
    if (!request.ParseFromArray(payload, len)) {
      return Status::InvalidArgument("registry: cannot decode RegisterRequest");
    }
    const ServiceInstance& input = request.instance();
    if (input.service_name().empty()) {
      return Status::InvalidArgument("registry: RegisterRequest.instance.service_name is empty");
    }
    if (input.host().empty() || input.port() == 0 || input.port() > 65535) {
      return Status::InvalidArgument("registry: RegisterRequest.instance host/port is invalid");
    }

    const std::uint64_t now = TimeUtil::NowMs();
    const std::uint64_t ttl = request.lease_ttl_ms() > 0
                                  ? static_cast<std::uint64_t>(request.lease_ttl_ms())
                                  : static_cast<std::uint64_t>(options.instance_ttl_ms);

    // 缺省 id = host:port（见文件头决定 1）。
    std::string instance_id = input.instance_id();
    if (instance_id.empty()) {
      instance_id = input.host() + ":" + std::to_string(input.port());
    }

    RegisterResponse response;
    {
      std::lock_guard<std::mutex> lk(mutex);
      auto& instances = services[input.service_name()];
      auto existing = instances.find(instance_id);
      if (existing != instances.end() && !options.allow_overwrite) {
        response.set_ok(false);
        response.set_error("instance '" + instance_id + "' already registered");
      } else {
        InstanceRecord& record = instances[instance_id];
        record.info = input;
        record.info.set_instance_id(instance_id);
        record.info.set_ttl_ms(static_cast<std::uint32_t>(ttl > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : ttl));
        record.last_seen_ms = now;
        record.ttl_ms = ttl;
        ++version;
        response.set_ok(true);
        response.set_assigned_instance_id(instance_id);
      }
    }

    if (m_register_total != nullptr) m_register_total->Inc();
    if (response.ok()) {
      QZ_LOG_INFO << "registry: registered '" << input.service_name() << "' instance "
                  << instance_id << " (" << input.host() << ":" << input.port() << ", ttl "
                  << ttl << "ms)";
    } else {
      QZ_LOG_WARN << "registry: register rejected for '" << input.service_name() << "' instance "
                  << instance_id << ": " << response.error();
    }
    if (out != nullptr) {
      out->clear();
      response.SerializeToString(out);
    }
    return Status::Ok();
  }

  Status HandleHeartbeat(const char* payload, std::size_t len, std::string* out,
                         rpc::ServerContext* ctx) {
    (void)ctx;
    HeartbeatRequest request;
    if (!request.ParseFromArray(payload, len)) {
      return Status::InvalidArgument("registry: cannot decode HeartbeatRequest");
    }
    if (request.service_name().empty() || request.instance_id().empty()) {
      return Status::InvalidArgument(
          "registry: HeartbeatRequest requires service_name and instance_id");
    }

    const std::uint64_t now = TimeUtil::NowMs();
    HeartbeatResponse response;
    {
      std::lock_guard<std::mutex> lk(mutex);
      auto svc = services.find(request.service_name());
      if (svc != services.end()) {
        auto record = svc->second.find(request.instance_id());
        if (record != svc->second.end() && IsAlive(record->second, now)) {
          record->second.last_seen_ms = now;
          response.set_ok(true);
        }
      }
      if (!response.ok()) {
        // 已过 TTL 或从未注册：让客户端重新 Register（而不是替它续命）。
        response.set_ok(false);
        response.set_not_found(true);
      }
    }

    if (m_heartbeat_total != nullptr) m_heartbeat_total->Inc();
    if (out != nullptr) {
      out->clear();
      response.SerializeToString(out);
    }
    return Status::Ok();
  }

  Status HandleFetch(const char* payload, std::size_t len, std::string* out,
                     rpc::ServerContext* ctx) {
    (void)ctx;
    FetchRequest request;
    if (!request.ParseFromArray(payload, len)) {
      return Status::InvalidArgument("registry: cannot decode FetchRequest");
    }

    const std::uint64_t now = TimeUtil::NowMs();
    FetchResponse response;
    {
      std::lock_guard<std::mutex> lk(mutex);
      auto svc = services.find(request.service_name());
      if (svc != services.end()) {
        for (const auto& kv : svc->second) {
          if (!IsAlive(kv.second, now)) continue;
          *response.add_instances() = kv.second.info;
        }
      }
      response.set_version(version);
    }

    if (m_fetch_total != nullptr) m_fetch_total->Inc();
    QZ_LOG_DEBUG << "registry: fetch '" << request.service_name() << "' -> "
                 << response.instances_size() << " instance(s), version " << response.version();
    if (out != nullptr) {
      out->clear();
      response.SerializeToString(out);
    }
    return Status::Ok();
  }

  Status HandleDeregister(const char* payload, std::size_t len, std::string* out,
                          rpc::ServerContext* ctx) {
    (void)ctx;
    DeregisterRequest request;
    if (!request.ParseFromArray(payload, len)) {
      return Status::InvalidArgument("registry: cannot decode DeregisterRequest");
    }
    if (request.service_name().empty() || request.instance_id().empty()) {
      return Status::InvalidArgument(
          "registry: DeregisterRequest requires service_name and instance_id");
    }

    bool removed = false;
    {
      std::lock_guard<std::mutex> lk(mutex);
      auto svc = services.find(request.service_name());
      if (svc != services.end()) {
        auto erased = svc->second.erase(request.instance_id());
        removed = erased > 0;
        if (removed) {
          ++version;
          if (svc->second.empty()) services.erase(svc);  // 不留下空服务名，ListServices 才干净
        }
      }
    }

    // 契约缺口：registry_client_proto.h 里没有 DeregisterResponse。
    // 这里的取舍是"用 RegisterResponse 当通用 ack（ok/error 足够表达结果），
    // 语义主要由返回的 Status 承载"（成功 kOk / 不存在 kNotFound）。
    // 若要严格化，应在 idl/registry.qz 补一个 DeregisterResponse 再重新生成。
    RegisterResponse response;
    response.set_ok(removed);
    if (!removed) response.set_error("instance '" + request.instance_id() + "' not found");
    if (out != nullptr) {
      out->clear();
      response.SerializeToString(out);
    }

    if (!removed) {
      QZ_LOG_WARN << "registry: deregister miss for '" << request.service_name() << "' instance "
                  << request.instance_id();
      return Status::NotFound("registry: instance '" + request.instance_id() + "' not found");
    }
    QZ_LOG_INFO << "registry: deregistered '" << request.service_name() << "' instance "
                << request.instance_id();
    return Status::Ok();
  }

  // ---- TTL 清扫 ----

  void SweeperLoop() {
    const int interval = std::max(kMinSweepIntervalMs, options.sweep_interval_ms);
    std::unique_lock<std::mutex> lock(sweeper_mutex);
    while (!sweeper_stopping) {
      // 用 wait_for 而不是 sleep：Stop 必须能立刻叫醒它（否则停机要等一个周期）。
      sweeper_cv.wait_for(lock, std::chrono::milliseconds(interval));
      if (sweeper_stopping) break;
      lock.unlock();
      SweepOnce();
      lock.lock();
    }
  }

  void SweepOnce() {
    const std::uint64_t now = TimeUtil::NowMs();
    std::vector<std::pair<std::string, std::string>> expired;
    {
      std::lock_guard<std::mutex> lk(mutex);
      for (auto svc = services.begin(); svc != services.end();) {
        for (auto it = svc->second.begin(); it != svc->second.end();) {
          if (!IsAlive(it->second, now)) {
            expired.emplace_back(svc->first, it->first);
            it = svc->second.erase(it);
          } else {
            ++it;
          }
        }
        if (svc->second.empty()) {
          svc = services.erase(svc);
        } else {
          ++svc;
        }
      }
      if (!expired.empty()) ++version;
    }
    if (expired.empty()) return;

    if (m_expired_total != nullptr) m_expired_total->Inc(expired.size());
    for (const auto& item : expired) {
      QZ_LOG_INFO << "registry: instance expired '" << item.first << "' / " << item.second;
    }
  }

  void StartSweeper() {
    std::lock_guard<std::mutex> lk(sweeper_mutex);
    if (sweeper.joinable()) return;
    sweeper_stopping = false;
    sweeper = std::thread([this] { SweeperLoop(); });
  }

  void StopSweeper() {
    {
      std::lock_guard<std::mutex> lk(sweeper_mutex);
      if (!sweeper.joinable()) return;
      sweeper_stopping = true;
    }
    sweeper_cv.notify_all();
    sweeper.join();
  }

  RegistryServerOptions options;
  std::string error_message;
  bool valid{false};

  std::unique_ptr<rpc::Server> server;

  // 一把大锁串行化全部目录操作。
  // 为什么不上读写锁：注册中心是控制面（每客户端 3 秒一次拉取），QPS 低到锁竞争
  // 完全不是瓶颈；而"清扫线程 + 4 个 handler + List* 查询"的多方一致性，
  // 用一把锁换取"永远不可能读到半个目录"是明显划算的交易。
  mutable std::mutex mutex;
  std::map<std::string, std::map<std::string, InstanceRecord>> services;
  std::uint64_t version{0};

  std::thread sweeper;
  std::mutex sweeper_mutex;
  std::condition_variable sweeper_cv;
  bool sweeper_stopping{false};

  Counter* m_register_total{nullptr};
  Counter* m_heartbeat_total{nullptr};
  Counter* m_fetch_total{nullptr};
  Counter* m_expired_total{nullptr};
};

// ---------------------------------------------------------------------------
// RegistryServer
// ---------------------------------------------------------------------------

RegistryServer::RegistryServer() : RegistryServer(RegistryServerOptions()) {}

RegistryServer::RegistryServer(const RegistryServerOptions& options)
    : impl_(new Impl(options)) {
  if (!impl_->valid) {
    QZ_LOG_ERROR << "registry: invalid options: " << impl_->error_message;
  }
}

RegistryServer::~RegistryServer() { Stop(); }

bool RegistryServer::valid() const noexcept { return impl_->valid; }

const std::string& RegistryServer::error_message() const noexcept {
  return impl_->error_message;
}

bool RegistryServer::Start(std::string* err) {
  if (!impl_->valid) {
    SetErr(err, impl_->error_message);
    return false;
  }
  if (!impl_->server) {
    SetErr(err, "registry: server is not constructed");
    return false;
  }
  if (impl_->server->running()) return true;
  if (!impl_->server->Start(err)) return false;

  impl_->StartSweeper();
  QZ_LOG_INFO << "registry: listening on " << impl_->options.server.bind_ip << ":"
              << impl_->server->port() << " (ttl " << impl_->options.instance_ttl_ms
              << "ms, sweep " << impl_->options.sweep_interval_ms << "ms)";
  return true;
}

void RegistryServer::Run() {
  if (!impl_->valid || !impl_->server) return;
  if (!impl_->server->running()) {
    std::string err;
    if (!Start(&err)) {
      QZ_LOG_ERROR << "registry: Run() aborted: " << err;
      return;
    }
  }
  impl_->server->Run();  // 阻塞到 Stop
}

void RegistryServer::Stop() {
  if (!impl_) return;
  // 顺序：先停清扫线程（它持有目录锁），再停 RPC 服务。
  // 反过来会出现"server 已经 Stop 但清扫线程还在改目录"的窗口 —— 不算错，
  // 但会让停机期间产生无法解释的日志。
  impl_->StopSweeper();
  if (impl_->server) impl_->server->Stop();
}

bool RegistryServer::running() const noexcept {
  return impl_->server != nullptr && impl_->server->running();
}

std::uint16_t RegistryServer::port() const noexcept {
  return impl_->server == nullptr ? 0 : impl_->server->port();
}

std::vector<ServiceInstance> RegistryServer::ListInstances(
    const std::string& service_name) const {
  const std::uint64_t now = TimeUtil::NowMs();
  std::vector<ServiceInstance> out;
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto svc = impl_->services.find(service_name);
  if (svc == impl_->services.end()) return out;
  out.reserve(svc->second.size());
  for (const auto& kv : svc->second) {
    if (!impl_->IsAlive(kv.second, now)) continue;  // 已过期但尚未被清扫的，不对外暴露
    out.push_back(kv.second.info);
  }
  return out;
}

std::vector<std::string> RegistryServer::ListServices() const {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lk(impl_->mutex);
  out.reserve(impl_->services.size());
  for (const auto& kv : impl_->services) out.push_back(kv.first);
  return out;  // std::map 天然有序
}

std::size_t RegistryServer::service_count() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->services.size();
}

std::size_t RegistryServer::instance_count() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::size_t total = 0;
  for (const auto& kv : impl_->services) total += kv.second.size();
  return total;
}

std::uint64_t RegistryServer::register_total() const noexcept {
  return impl_->m_register_total == nullptr ? 0 : impl_->m_register_total->value();
}

std::uint64_t RegistryServer::heartbeat_total() const noexcept {
  return impl_->m_heartbeat_total == nullptr ? 0 : impl_->m_heartbeat_total->value();
}

std::uint64_t RegistryServer::fetch_total() const noexcept {
  return impl_->m_fetch_total == nullptr ? 0 : impl_->m_fetch_total->value();
}

std::uint64_t RegistryServer::expired_total() const noexcept {
  return impl_->m_expired_total == nullptr ? 0 : impl_->m_expired_total->value();
}

}  // namespace registry
}  // namespace qzrpc
