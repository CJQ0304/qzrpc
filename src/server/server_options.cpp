// 轻舟 RPC (Qingzhou RPC)
// src/server/server_options.cpp
//
// 职责：ServerOptions 的 INI 读入 / 文本导出 / 合法性校验。
//
// 设计取舍（与 phxrpc 的 ServerConfig 类层级对照）：
//   * 本文件只做"配置文本 → POD 字段"的翻译，不持有任何全局状态，
//     因此同一个进程里可以并存多个 ServerOptions（单进程多实例）。
//   * Config::GetInt/GetBool 在"值非法"时会静默返回默认值，这在生产里是灾难
//     （写错一个字母就悄悄跑默认配置）。所以本文件用 with-Has + 哨兵值 的方式
//     区分"缺失"（保留默认，正常）与"非法"（记错误，FromConfig 返回 false），
//     并把**所有**非法项聚合进 err，而不是只报第一个。
//   * ToIniText() 输出的是**当前生效值**（不是硬编码默认值），因此它既能当默认
//     配置文件模板，也能当"dump 当前配置"的运维工具；配合 FromConfig 必须能往返
//     （我已用自检程序验证 round-trip 后所有字段不变）。

#include "server/server_options.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "common/logger.h"
#include "common/string_util.h"
#include "net/platform.h"
#include "protocol/protocol_const.h"

