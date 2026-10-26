// 轻舟 RPC 示例：Echo 客户端
// example/echo_client.cpp
//
// 三种模式：
//   --once            调一次，打印状态 / 回显内容 / 谁服务的 / 往返耗时
//   --loop N          顺序调 N 次（默认模式，N=10），**按实例统计各被调了多少次**
//   --bench N [T]     T 个线程各调 N 次，输出 QPS / 延迟分位 / 错误分类
//
// 为什么 --loop 要按实例统计：
//   "能连上"不等于"负载均衡在工作"。单机起两个 server（16161/16162）再跑
//   `--loop 100`，两个实例的计数应该各接近 50 —— 这是"轮询真的在分发"最直接的证据，
//   也是演示时最有说服力的一屏。响应里的 served_by 就是监听端口，不需要额外协议。
//
// 用法：
//   echo_client --servers 127.0.0.1:16161,127.0.0.1:16162 --loop 100
//   echo_client --once
//   echo_client --bench 2000 4          # 4 线程 × 2000 次
//   echo_client --conf example/conf/echo_client.conf

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "client/channel.h"
#include "client/client_options.h"
#include "common/config.h"
#include "common/logger.h"
#include "common/metrics.h"
#include "common/status.h"
#include "common/string_util.h"
#include "common/time_util.h"
#include "echo.qz.h"
#include "net/socket_util.h"

namespace {

// 命令行解析结果。
struct Args {
  std::string conf_path;
  std::vector<std::string> servers;  // "ip:port"
  std::string mode = "loop";         // once | loop | bench
  int count = 10;                    // loop/bench 的次数
  int threads = 1;                   // bench 的线程数
  int conns = 0;                     // 每 endpoint 连接数；0 = 未指定，按模式推导
  int max_retries = -1;              // 覆盖 max_retries；-1 = 未指定
  int timeout_ms = 0;                // 0 = 用 ClientOptions 的默认值
  bool verbose = false;
};

void PrintUsage(const char* argv0) {
  std::printf(
      "轻舟 RPC Echo 客户端\n"
      "用法: %s [选项]\n"
      "  --conf <path>       读取 INI 配置（段名 [Client]，含 endpoint 列表）\n"
      "  --servers a,b       逗号分隔的 ip:port 列表（覆盖配置里的 endpoints）\n"
      "  --once              只调一次，打印详细结果\n"
      "  --loop N            顺序调 N 次并按实例统计（默认 N=10）\n"
      "  --bench N [T]       T 线程各调 N 次，输出 QPS / 延迟分位 / 错误分类\n"
      "  --conns N           每 endpoint 连接数（1..1024）。不给时：bench 取 max(8,T)，\n"
      "                      loop/once 取 1\n"
      "  --max-retries N     业务重试次数（0..10）。不给时：conf 里有就用 conf，否则用\n"
      "                      ClientOptions 的默认值。**不按端点数推导**：重试是语义\n"
      "                      旋钮，推导错了是重复扣款，不是快慢问题\n"
      "  --timeout <ms>      单次调用超时（0 = 用配置默认值）\n"
      "  -v, --verbose       打印每次调用（在 --loop 下很有用）\n"
      "  -h, --help          显示本帮助\n"
      "\n"
      "并行演示：\n"
      "  echo_client --servers 127.0.0.1:16161,127.0.0.1:16162 --loop 100\n",
      argv0);
}

// "ip:port" → Endpoint。只接受 IP 字面量之外也支持域名由 SocketUtil 处理，
// 但这里保持简单：用 rfind(':') 切分，域名交给 Channel 的连接层去解析。
bool ParseEndpoint(const std::string& text, qzrpc::net::Endpoint* out) {
  if (out == nullptr) return false;
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
    return false;
  }
  out->ip = text.substr(0, colon);
  const long port = std::strtol(text.c_str() + colon + 1, nullptr, 10);
  if (port <= 0 || port > 65535) return false;
  out->port = static_cast<std::uint16_t>(port);
  return true;
}

