// 轻舟 RPC (Qingzhou RPC)
// src/client/client_options.cpp
//
// 职责：ClientOptions 的 INI 读写与校验。
//
// 与 server_options.cpp 用同一套严格读法：
//   * 用 Has() + 哨兵值区分"缺失（保留默认）"与"值非法（记错误）"——Config::GetInt 在
//     值非法时会静默返回默认值，这在生产里等于"写错一个字母就悄悄跑默认配置"；
//   * 所有非法项聚合进 err，一次报全，而不是只报第一个；
//   * ToIniText() 反映**当前生效值**，与 FromConfig 必须能往返。
//
// endpoint 的写法（与 WriteTo 对称）：
//   [Client]
//   endpoint_count = 2
//   endpoint_0 = 127.0.0.1:16161
//   endpoint_1 = [::1]:16161          # IPv6 用方括号包住地址
//   registry_endpoint = 127.0.0.1:16162
// 注意：endpoint_* 存在时**增量覆盖**，不会清空从代码里预设的 endpoints——
//      只有 endpoint_count 出现时才按计数重建整个列表。

#include "client/client_options.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/string_util.h"
#include "protocol/protocol_const.h"

namespace qzrpc {
namespace rpc {

namespace {

constexpr std::int64_t kIntSentinel = std::numeric_limits<std::int64_t>::min();

bool IsTruthyText(const std::string& lowered) {
  return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

bool IsFalsyText(const std::string& lowered) {
  return lowered.empty() || lowered == "0" || lowered == "false" || lowered == "no" ||
         lowered == "off";
}

// "1.2.3.4:8080" / "[::1]:8080" → Endpoint。
// 只接受 IP 字面量（服务端绑定/客户端连接都不做域名解析：解析结果不稳定，
// 且 RPC 客户端的 endpoint 来自配置或注册中心，本来就该是字面量）。
bool ParseEndpoint(const std::string& text, net::Endpoint* out, std::string* why) {
  if (text.empty()) {
    *why = "empty endpoint";
    return false;
  }
  std::string ip;
  std::string port_text;
  if (text.front() == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') {
      *why = "'" + text + "' is not a valid [ipv6]:port endpoint";
      return false;
    }
    ip = text.substr(1, close - 1);
    port_text = text.substr(close + 2);
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
      *why = "'" + text + "' is missing ':port'";
      return false;
    }
    ip = text.substr(0, colon);
    port_text = text.substr(colon + 1);
  }
  if (ip.empty()) {
    *why = "'" + text + "' has an empty ip";
    return false;
  }
  // 端口必须是纯十进制、1..65535（0 对客户端无意义）。
  if (port_text.empty() || port_text.size() > 5) {
    *why = "'" + text + "' has an invalid port";
    return false;
  }
  for (const char c : port_text) {
    if (c < '0' || c > '9') {
      *why = "'" + text + "' has a non-numeric port";
      return false;
    }
  }
  const long port = std::strtol(port_text.c_str(), nullptr, 10);
  if (port <= 0 || port > 65535) {
    *why = "'" + text + "' port out of range [1, 65535]";
    return false;
  }
  out->ip = ip;
  out->port = static_cast<std::uint16_t>(port);
  return true;
}

std::string FormatEndpoint(const net::Endpoint& endpoint) { return endpoint.ToString(); }

// "127.0.0.1:16162" → 同一个 Endpoint（注册中心地址复用同一套解析）。
bool ParseRegistryEndpoint(const std::string& text, net::Endpoint* out, std::string* why) {
  if (text.empty()) return true;  // 空 = 不使用注册中心
  return ParseEndpoint(text, out, why);
}

class FieldReader {
 public:
  FieldReader(const Config& cfg, std::string section) : cfg_(cfg), section_(std::move(section)) {}