namespace qzrpc {
namespace rpc {

namespace {

// 判定"值存在但无法解析"的哨兵：Config::GetInt 在解析失败时返回我们传入的默认值。
constexpr std::int64_t kIntSentinel = std::numeric_limits<std::int64_t>::min();

bool IsTruthyText(const std::string& lowered) {
  return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

bool IsFalsyText(const std::string& lowered) {
  return lowered.empty() || lowered == "0" || lowered == "false" || lowered == "no" ||
         lowered == "off";
}

// 把 IP 字面量校验与解析都收在一处（不依赖 DNS：inet_pton 只认字面量）。
bool IsIpLiteral(const std::string& ip, int* out_family) {
  if (ip.empty()) return false;
  in_addr addr4;
  if (::inet_pton(AF_INET, ip.c_str(), &addr4) == 1) {
    if (out_family != nullptr) *out_family = AF_INET;
    return true;
  }
  in6_addr addr6;
  if (::inet_pton(AF_INET6, ip.c_str(), &addr6) == 1) {
    if (out_family != nullptr) *out_family = AF_INET6;
    return true;
  }
  return false;
}

// 逐字段读取器：把"缺失/非法/越界"三种情况分开处理，并聚合错误。
class FieldReader {
 public:
  FieldReader(const Config& cfg, std::string section) : cfg_(cfg), section_(std::move(section)) {}

  bool ReadString(const char* key, std::string* out) {
    if (!cfg_.Has(section_, key)) return true;  // 缺失：保留默认
    *out = cfg_.GetString(section_, key, *out);
    return true;
  }

  bool ReadInt(const char* key, std::int64_t low, std::int64_t high, std::int64_t* out) {
    if (!cfg_.Has(section_, key)) return true;
    const std::int64_t value = cfg_.GetInt(section_, key, kIntSentinel);
    if (value == kIntSentinel) {
      Fail(std::string(key) + " = '" + cfg_.GetString(section_, key, std::string()) +
           "' is not a valid integer");
      return false;
    }
    if (value < low || value > high) {
      Fail(std::string(key) + " = " + std::to_string(value) + " out of range [" +
           std::to_string(low) + ", " + std::to_string(high) + "]");
      return false;
    }
    *out = value;
    return true;
  }

  bool ReadBool(const char* key, bool* out) {
    if (!cfg_.Has(section_, key)) return true;
    const std::string raw = StringUtil::ToLower(cfg_.GetString(section_, key, std::string()));
    if (IsTruthyText(raw)) {
      *out = true;
      return true;
    }
    if (IsFalsyText(raw)) {
      *out = false;
      return true;
    }
    Fail(std::string(key) + " = '" + raw + "' is not a boolean (accept 1/true/yes/on or 0/false/no/off)");
    return false;
  }

  bool ok() const noexcept { return errors_.empty(); }

  std::string errors() const {
    std::string joined;
    for (std::size_t i = 0; i < errors_.size(); ++i) {
      if (i != 0) joined += "; ";
      joined += errors_[i];
    }
    return joined;
  }

 private:
  void Fail(std::string message) { errors_.push_back("[" + section_ + "] " + std::move(message)); }

  const Config& cfg_;
  std::string section_;
  std::vector<std::string> errors_;
};

std::string FormatBool(bool value) { return value ? "1" : "0"; }

// "ip:port" → Endpoint。只切**最后一个**冒号：IPv6 字面量形如 "[::1]:16160"，
// 用第一个冒号会把地址切碎。这里不解析域名（服务端配置里的地址必须是字面量，
// 理由与 bind_ip 相同：域名解析结果不可预测，注册中心地址写错要立刻报出来）。
bool ParseIpPort(const std::string& text, std::string* ip, std::uint16_t* port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
  std::string host = text.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);  // 去掉 IPv6 的方括号
  }
  if (!IsIpLiteral(host, nullptr)) return false;
  const long parsed = std::strtol(text.c_str() + colon + 1, nullptr, 10);
  if (parsed <= 0 || parsed > 65535) return false;
  if (ip != nullptr) *ip = host;
  if (port != nullptr) *port = static_cast<std::uint16_t>(parsed);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// FromConfig
// ---------------------------------------------------------------------------

bool ServerOptions::FromConfig(const Config& cfg, const std::string& section, std::string* err) {
  FieldReader reader(cfg, section);

  // ---- 网络 ----
  reader.ReadString("bind_ip", &bind_ip);
  {
    std::int64_t value = port;
    if (reader.ReadInt("port", 0, 65535, &value)) port = static_cast<std::uint16_t>(value);
  }
  {
    std::int64_t value = backlog;
    if (reader.ReadInt("backlog", 1, 65535, &value)) backlog = static_cast<int>(value);
  }

  // ---- 线程模型 ----
  {
    std::int64_t value = static_cast<std::int64_t>(io_thread_count);
    if (reader.ReadInt("io_thread_count", 1, 256, &value)) {
      io_thread_count = static_cast<std::size_t>(value);
    }
  }
  {
    std::int64_t value = static_cast<std::int64_t>(worker_thread_count);
    if (reader.ReadInt("worker_thread_count", 1, 1024, &value)) {
      worker_thread_count = static_cast<std::size_t>(value);
    }
  }
  {
    std::int64_t value = static_cast<std::int64_t>(max_pending_tasks);
    if (reader.ReadInt("max_pending_tasks", 0, 10000000, &value)) {
      max_pending_tasks = static_cast<std::size_t>(value);
    }
  }

  // ---- 连接与超时 ----
  {
    std::int64_t value = static_cast<std::int64_t>(max_connections);
    if (reader.ReadInt("max_connections", 1, 1000000, &value)) {
      max_connections = static_cast<std::size_t>(value);
    }
  }
  {
    std::int64_t value = static_cast<std::int64_t>(max_frame_size);
    if (reader.ReadInt("max_frame_size", static_cast<std::int64_t>(protocol::kMetadataLengthSize),
                       static_cast<std::int64_t>(protocol::kAbsoluteMaxFrameSize), &value)) {
      max_frame_size = static_cast<std::uint32_t>(value);
    }
  }
  {
    std::int64_t value = idle_timeout_ms;
    if (reader.ReadInt("idle_timeout_ms", 0, 86400000, &value)) {
      idle_timeout_ms = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = max_accept_batch;
    if (reader.ReadInt("max_accept_batch", 1, 4096, &value)) {
      max_accept_batch = static_cast<int>(value);
    }
  }
  reader.ReadBool("enable_nodelay", &enable_nodelay);

  // ---- 业务超时 ----
  {
    std::int64_t value = request_timeout_ms;
    if (reader.ReadInt("request_timeout_ms", 1, 600000, &value)) {
      request_timeout_ms = static_cast<int>(value);
    }
  }

  // ---- 过载保护 ----
  reader.ReadBool("enable_adaptive_reject", &enable_adaptive_reject);
  {
    std::int64_t value = fast_reject_threshold_ms;
    if (reader.ReadInt("fast_reject_threshold_ms", 0, 60000, &value)) {
      fast_reject_threshold_ms = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = fast_reject_adjust_rate;
    if (reader.ReadInt("fast_reject_adjust_rate", 1, 50, &value)) {
      fast_reject_adjust_rate = static_cast<int>(value);
    }
  }

  // ---- 可观测性 ----
  reader.ReadBool("enable_metrics", &enable_metrics);
  {
    std::int64_t value = metrics_port;
    if (reader.ReadInt("metrics_port", 0, 65535, &value)) {
      metrics_port = static_cast<std::uint16_t>(value);
    }
  }
  reader.ReadString("metrics_bind_ip", &metrics_bind_ip);
  reader.ReadString("service_name", &service_name);

  // ---- 服务发现（自注册）----
  reader.ReadString("registry_endpoint", &registry_endpoint);
  {
    std::int64_t value = registry_heartbeat_interval_ms;
    if (reader.ReadInt("registry_heartbeat_interval_ms", 100, 60000, &value)) {
      registry_heartbeat_interval_ms = static_cast<int>(value);
    }
  }

  // ---- 优雅停机 / 日志 ----
  {
    std::int64_t value = graceful_shutdown_timeout_ms;
    if (reader.ReadInt("graceful_shutdown_timeout_ms", 0, 600000, &value)) {
      graceful_shutdown_timeout_ms = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = log_level;
    if (reader.ReadInt("log_level", static_cast<std::int64_t>(Logger::kTrace),
                       static_cast<std::int64_t>(Logger::kOff), &value)) {
      log_level = static_cast<int>(value);
    }
  }

  if (!reader.ok()) {
    if (err != nullptr) *err = reader.errors();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// ToIniText
// ---------------------------------------------------------------------------

std::string ServerOptions::ToIniText() const {
  std::string out;
  out.reserve(2048);
  out += "# ============================================================\n";
  out += "# 轻舟 RPC (Qingzhou RPC) 服务端配置\n";
  out += "# 本文件由 ServerOptions::ToIniText() 生成，反映**当前生效值**：\n";
  out += "#   * 可以直接当默认配置模板使用；\n";
  out += "#   * 也可以用它在线上 dump 当前配置（便于排查\"到底读到了什么\"）。\n";
  out += "# 语法：段 [Section]、键值 Key = Value、# 或 ; 起注释、行内 # 之后为注释。\n";
  out += "# ============================================================\n";
  out += "\n[Server]\n";

  out += "# ---- 网络 ----\n";
  out += "# 监听地址。0.0.0.0 表示全部网卡；只写 IP 字面量（不支持域名）。\n";
  out += "bind_ip = " + bind_ip + "\n";
  out += "# 监听端口。0 表示由系统分配（测试常用，通过 Server::port() 取实际值）。\n";
  out += "port = " + std::to_string(static_cast<unsigned>(port)) + "\n";
  out += "# listen backlog（已完成三次握手、尚未 accept 的队列长度）。\n";
  out += "backlog = " + std::to_string(backlog) + "\n";

  out += "\n# ---- 线程模型 ----\n";
  out += "# IO 从 Reactor 线程数（每线程一个 EventLoop）。建议 min(CPU 核数, 8)。\n";
  out += "io_thread_count = " + std::to_string(io_thread_count) + "\n";
  out += "# 业务线程数。有阻塞调用（磁盘/下游 RPC）时要给足。\n";
  out += "worker_thread_count = " + std::to_string(worker_thread_count) + "\n";
  out += "# 业务队列（有界）上限。满了立即回 OVERLOAD 帧，队列本身检查与入队同锁。\n";
  out += "max_pending_tasks = " + std::to_string(max_pending_tasks) + "\n";

  out += "\n# ---- 连接与超时 ----\n";
  out += "# 最大并发连接数。达到上限后新 accept 的连接被立即关闭（此时还没读到帧）。\n";
  out += "max_connections = " + std::to_string(max_connections) + "\n";
  out += "# 单帧 body（metadata + payload）上限，字节。硬上限 " +
         std::to_string(static_cast<unsigned long long>(protocol::kAbsoluteMaxFrameSize)) +
         "（64 MiB）。\n";
  out += "max_frame_size = " + std::to_string(max_frame_size) + "\n";
  out += "# 连接空闲回收时间（毫秒）。0 表示不回收。\n";
  out += "idle_timeout_ms = " + std::to_string(idle_timeout_ms) + "\n";
  out += "# 单次 accept 事件最多处理多少个连接（避免饿死定时器与任务队列）。\n";
  out += "max_accept_batch = " + std::to_string(max_accept_batch) + "\n";
  out += "# 是否开启 TCP_NODELAY（RPC 场景几乎总该开）。\n";
  out += "enable_nodelay = " + FormatBool(enable_nodelay) + "\n";

  out += "\n# ---- 业务超时 ----\n";
  out += "# 业务处理超时（毫秒）。超时只回 kTimeout 并记 metrics，**不强行杀线程**。\n";
  out += "request_timeout_ms = " + std::to_string(request_timeout_ms) + "\n";

  out += "\n# ---- 过载保护 ----\n";
  out += "# 是否启用基于排队时延闭环的自适应拒绝。\n";
  out += "enable_adaptive_reject = " + FormatBool(enable_adaptive_reject) + "\n";
  out += "# 排队时延超过该阈值（毫秒）即开始按比例拒绝。\n";
  out += "fast_reject_threshold_ms = " + std::to_string(fast_reject_threshold_ms) + "\n";
  out += "# 每轮调节的百分比步长（越大收敛越快、越易震荡，建议 5）。\n";
  out += "fast_reject_adjust_rate = " + std::to_string(fast_reject_adjust_rate) + "\n";

  out += "\n# ---- 可观测性 ----\n";
  out += "# 是否启用内建 Counter/Histogram 采集。\n";
  out += "enable_metrics = " + FormatBool(enable_metrics) + "\n";
  out += "# metrics HTTP 端口（0 = 不启动）。必须与业务端口不同。\n";
  out += "metrics_port = " + std::to_string(static_cast<unsigned>(metrics_port)) + "\n";
  out += "# metrics 监听地址（默认只听回环，不要把 /metrics 暴露到公网）。\n";
  out += "metrics_bind_ip = " + metrics_bind_ip + "\n";
  out += "# 服务名，用于 metrics label 与日志。\n";
  out += "service_name = " + service_name + "\n";

  out += "\n# ---- 服务发现（自注册）----\n";
  out += "# 注册中心地址 \"ip:port\"。留空 = 不注册（纯静态 endpoint 部署）。\n";
  out += "# 填了之后本实例启动成功即 Register，并按下面的间隔发 Heartbeat；\n";
  out += "# Stop() 时发一次 Deregister（尽力而为）。注册失败不会阻止本机提供服务。\n";
  out += "registry_endpoint = " + registry_endpoint + "\n";
  out += "# 心跳间隔（毫秒）。必须显著小于注册中心的实例 TTL（默认 15000），\n";
  out += "# 否则实例会在两次心跳之间被摘除。通常取 TTL 的 1/3。\n";
  out += "registry_heartbeat_interval_ms = " + std::to_string(registry_heartbeat_interval_ms) + "\n";

  out += "\n# ---- 优雅停机与日志 ----\n";
  out += "# 排空在飞请求的最长等待（毫秒），超时后强制关闭连接。\n";
  out += "graceful_shutdown_timeout_ms = " + std::to_string(graceful_shutdown_timeout_ms) + "\n";
  out += "# 日志级别：0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=FATAL 6=OFF。\n";
  out += "log_level = " + std::to_string(log_level) + "\n";
  return out;
}

// ---------------------------------------------------------------------------
// ToString
// ---------------------------------------------------------------------------

std::string ServerOptions::ToString() const {
  // 一行摘要：字段多，但启动日志只需要这几个关键值；细节看 ToIniText()。
  return "bind=" + bind_ip + ":" + std::to_string(static_cast<unsigned>(port)) +
         " backlog=" + std::to_string(backlog) + " io_threads=" + std::to_string(io_thread_count) +
         " workers=" + std::to_string(worker_thread_count) +
         " max_conn=" + std::to_string(max_connections) +
         " max_frame=" + std::to_string(max_frame_size) +
         " request_timeout_ms=" + std::to_string(request_timeout_ms) +
         " idle_timeout_ms=" + std::to_string(idle_timeout_ms) +
         " adaptive_reject=" + FormatBool(enable_adaptive_reject) +
         " metrics=" + (metrics_port == 0 ? std::string("off")
                                          : metrics_bind_ip + ":" +
                                                std::to_string(static_cast<unsigned>(metrics_port))) +
         " registry=" + (registry_endpoint.empty() ? std::string("off") : registry_endpoint) +
         " name=" + service_name;
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

bool ServerOptions::Validate(std::string* err) const {
  std::vector<std::string> problems;

  // ---- 监听地址 ----
  int bind_family = 0;
  if (!IsIpLiteral(bind_ip, &bind_family)) {
    // "0.0.0.0" 与 "::" 都是合法字面量，能过这一关；域名在这里会被拒绝，
    // 因为服务端绑定的必须是明确的本地地址，域名解析结果可能不可预测。
    problems.push_back("bind_ip '" + bind_ip + "' is not an IPv4/IPv6 literal");
  }

  // ---- 端口 ----
  // port 是 uint16，天然 <= 65535；这里只提示"0 表示随机端口"这一特殊情况。
  // （保留显式检查是为了将来 port 类型变更时不会静默失效。）
  if (port > 65535) problems.push_back("port " + std::to_string(port) + " > 65535");

  // ---- 线程模型 ----
  if (io_thread_count == 0) problems.push_back("io_thread_count must be >= 1");
  if (io_thread_count > 256) problems.push_back("io_thread_count > 256 is not supported");
  if (worker_thread_count == 0) problems.push_back("worker_thread_count must be >= 1");
  if (worker_thread_count > 1024) problems.push_back("worker_thread_count > 1024 is not supported");

  // ---- 连接与帧 ----
  if (max_connections == 0) problems.push_back("max_connections must be >= 1");
  if (backlog < 1) problems.push_back("backlog must be >= 1");
  if (max_accept_batch < 1) problems.push_back("max_accept_batch must be >= 1");
  if (max_frame_size < protocol::kMetadataLengthSize) {
    problems.push_back("max_frame_size must be >= 4 (metadata length prefix)");
  } else if (max_frame_size > protocol::kAbsoluteMaxFrameSize) {
    // 这是最危险的一条：配置写成超大值会让"攻击者用一个 4GB 的 body_len 就打死进程"
    // 的防护失效（doc 03 §9）。
    problems.push_back("max_frame_size " + std::to_string(max_frame_size) +
                       " exceeds absolute limit " +
                       std::to_string(protocol::kAbsoluteMaxFrameSize));
  }

  // ---- 超时 ----
  if (request_timeout_ms < 1) problems.push_back("request_timeout_ms must be >= 1");
  if (idle_timeout_ms < 0) problems.push_back("idle_timeout_ms must be >= 0");
  if (graceful_shutdown_timeout_ms < 0) {
    problems.push_back("graceful_shutdown_timeout_ms must be >= 0");
  }

  // ---- 过载保护 ----
  if (fast_reject_threshold_ms < 0) problems.push_back("fast_reject_threshold_ms must be >= 0");
  if (fast_reject_adjust_rate < 1 || fast_reject_adjust_rate > 50) {
    problems.push_back("fast_reject_adjust_rate must be in [1, 50]");
  }

  // ---- metrics ----
  if (metrics_port != 0) {
    int metrics_family = 0;
    if (!IsIpLiteral(metrics_bind_ip, &metrics_family)) {
      problems.push_back("metrics_bind_ip '" + metrics_bind_ip +
                         "' is not an IPv4/IPv6 literal");
    }
    // 同地址同端口必然 bind 失败，提前拦住（比启动时抛 EADDRINUSE 好排查）。
    if (metrics_port == port && metrics_bind_ip == bind_ip) {
      problems.push_back("metrics_port equals port on the same bind address");
    }
  }

  if (service_name.empty()) problems.push_back("service_name must not be empty");

  // ---- 服务发现（自注册）----
  // 三条都要查，而且都必须在**启动前**查出来：
  // 注册中心地址写错是运维最常见的笔误，让它变成"服务起来了但永远发现不到"最难查。
  if (!registry_endpoint.empty()) {
    if (!ParseIpPort(registry_endpoint, nullptr, nullptr)) {
      problems.push_back("registry_endpoint '" + registry_endpoint +
                         "' is not a valid ip:port literal");
    }
    // 注册中心的 instance_ttl_ms 默认 15000（见 registry_server.cpp）。
    // 心跳比 TTL 还长 → 每次心跳之前实例就已经被摘除了，等于没注册。
    // 服务端配置里拿不到对方的 TTL，因此用这个"常识上限"卡住明显写错的值。
    if (registry_heartbeat_interval_ms >= 15000) {
      problems.push_back("registry_heartbeat_interval_ms " +
                         std::to_string(registry_heartbeat_interval_ms) +
                         " must be shorter than the registry's default instance TTL (15000ms)");
    }
  }
  if (registry_heartbeat_interval_ms < 100 || registry_heartbeat_interval_ms > 60000) {
    problems.push_back("registry_heartbeat_interval_ms " +
                       std::to_string(registry_heartbeat_interval_ms) +
                       " out of range [100, 60000]");
  }

  // ---- 日志级别 ----
  if (log_level < static_cast<int>(Logger::kTrace) || log_level > static_cast<int>(Logger::kOff)) {
    problems.push_back("log_level " + std::to_string(log_level) + " out of range [0, 6]");
  }

  if (problems.empty()) return true;
  if (err != nullptr) {
    std::string joined;
    for (std::size_t i = 0; i < problems.size(); ++i) {
      if (i != 0) joined += "; ";
      joined += problems[i];
    }
    *err = joined;
  }
  return false;
}

}  // namespace rpc
}  // namespace qzrpc