std::vector<std::string> SplitComma(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  for (char ch : text) {
    if (ch == ',') {
      if (!current.empty()) out.push_back(current);
      current.clear();
    } else if (ch != ' ') {
      current.push_back(ch);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

bool ParseArgs(int argc, char** argv, Args* args, bool* show_help) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "参数 %s 缺少取值\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") {
      *show_help = true;
    } else if (arg == "-v" || arg == "--verbose") {
      args->verbose = true;
    } else if (arg == "--once") {
      args->mode = "once";
    } else if (arg == "--loop" || arg == "--bench") {
      args->mode = (arg == "--loop") ? "loop" : "bench";
      const char* value = next(arg.c_str());
      if (value == nullptr) return false;
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed <= 0 || parsed > 100000000) {
        std::fprintf(stderr, "%s 的次数必须在 1..1e8，收到 %s\n", arg.c_str(), value);
        return false;
      }
      args->count = static_cast<int>(parsed);
      // --bench 额外接受一个"线程数"位置参数
      if (args->mode == "bench" && i + 1 < argc && argv[i + 1][0] != '-') {
        const long threads = std::strtol(argv[++i], nullptr, 10);
        if (threads <= 0 || threads > 256) {
          std::fprintf(stderr, "--bench 的线程数必须在 1..256\n");
          return false;
        }
        args->threads = static_cast<int>(threads);
      }
    } else if (arg == "--conf") {
      const char* value = next("--conf");
      if (value == nullptr) return false;
      args->conf_path = value;
    } else if (arg == "--conns") {
      const char* value = next("--conns");
      if (value == nullptr) return false;
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed < 1 || parsed > 1024) {
        std::fprintf(stderr, "--conns 必须在 1..1024，收到 %s\n", value);
        return false;
      }
      args->conns = static_cast<int>(parsed);
    } else if (arg == "--max-retries") {
      const char* value = next("--max-retries");
      if (value == nullptr) return false;
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed < 0 || parsed > 10) {
        std::fprintf(stderr, "--max-retries 必须在 0..10，收到 %s\n", value);
        return false;
      }
      args->max_retries = static_cast<int>(parsed);
    } else if (arg == "--servers") {
      const char* value = next("--servers");
      if (value == nullptr) return false;
      args->servers = SplitComma(value);
    } else if (arg == "--timeout") {
      const char* value = next("--timeout");
      if (value == nullptr) return false;
      args->timeout_ms = static_cast<int>(std::strtol(value, nullptr, 10));
    } else {
      std::fprintf(stderr, "未知参数: %s（--help 看用法）\n", arg.c_str());
      return false;
    }
  }
  return true;
}

// 构造一份请求：把几种字段都填上，让"不是只有 string 能过"这件事在演示里成立。
demo::EchoRequest MakeRequest(const std::string& text, int index) {
  demo::EchoRequest request;
  request.set_message(text);
  request.add_ids(index);          // repeated int32 → packed
  request.add_tags("demo");        // repeated string → 逐元素带 tag
  request.set_lang("zh");          // optional → 有 presence
  request.mutable_body()->set_weight(index % 100);  // 嵌套 message
  request.set_site(demo::SiteType::NEWS);           // enum
  return request;
}

// 百分位（微秒）。输入会被就地排序。
std::uint64_t Percentile(std::vector<std::uint64_t>* sorted, double p) {
  if (sorted == nullptr || sorted->empty()) return 0;
  std::size_t index = static_cast<std::size_t>(p * static_cast<double>(sorted->size() - 1));
  if (index >= sorted->size()) index = sorted->size() - 1;
  return (*sorted)[index];
}