  void ReadString(const char* key, std::string* out) {
    if (!cfg_.Has(section_, key)) return;
    *out = cfg_.GetString(section_, key, *out);
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
    Fail(std::string(key) + " = '" + raw +
         "' is not a boolean (accept 1/true/yes/on or 0/false/no/off)");
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

  void Fail(std::string message) {
    errors_.push_back("[" + section_ + "] " + std::move(message));
  }

  const Config& config() const noexcept { return cfg_; }
  const std::string& section() const noexcept { return section_; }

 private:
  const Config& cfg_;
  std::string section_;
  std::vector<std::string> errors_;
};

}  // namespace

// ---------------------------------------------------------------------------
// FromConfig
// ---------------------------------------------------------------------------

bool ClientOptions::FromConfig(const Config& cfg, const std::string& section, std::string* err) {
  FieldReader reader(cfg, section);

  reader.ReadString("service_name", &service_name);
  reader.ReadString("caller_name", &caller_name);

  // ---- endpoints：只有出现 endpoint_count 才重建列表，避免误清空代码里预设的值 ----
  if (cfg.Has(section, "endpoint_count")) {
    std::int64_t count = 0;
    if (reader.ReadInt("endpoint_count", 0, 4096, &count)) {
      std::vector<net::Endpoint> parsed;
      parsed.reserve(static_cast<std::size_t>(count));
      for (std::int64_t i = 0; i < count; ++i) {
        const std::string key = "endpoint_" + std::to_string(i);
        if (!cfg.Has(section, key)) {
          reader.Fail(key + " is missing (endpoint_count = " + std::to_string(count) + ")");
          continue;
        }
        net::Endpoint endpoint;
        std::string why;
        if (!ParseEndpoint(cfg.GetString(section, key, std::string()), &endpoint, &why)) {
          reader.Fail(key + ": " + why);
          continue;
        }
        parsed.push_back(endpoint);
      }
      // 只有全部解析成功才整体替换，避免"半套 endpoint 上线"这种最难排查的状态。
      if (parsed.size() == static_cast<std::size_t>(count)) endpoints = std::move(parsed);
    }
  }

  // ---- 超时与重试 ----
  {
    std::int64_t value = connect_timeout_ms;
    if (reader.ReadInt("connect_timeout_ms", 1, 600000, &value)) {
      connect_timeout_ms = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = request_timeout_ms;
    if (reader.ReadInt("request_timeout_ms", 1, 3600000, &value)) {
      request_timeout_ms = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = max_retries;
    if (reader.ReadInt("max_retries", 0, 20, &value)) max_retries = static_cast<int>(value);
  }
  {
    std::int64_t value = retry_backoff_ms;
    if (reader.ReadInt("retry_backoff_ms", 0, 60000, &value)) {
      retry_backoff_ms = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = acquire_timeout_ms;
    if (reader.ReadInt("acquire_timeout_ms", 1, 600000, &value)) {
      acquire_timeout_ms = static_cast<int>(value);
    }
  }

  // ---- 连接池 ----
  {
    std::int64_t value = static_cast<std::int64_t>(connections_per_endpoint);
    if (reader.ReadInt("connections_per_endpoint", 1, 1024, &value)) {
      connections_per_endpoint = static_cast<std::size_t>(value);
    }
  }
  {
    std::int64_t value = idle_connection_timeout_ms;
    if (reader.ReadInt("idle_connection_timeout_ms", 0, 86400000, &value)) {
      idle_connection_timeout_ms = static_cast<int>(value);
    }
  }
  reader.ReadBool("enable_nodelay", &enable_nodelay);
  {
    std::int64_t value = static_cast<std::int64_t>(max_frame_size);
    if (reader.ReadInt("max_frame_size", static_cast<std::int64_t>(protocol::kMetadataLengthSize),
                       static_cast<std::int64_t>(protocol::kAbsoluteMaxFrameSize), &value)) {
      max_frame_size = static_cast<std::uint32_t>(value);
    }
  }

  // ---- 熔断 ----
  {
    std::int64_t value = breaker_failure_threshold;
    if (reader.ReadInt("breaker_failure_threshold", 1, 1000, &value)) {
      breaker_failure_threshold = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = breaker_cooldown_ms;
    if (reader.ReadInt("breaker_cooldown_ms", 1, 600000, &value)) {
      breaker_cooldown_ms = static_cast<int>(value);
    }
  }

  // ---- 服务发现 ----
  reader.ReadString("registry_endpoint", &registry_endpoint);
  {
    std::int64_t value = registry_poll_interval_ms;
    if (reader.ReadInt("registry_poll_interval_ms", 100, 3600000, &value)) {
      registry_poll_interval_ms = static_cast<int>(value);
    }
  }
  {
    std::int64_t value = registry_request_timeout_ms;
    if (reader.ReadInt("registry_request_timeout_ms", 1, 600000, &value)) {
      registry_request_timeout_ms = static_cast<int>(value);
    }
  }

  // ---- 其它 ----
  reader.ReadBool("enable_metrics", &enable_metrics);
  reader.ReadBool("auto_trace_id", &auto_trace_id);

  if (!reader.ok()) {
    if (err != nullptr) *err = reader.errors();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// WriteTo
// ---------------------------------------------------------------------------

void ClientOptions::WriteTo(Config* cfg, const std::string& section) const {
  if (cfg == nullptr) return;
  cfg->Set(section, "service_name", service_name);
  cfg->Set(section, "endpoint_count", std::to_string(endpoints.size()));
  for (std::size_t i = 0; i < endpoints.size(); ++i) {
    cfg->Set(section, "endpoint_" + std::to_string(i), FormatEndpoint(endpoints[i]));
  }
  // 写回时把旧的 endpoint_i 清掉（缩容场景：原来 3 个，现在 2 个）。
  for (std::size_t i = endpoints.size(); i < endpoints.size() + 8; ++i) {
    const std::string key = "endpoint_" + std::to_string(i);
    if (!cfg->Has(section, key)) break;
    cfg->Set(section, key, std::string());
  }
  cfg->Set(section, "registry_endpoint", registry_endpoint);
  cfg->Set(section, "caller_name", caller_name);
  cfg->Set(section, "connect_timeout_ms", std::to_string(connect_timeout_ms));
  cfg->Set(section, "request_timeout_ms", std::to_string(request_timeout_ms));
  cfg->Set(section, "max_retries", std::to_string(max_retries));
  cfg->Set(section, "retry_backoff_ms", std::to_string(retry_backoff_ms));
  cfg->Set(section, "acquire_timeout_ms", std::to_string(acquire_timeout_ms));
  cfg->Set(section, "connections_per_endpoint", std::to_string(connections_per_endpoint));
  cfg->Set(section, "idle_connection_timeout_ms", std::to_string(idle_connection_timeout_ms));
  cfg->Set(section, "enable_nodelay", enable_nodelay ? "1" : "0");
  cfg->Set(section, "max_frame_size", std::to_string(max_frame_size));
  cfg->Set(section, "breaker_failure_threshold", std::to_string(breaker_failure_threshold));
  cfg->Set(section, "breaker_cooldown_ms", std::to_string(breaker_cooldown_ms));
  cfg->Set(section, "registry_poll_interval_ms", std::to_string(registry_poll_interval_ms));
  cfg->Set(section, "registry_request_timeout_ms", std::to_string(registry_request_timeout_ms));
  cfg->Set(section, "enable_metrics", enable_metrics ? "1" : "0");
  cfg->Set(section, "auto_trace_id", auto_trace_id ? "1" : "0");
}

// ---------------------------------------------------------------------------
// ToIniText
// ---------------------------------------------------------------------------

std::string ClientOptions::ToIniText() const {
  std::string out;
  out.reserve(2048);
  out += "# ============================================================\n";
  out += "# 轻舟 RPC (Qingzhou RPC) 客户端配置（反映当前生效值）\n";
  out += "# ============================================================\n";
  out += "\n[Client]\n";

  out += "# 服务名（注册中心查询用，也是日志/metrics 的 label）。\n";
  out += "service_name = " + service_name + "\n";
  out += "# 调用方标识，写进请求 metadata 的 caller 字段，便于服务端定位上游。\n";
  out += "caller_name = " + caller_name + "\n";

  out += "\n# ---- 目标 ----\n";
  out += "# 静态 endpoint 列表；使用注册中心时这里是兜底地址（注册中心不可用仍可工作）。\n";
  out += "# IPv6 用方括号：[::1]:16161\n";
  out += "endpoint_count = " + std::to_string(endpoints.size()) + "\n";
  for (std::size_t i = 0; i < endpoints.size(); ++i) {
    out += "endpoint_" + std::to_string(i) + " = " + FormatEndpoint(endpoints[i]) + "\n";
  }
  out += "# 注册中心地址（ip:port）。留空表示不使用服务发现，只用上面的静态列表。\n";
  out += "registry_endpoint = " + registry_endpoint + "\n";

  out += "\n# ---- 超时与重试 ----\n";
  out += "# 建连超时（毫秒）。\n";
  out += "connect_timeout_ms = " + std::to_string(connect_timeout_ms) + "\n";
  out += "# 单次请求默认超时（毫秒），CallOptions::timeout_ms 可逐次覆盖。\n";
  out += "request_timeout_ms = " + std::to_string(request_timeout_ms) + "\n";
  out += "# 最大重试次数（不含首次）。仅当调用标记幂等且错误可重试时才真正重试。\n";
  out += "max_retries = " + std::to_string(max_retries) + "\n";
  out += "# 重试退避基数（毫秒）：第 n 次重试等待 n * retry_backoff_ms。\n";
  out += "retry_backoff_ms = " + std::to_string(retry_backoff_ms) + "\n";
  out += "# 从连接池借连接的最长等待（毫秒）。\n";
  out += "acquire_timeout_ms = " + std::to_string(acquire_timeout_ms) + "\n";

  out += "\n# ---- 连接池 ----\n";
  out += "# 每个 endpoint 维持的长连接数（1 = 串行；高并发建议 2~4）。\n";
  out += "connections_per_endpoint = " + std::to_string(connections_per_endpoint) + "\n";
  out += "# 空闲连接回收时长（毫秒）。0 = 不回收。\n";
  out += "idle_connection_timeout_ms = " + std::to_string(idle_connection_timeout_ms) + "\n";
  out += "# 是否开启 TCP_NODELAY。\n";
  out += "enable_nodelay = " + std::string(enable_nodelay ? "1" : "0") + "\n";
  out += "# 单帧 body 上限，必须与服务端一致（否则服务端会回 kInvalidArgument）。\n";
  out += "max_frame_size = " + std::to_string(max_frame_size) + "\n";

  out += "\n# ---- 熔断 ----\n";
  out += "# 连续失败多少次后把该 endpoint 标记为不可用。\n";
  out += "breaker_failure_threshold = " + std::to_string(breaker_failure_threshold) + "\n";
  out += "# 熔断冷却（毫秒），到期后允许一次试探。\n";
  out += "breaker_cooldown_ms = " + std::to_string(breaker_cooldown_ms) + "\n";

  out += "\n# ---- 服务发现 ----\n";
  out += "# 拉取服务列表的周期（毫秒）。\n";
  out += "registry_poll_interval_ms = " + std::to_string(registry_poll_interval_ms) + "\n";
  out += "# 单次拉取的超时（毫秒）。\n";
  out += "registry_request_timeout_ms = " + std::to_string(registry_request_timeout_ms) + "\n";

  out += "\n# ---- 其它 ----\n";
  out += "# 是否采集客户端指标。\n";
  out += "enable_metrics = " + std::string(enable_metrics ? "1" : "0") + "\n";
  out += "# 是否每次调用自动生成 trace_id。\n";
  out += "auto_trace_id = " + std::string(auto_trace_id ? "1" : "0") + "\n";
  return out;
}

// ---------------------------------------------------------------------------
// ToString
// ---------------------------------------------------------------------------

std::string ClientOptions::ToString() const {
  std::string out = "service=" + service_name + " endpoints=";
  if (endpoints.empty()) {
    out += "<none>";
  } else {
    // 只打前 3 个：注册中心场景下可能有几十个 endpoint，全打会把启动日志刷满。
    const std::size_t shown = endpoints.size() < 3 ? endpoints.size() : 3;
    for (std::size_t i = 0; i < shown; ++i) {
      if (i != 0) out += ",";
      out += FormatEndpoint(endpoints[i]);
    }
    if (endpoints.size() > shown) {
      out += ",...(" + std::to_string(endpoints.size()) + " total)";
    }
  }
  out += " registry=" + (registry_endpoint.empty() ? std::string("off") : registry_endpoint);
  out += " conn_per_ep=" + std::to_string(connections_per_endpoint);
  out += " req_timeout_ms=" + std::to_string(request_timeout_ms);
  out += " retries=" + std::to_string(max_retries);
  out += " acquire_timeout_ms=" + std::to_string(acquire_timeout_ms);
  return out;
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

bool ClientOptions::Validate(std::string* err) const {
  std::vector<std::string> problems;

  // 目标：静态列表为空时必须有注册中心，否则连"往哪发"都不知道。
  if (endpoints.empty() && registry_endpoint.empty()) {
    problems.push_back("no endpoint and no registry_endpoint configured");
  }

  // endpoint 内容复核（可能来自代码直接赋值，绕过了 FromConfig 的解析校验）。
  for (std::size_t i = 0; i < endpoints.size(); ++i) {
    const net::Endpoint& endpoint = endpoints[i];
    if (endpoint.ip.empty()) {
      problems.push_back("endpoint_" + std::to_string(i) + " has an empty ip");
    }
    if (endpoint.port == 0) {
      problems.push_back("endpoint_" + std::to_string(i) + " has port 0");
    }
  }

  if (!registry_endpoint.empty()) {
    net::Endpoint parsed;
    std::string why;
    if (!ParseRegistryEndpoint(registry_endpoint, &parsed, &why)) {
      problems.push_back("registry_endpoint: " + why);
    }
  }

  if (service_name.empty()) problems.push_back("service_name must not be empty");
  if (connect_timeout_ms < 1) problems.push_back("connect_timeout_ms must be >= 1");
  if (request_timeout_ms < 1) problems.push_back("request_timeout_ms must be >= 1");
  if (acquire_timeout_ms < 1) problems.push_back("acquire_timeout_ms must be >= 1");
  if (max_retries < 0) problems.push_back("max_retries must be >= 0");
  if (retry_backoff_ms < 0) problems.push_back("retry_backoff_ms must be >= 0");
  if (connections_per_endpoint == 0) problems.push_back("connections_per_endpoint must be >= 1");
  if (idle_connection_timeout_ms < 0) problems.push_back("idle_connection_timeout_ms must be >= 0");
  if (breaker_failure_threshold < 1) problems.push_back("breaker_failure_threshold must be >= 1");
  if (breaker_cooldown_ms < 1) problems.push_back("breaker_cooldown_ms must be >= 1");
  if (registry_poll_interval_ms < 100) problems.push_back("registry_poll_interval_ms must be >= 100");
  if (registry_request_timeout_ms < 1) problems.push_back("registry_request_timeout_ms must be >= 1");

  // 与 decode 侧同一把尺子：超过硬上限的 max_frame_size 会让"恶意大 body_len 预分配"
  // 的防护失效（doc 03 §9）。
  if (max_frame_size < protocol::kMetadataLengthSize) {
    problems.push_back("max_frame_size must be >= 4");
  } else if (max_frame_size > protocol::kAbsoluteMaxFrameSize) {
    problems.push_back("max_frame_size " + std::to_string(max_frame_size) +
                       " exceeds absolute limit " +
                       std::to_string(protocol::kAbsoluteMaxFrameSize));
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