// 字节数 → "1.24MB"。为什么不用固定单位：bench 的字节量跨三个数量级，
// 全用 B 会让 `bytes` 那行列宽随着负载漂移，人眼反而抓不到量级变化。
std::string FormatBytes(std::uint64_t value) {
  static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  const std::size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);
  double scaled = static_cast<double>(value);
  std::size_t unit = 0;
  while (scaled >= 1024.0 && unit + 1 < kUnitCount) {
    scaled /= 1024.0;
    ++unit;
  }
  char buffer[64];
  if (unit == 0) {
    std::snprintf(buffer, sizeof(buffer), "%llu%s",
                  static_cast<unsigned long long>(value), kUnits[unit]);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f%s", scaled, kUnits[unit]);
  }
  return std::string(buffer);
}

// 错误按状态码分类计数。
// 为什么不用单一的 failed 计数：`errors: 12` 看不出是"服务端过载"还是"客户端等超时"，
// 而这两者的处置方式完全相反（前者要退避，后者要查网络/调大 timeout）。
struct ErrorTally {
  std::map<qzrpc::StatusCode, std::uint64_t> by_code;
  std::uint64_t total = 0;

  void Add(qzrpc::StatusCode code) {
    ++total;
    ++by_code[code];
  }

  std::uint64_t count(qzrpc::StatusCode code) const {
    const auto it = by_code.find(code);
    return it == by_code.end() ? 0 : it->second;
  }
};

// 除 overload / timeout 之外的状态码（这两类单独占一列，其余按次数降序附在后面）。
std::vector<std::pair<qzrpc::StatusCode, std::uint64_t>> ExtraErrorCodes(
    const ErrorTally& tally) {
  std::vector<std::pair<qzrpc::StatusCode, std::uint64_t>> extra;
  for (const auto& item : tally.by_code) {
    if (item.first == qzrpc::StatusCode::kOverload) continue;
    if (item.first == qzrpc::StatusCode::kTimeout) continue;
    extra.push_back(item);
  }
  // 次数降序；同次数按码升序，保证同一批数据每次输出完全一致（便于 diff 日志）。
  std::sort(extra.begin(), extra.end(),
            [](const std::pair<qzrpc::StatusCode, std::uint64_t>& a,
               const std::pair<qzrpc::StatusCode, std::uint64_t>& b) {
              if (a.second != b.second) return a.second > b.second;
              return static_cast<int>(a.first) < static_cast<int>(b.first);
            });
  return extra;
}

void PrintTally(const std::map<std::int32_t, std::uint64_t>& tally, const char* title) {
  if (tally.empty()) return;
  std::printf("\n%s（按响应里的 served_by = 服务端监听端口）\n", title);
  std::printf("  %-10s %10s %8s\n", "端口", "次数", "占比");
  std::uint64_t total = 0;
  for (const auto& item : tally) total += item.second;
  for (const auto& item : tally) {
    const double ratio = total == 0 ? 0.0 : 100.0 * static_cast<double>(item.second) / static_cast<double>(total);
    std::printf("  %-10d %10llu %7.1f%%\n", static_cast<int>(item.first),
                static_cast<unsigned long long>(item.second), ratio);
  }
  std::printf("  %-10s %10llu\n", "合计", static_cast<unsigned long long>(total));
  if (tally.size() >= 2) {
    std::printf("  → %u 个实例都收到了请求：轮询负载均衡在工作\n",
                static_cast<unsigned>(tally.size()));
  } else {
    std::printf("  → 只有 1 个实例收到请求（若期望多实例，检查 endpoints 列表）\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  bool show_help = false;
  if (!ParseArgs(argc, argv, &args, &show_help)) return 2;
  if (show_help) {
    PrintUsage(argv[0]);
    return 0;
  }

  // ---- 1. 配置 ----
  qzrpc::rpc::ClientOptions options;
  // conf 里到底写没写 connections_per_endpoint / max_retries？这决定了"值从哪来"。
  bool conf_has_conns = false;
  bool conf_has_retries = false;
  if (!args.conf_path.empty()) {
    qzrpc::Config config;
    std::string err;
    if (!config.LoadFromFile(args.conf_path, &err)) {
      std::fprintf(stderr, "读取配置失败 %s: %s\n", args.conf_path.c_str(), err.c_str());
      return 1;
    }
    if (!options.FromConfig(config, "Client", &err)) {
      std::fprintf(stderr, "配置项非法: %s\n", err.c_str());
      return 1;
    }
    conf_has_conns = config.Has("Client", "connections_per_endpoint");
    conf_has_retries = config.Has("Client", "max_retries");
  }
  if (!args.servers.empty()) {
    options.endpoints.clear();
    for (const std::string& text : args.servers) {
      qzrpc::net::Endpoint endpoint;
      if (!ParseEndpoint(text, &endpoint)) {
        std::fprintf(stderr, "endpoint 格式非法: '%s'（应为 ip:port）\n", text.c_str());
        return 2;
      }
      options.endpoints.push_back(endpoint);
    }
  }
  if (options.endpoints.size() <= 1) {
    // 单 endpoint 也要能跑：默认连本地 16161。
    if (options.endpoints.empty()) {
      qzrpc::net::Endpoint endpoint;
      endpoint.ip = "127.0.0.1";
      endpoint.port = qzrpc::protocol::kDefaultPort;
      options.endpoints.push_back(endpoint);
    }
  }

  // ---- 每 endpoint 连接数：显式 > conf > 按模式推导 ----
  //
  // 这一段存在的原因是一个真实缺陷：这里原先对"单 endpoint"和"多 endpoint"各写死了一次
  // `connections_per_endpoint = 1`，于是
  //   * 用户在 conf 里写 connections_per_endpoint = 4，会被无声改成 1；
  //   * `--bench 2000 8` 的 8 个线程共享同一条连接，请求只能在连接上串行排队，
  //     加线程不升反降（实测 1 线程 14925 QPS / 4 线程 13418 QPS，p50 54→235us）。
  // 教训不只是"别写死"，还有：**默认值被推导 / 用户的值被改，必须打一行日志说明来源**，
  // 否则行为变成另一个，程序却一声不吭。
  std::string conns_source;
  if (args.conns > 0) {
    options.connections_per_endpoint = args.conns;
    conns_source = "cli(--conns " + std::to_string(args.conns) + ")";
  } else if (conf_has_conns) {
    // 值已经在 FromConfig 里读进来了，这里一个字节都不要动。
    conns_source = "conf(" + args.conf_path + ")";
  } else if (args.mode == "bench") {
    // 压测要打的是服务端容量，连接数不能是瓶颈：一条连接上的请求是串行的，
    // T 个线程挤 1 条连接等于没有并发，`--bench N T` 的 T 就名实不符。
    options.connections_per_endpoint = std::max(8, args.threads);
    conns_source = "auto(bench threads=" + std::to_string(args.threads) + " -> max(8,threads))";
  } else {
    // loop / once：单线程顺序调用，1 条长连接足够，而且演示分发时"每端点 1 条"
    // 正是要把 50/50 看清楚的前提。
    options.connections_per_endpoint = 1;
    conns_source = "auto(" + args.mode + ": single-threaded sequential)";
  }
  // ---- 重试预算：显式 > conf，**不推导** ----
  //
  // 这一条和上面的 connections_per_endpoint 故意不对称，理由是把两类旋钮分开看：
  //   * connections_per_endpoint 是**性能**旋钮：推错了只影响快慢，不影响正确性；
  //   * max_retries 是**语义**旋钮：它决定"失败之后会不会再发一次"。对非幂等调用，
  //     重试就是重复扣款。**推导错了是正确性问题，不是性能问题。**
  // 所以语义参数只允许两个来源：conf 与显式 CLI，永远不推导。
  //
  // 原先这里有一段"多 endpoint 就自动压成 0"的推导，条件还写的是 args.servers.size() > 1，
  // 于是同一份逻辑配置出现了两种行为：
  //   --servers 127.0.0.1:16161,127.0.0.1:16162   → retries=0
  //   --conf example/conf/echo_client.conf（同样这两个）→ retries=2
  // 决定"失败会不会重发"的居然是"端点怎么指定"，这正是我们反复在打的那一类
  // "看起来一样、行为是另一个"。演示需要 retries=0 时，就显式写在命令里。
  std::string retries_source;
  if (args.max_retries >= 0) {
    options.max_retries = args.max_retries;
    retries_source = "cli(--max-retries " + std::to_string(args.max_retries) + ")";
  } else if (conf_has_retries) {
    // 值已经由 FromConfig 读进来了，这里一个字节都不要动。
    retries_source = "conf(" + args.conf_path + ")";
  } else {
    retries_source = "default(ClientOptions)";
  }
  options.service_name = "demo.EchoService";
  options.caller_name = "echo_client";

  std::string err;
  if (!options.Validate(&err)) {
    std::fprintf(stderr, "客户端配置非法: %s\n", err.c_str());
    return 1;
  }

  // 两行来源日志挨在一起输出，中间不插目标信息：读的人一眼看完"这次跑的到底是什么配置"。
  // 显式给的、conf 读到的、按模式推导的，必须能分辨；演示里"杀掉一个实例仍然 20/20"
  // 那一屏全靠 max_retries=0 这一行自证——没有它，那句口播就只是断言。
  std::printf("[client] connections_per_endpoint=%zu source=%s endpoints=%llu total_conns=%d\n",
              options.connections_per_endpoint, conns_source.c_str(),
              static_cast<unsigned long long>(options.endpoints.size()),
              static_cast<int>(options.connections_per_endpoint * options.endpoints.size()));
  std::printf("[client] max_retries=%d source=%s\n", options.max_retries, retries_source.c_str());

  std::printf("目标: ");
  for (std::size_t i = 0; i < options.endpoints.size(); ++i) {
    std::printf("%s%s:%u", i == 0 ? "" : ", ", options.endpoints[i].ip.c_str(),
                static_cast<unsigned>(options.endpoints[i].port));
  }
  std::printf("  模式=%s\n", args.mode.c_str());

  // ---- 2. 建 Channel 并启动 ----
  qzrpc::rpc::Channel channel(options);
  if (!channel.Start(&err)) {
    std::fprintf(stderr, "Channel 启动失败: %s\n", err.c_str());
    return 1;
  }
  qzrpc::rpc::Channel::RegisterDefaultMetrics();
  demo::EchoServiceStub stub(channel);

  qzrpc::rpc::CallOptions call_options;
  call_options.timeout_ms = args.timeout_ms;
  call_options.server_timeout_ms = 500;

  // ---- 3. 三种模式 ----
  int exit_code = 0;

  if (args.mode == "once") {
    demo::EchoRequest request = MakeRequest("hello, qingzhou", 1);
    demo::EchoResponse response;
    const std::uint64_t begin_us = qzrpc::TimeUtil::NowUs();
    const qzrpc::Status status = stub.Echo(request, &response, call_options);
    const std::uint64_t cost_us = qzrpc::TimeUtil::NowUs() - begin_us;

    std::printf("\nstatus      : %s\n", status.ok() ? "OK" : status.ToString().c_str());
    if (status.ok()) {
      std::printf("echo        : %s\n", response.message().c_str());
      std::printf("served_by   : %d（服务端监听端口）\n", static_cast<int>(response.served_by()));
      std::printf("method_id   : 0x%08X\n", demo::EchoServiceStub::MethodIdEcho());
      std::printf("往返耗时    : %.3f ms\n", static_cast<double>(cost_us) / 1000.0);
    } else {
      exit_code = 1;
    }
  } else if (args.mode == "loop") {
    std::map<std::int32_t, std::uint64_t> tally;
    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    std::uint64_t total_us = 0;
    const std::uint64_t begin_us = qzrpc::TimeUtil::NowUs();

    for (int i = 0; i < args.count; ++i) {
      demo::EchoRequest request = MakeRequest("loop-" + std::to_string(i), i);
      demo::EchoResponse response;
      const std::uint64_t start_us = qzrpc::TimeUtil::NowUs();
      const qzrpc::Status status = stub.Echo(request, &response, call_options);
      const std::uint64_t cost_us = qzrpc::TimeUtil::NowUs() - start_us;
      if (status.ok()) {
        ++ok;
        total_us += cost_us;
        ++tally[response.served_by()];
        if (args.verbose) {
          std::printf("  [%3d] ok   %7.3f ms  served_by=%d  echo=%s\n", i,
                      static_cast<double>(cost_us) / 1000.0,
                      static_cast<int>(response.served_by()), response.message().c_str());
        }
      } else {
        ++failed;
        std::printf("  [%3d] FAIL %s\n", i, status.ToString().c_str());
      }
    }
    const std::uint64_t elapsed_us = qzrpc::TimeUtil::NowUs() - begin_us;

    std::printf("\n%llu 次调用：成功 %llu / 失败 %llu，总耗时 %.3f ms，平均 %.3f ms\n",
                static_cast<unsigned long long>(args.count),
                static_cast<unsigned long long>(ok),
                static_cast<unsigned long long>(failed),
                static_cast<double>(elapsed_us) / 1000.0,
                ok == 0 ? 0.0 : static_cast<double>(total_us) / 1000.0 / static_cast<double>(ok));
    PrintTally(tally, "分发统计");
    if (failed != 0) exit_code = 1;
  } else {  // bench
    // 每帧的固定开销：20 字节帧头 + 4 字节 metadata 长度前缀。
    // 只统计「客户端能看见的」部分（固定头 + payload）；Channel 追加的 metadata 正文
    // （trace_id / caller / timeout 等）不在这里 —— 宁可少算并注明，也不编一个假精确的数。
    const std::uint64_t kFrameOverhead = 24;

    std::atomic<std::uint64_t> ok{0};
    std::atomic<std::uint64_t> tx_bytes{0};
    std::atomic<std::uint64_t> rx_bytes{0};
    // 每个线程各收集延迟与实例分布，最后合并 —— 比全程加锁便宜得多。
    std::vector<std::vector<std::uint64_t>> latencies(static_cast<std::size_t>(args.threads));
    std::vector<std::map<std::int32_t, std::uint64_t>> tallies(
        static_cast<std::size_t>(args.threads));
    std::vector<ErrorTally> errors_per_thread(static_cast<std::size_t>(args.threads));

    std::atomic<bool> start_flag{false};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(args.threads));

    for (int t = 0; t < args.threads; ++t) {
      workers.emplace_back([&, t]() {
        const std::size_t index = static_cast<std::size_t>(t);
        latencies[index].reserve(static_cast<std::size_t>(args.count));
        std::uint64_t local_ok = 0;
        std::uint64_t local_tx = 0;
        std::uint64_t local_rx = 0;
        while (!start_flag.load(std::memory_order_acquire)) {
          qzrpc::TimeUtil::SleepMs(1);
        }
        for (int i = 0; i < args.count; ++i) {
          demo::EchoRequest request = MakeRequest("bench", i);
          demo::EchoResponse response;
          const std::uint64_t begin = qzrpc::TimeUtil::NowUs();
          const qzrpc::Status status = stub.Echo(request, &response, call_options);
          const std::uint64_t cost = qzrpc::TimeUtil::NowUs() - begin;
          // 请求侧无论成败都真的发出去了，都要计。ByteSize() 走的是
          // Writer::Counting 再跑一遍 Encode，**在计时区间之外**，对 QPS 的影响
          // 是小消息 ~1e-3 量级（200ns vs 一次往返 ~200us）。
          local_tx += kFrameOverhead + static_cast<std::uint64_t>(request.ByteSize());
          if (status.ok()) {
            ++local_ok;
            local_rx += kFrameOverhead + static_cast<std::uint64_t>(response.ByteSize());
            latencies[index].push_back(cost);
            ++tallies[index][response.served_by()];
          } else {
            errors_per_thread[index].Add(status.code());
          }
        }
        ok.fetch_add(local_ok, std::memory_order_relaxed);
        tx_bytes.fetch_add(local_tx, std::memory_order_relaxed);
        rx_bytes.fetch_add(local_rx, std::memory_order_relaxed);
      });
    }

    const std::uint64_t begin_us = qzrpc::TimeUtil::NowUs();
    start_flag.store(true, std::memory_order_release);
    for (std::thread& worker : workers) worker.join();
    const std::uint64_t elapsed_us = qzrpc::TimeUtil::NowUs() - begin_us;

    std::vector<std::uint64_t> merged;
    std::map<std::int32_t, std::uint64_t> tally;
    ErrorTally errors;
    for (std::size_t t = 0; t < latencies.size(); ++t) {
      merged.insert(merged.end(), latencies[t].begin(), latencies[t].end());
      for (const auto& item : tallies[t]) tally[item.first] += item.second;
      for (const auto& item : errors_per_thread[t].by_code) {
        errors.by_code[item.first] += item.second;
        errors.total += item.second;
      }
    }
    std::sort(merged.begin(), merged.end());

    const std::uint64_t requests =
        static_cast<std::uint64_t>(args.threads) * static_cast<std::uint64_t>(args.count);
    const std::uint64_t ok_count = ok.load();
    const double seconds = static_cast<double>(elapsed_us) / 1000000.0;
    const double qps = seconds <= 0.0 ? 0.0 : static_cast<double>(ok_count) / seconds;
    const double error_ratio = requests == 0
                                   ? 0.0
                                   : 100.0 * static_cast<double>(errors.total) /
                                         static_cast<double>(requests);
    std::uint64_t sum_us = 0;
    for (std::uint64_t value : merged) sum_us += value;
    const std::uint64_t mean_us =
        merged.empty() ? 0 : (sum_us + static_cast<std::uint64_t>(merged.size() / 2)) /
                                 static_cast<std::uint64_t>(merged.size());

    // ---- 输出：字段顺序是契约，演示脚本与日志抓取都按位置读 ----
    std::printf("\n=== bench ===\n");
    std::printf("requests   : %llu  concurrency: %d  duration: %.3fs\n",
                static_cast<unsigned long long>(requests), args.threads, seconds);
    std::printf("qps        : %.1f\n", qps);
    std::printf("latency_us : p50=%llu  p90=%llu  p95=%llu  p99=%llu  max=%llu  mean=%llu\n",
                static_cast<unsigned long long>(Percentile(&merged, 0.50)),
                static_cast<unsigned long long>(Percentile(&merged, 0.90)),
                static_cast<unsigned long long>(Percentile(&merged, 0.95)),
                static_cast<unsigned long long>(Percentile(&merged, 0.99)),
                static_cast<unsigned long long>(merged.empty() ? 0 : merged.back()),
                static_cast<unsigned long long>(mean_us));
    std::printf("errors     : %llu (%.2f%%)  overload=%llu  timeout=%llu\n",
                static_cast<unsigned long long>(errors.total), error_ratio,
                static_cast<unsigned long long>(errors.count(qzrpc::StatusCode::kOverload)),
                static_cast<unsigned long long>(errors.count(qzrpc::StatusCode::kTimeout)));
    // overload / timeout 单独成列（它们是要立刻动作的两类），其余状态码按次数降序跟在后面。
    const std::vector<std::pair<qzrpc::StatusCode, std::uint64_t>> extra = ExtraErrorCodes(errors);
    if (!extra.empty()) {
      std::printf("             also:");
      for (const auto& item : extra) {
        std::printf("  %s=%llu", qzrpc::StatusCodeName(item.first),
                    static_cast<unsigned long long>(item.second));
      }
      std::printf("\n");
    }
    std::printf("bytes      : tx=%s  rx=%s\n",
                FormatBytes(tx_bytes.load()).c_str(), FormatBytes(rx_bytes.load()).c_str());
    std::printf("served_by  : ");
    if (tally.empty()) {
      std::printf("-");
    } else {
      bool first = true;
      for (const auto& item : tally) {
        std::printf("%s%d=%llu", first ? "" : "  ", static_cast<int>(item.first),
                    static_cast<unsigned long long>(item.second));
        first = false;
      }
    }
    std::printf("\n");
    // 这一行是给"把 numbers 当线缆字节读"的人看的脚注，位置固定在最后，前面六行不受影响。
    std::printf("# bytes = payload + %lluB/frame; metadata body not counted\n",
                static_cast<unsigned long long>(kFrameOverhead));
    if (errors.total != 0) exit_code = 1;
  }

  // ---- 退出前报一次"语义计数器" ----
  //
  // retry / failover 这两个数必须和结果一起出现，否则"0 errors"是可以被重试掩盖的：
  //   * retry_total    = 业务重试次数（请求**可能已经被对端看到**之后的重发）
  //   * failover_total = 故障转移次数（一个字节都没发出去就换实例，不占 max_retries）
  // 两者都取自客户端自己的 MetricsRegistry（channel.cpp 里注册的同名计数器），进程内累计；
  // 一次运行就是一个新进程，所以这里打出来的就是"这一轮"的增量。
  // 之所以不用日志级别控制：证据行不能因为日志过滤而消失。
  {
    qzrpc::MetricsRegistry& registry = qzrpc::MetricsRegistry::Instance();
    const std::uint64_t retry_total = registry.GetCounter("qzrpc_client_retry_total")->value();
    const std::uint64_t failover_total =
        registry.GetCounter("qzrpc_client_failover_total")->value();
    // late_total：调用方已经放弃之后才到达、被就地丢弃的响应数。健康压测里它必须是 0 ——
    // 它涨了就说明确实开始超时了（late 只可能发生在一次 timeout 之后），那个事实比 QPS 重要。
    const std::uint64_t late_total =
        registry.GetCounter("qzrpc_client_late_response_total")->value();
    std::printf("[client] retry_total=%llu failover_total=%llu late_total=%llu\n",
                static_cast<unsigned long long>(retry_total),
                static_cast<unsigned long long>(failover_total),
                static_cast<unsigned long long>(late_total));

    // 借连接耗时：排队（线程抢不到池位）与服务时间变慢这两种原因，在 e2e 上长得一样，
    // 但在这个直方图上完全不同 —— 排队会让它同步变大，服务时间变慢则它基本不动。
    // 注意只报 mean/max/n，不报分位：直方图是对数分桶（2^i），Percentile() 返回桶上界，
    // p99 只可能落在 511/1023/2047… 这类格子值上，用它做比值判据是自欺。
    // n 必须打：metrics 没开时这个直方图不会被 Observe，n=0 就说明这一列不可用。
    qzrpc::Histogram* acquire_us = registry.GetHistogram("qzrpc_client_pool_acquire_us");
    std::printf("[client] pool_acquire_us : mean=%llu max=%llu n=%llu\n",
                static_cast<unsigned long long>(acquire_us->MeanUs()),
                static_cast<unsigned long long>(acquire_us->max_us()),
                static_cast<unsigned long long>(acquire_us->count()));
  }

  channel.Stop();
  return exit_code;
}
