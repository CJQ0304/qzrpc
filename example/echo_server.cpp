// 轻舟 RPC 示例：Echo 服务端
// example/echo_server.cpp
//
// 用法：
//   echo_server                                    # 内置默认值（端口 16161）
//   echo_server --conf example/conf/echo_server.conf
//   echo_server --port 16162 --metrics-port 9102   # 同一台机器起第二个实例
//
// 演示要点（这是"框架能用"的最短证据链）：
//   1. 读配置 → ServerOptions（POD，构造后只读）；
//   2. RegisterEchoService 在 Start() **之前**把方法写进路由表；
//   3. 启动后打印一行 `QZRPC_READY port=...`，让脚本/人能发现第二个实例的端口；
//   4. Ctrl-C / SIGTERM 走优雅停机，退出时打印本实例被调用次数。
//
// 关于信号处理：**不在信号处理函数里直接调 Stop()**。
// Stop() 会拿锁、会唤醒后台线程，而信号处理函数里能安全做的事极少。
// 这里用"信号置原子标志 + 看护线程轮询"的写法：处理函数只做一次 atomic store。

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>

#include "client/channel.h"
#include "common/config.h"
#include "common/logger.h"
#include "common/status.h"
#include "common/time_util.h"
#include "echo.qz.h"
#include "echo_service_impl.h"
#include "registry/registry_client_proto.h"
#include "server/server.h"
#include "server/server_options.h"

namespace {

std::atomic<bool> g_signal_seen{false};

extern "C" void OnSignal(int /*sig*/) {
  // 只做一次 store：无锁 atomic<bool> 在信号处理函数里是安全的。
  // 日志、内存分配、拿锁一律不碰。
  g_signal_seen.store(true, std::memory_order_relaxed);
}

void PrintUsage(const char* argv0) {
  std::printf(
      "轻舟 RPC Echo 服务端\n"
      "用法: %s [选项]\n"
      "  --conf <path>        读取 INI 配置（段名 [Server]）\n"
      "  --port <n>           监听端口（覆盖配置；0 = 系统分配）\n"
      "  --bind <ip>          监听地址（默认 0.0.0.0）\n"
      "  --metrics-port <n>   内建 /metrics 的 HTTP 端口（0 = 不启动）\n"
      "  --registry <ip:port> 向注册中心登记自己（不填则不注册）\n"
      "  --service <name>     注册到注册中心的服务名（默认 demo.EchoService）\n"
      "  --log-level <0..5>   0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=OFF\n"
      "  --dump-conf          打印当前生效配置后退出（可当配置模板）\n"
      "  -h, --help           显示本帮助\n"
      "\n"
      "并行演示（两个实例 + 一个客户端）：\n"
      "  echo_server --port 16161 --metrics-port 9101\n"
      "  echo_server --port 16162 --metrics-port 9102\n"
      "  echo_client --servers 127.0.0.1:16161,127.0.0.1:16162 --loop 100\n",
      argv0);
}

// 命令行里显式给出的覆盖项。
//
// 为什么不在 ParseArgs 里直接写进 ServerOptions：配置文件只能在参数解析完之后
// 才加载（`--conf` 自己的取值也要靠解析才知道），而 `FromConfig()` 会用文件里的
// 值覆盖当前字段。于是 `echo_server --conf x.conf --port 16162` 会悄悄退回
// x.conf 里的 16161 —— 两个实例撞同一个端口，第二个要么 bind 失败，要么在
// Windows 上被 SO_REUSEADDR 放行、和第一个互相抢连接（更难查）。
// 正确顺序是：先记下"用户显式给了什么"，配置加载完再盖上去。
struct CliOverrides {
  bool has_port = false;
  std::uint16_t port = 0;
  bool has_metrics_port = false;
  std::uint16_t metrics_port = 0;
  bool has_bind_ip = false;
  std::string bind_ip;
  bool has_log_level = false;
  int log_level = 0;
  bool has_registry = false;
  std::string registry_endpoint;
  bool has_service_name = false;
  std::string service_name;

  void ApplyTo(qzrpc::rpc::ServerOptions* options) const {
    if (options == nullptr) return;
    if (has_port) options->port = port;
    if (has_metrics_port) options->metrics_port = metrics_port;
    if (has_bind_ip) options->bind_ip = bind_ip;
    if (has_log_level) options->log_level = log_level;
    // 注册中心地址走 ServerOptions（Server 内建自注册），example 不再自己起线程。
    if (has_registry) options->registry_endpoint = registry_endpoint;
    if (has_service_name) options->service_name = service_name;
  }
};

bool ParseArgs(int argc, char** argv, CliOverrides* overrides, std::string* conf_path,
               bool* dump_conf, bool* show_help) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    // 取下一个参数作为取值；缺失时返回 nullptr 并已打印原因。
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "参数 %s 缺少取值\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") {
      *show_help = true;
    } else if (arg == "--dump-conf") {
      *dump_conf = true;
    } else if (arg == "--conf") {
      const char* value = next("--conf");
      if (value == nullptr) return false;
      *conf_path = value;
    } else if (arg == "--port" || arg == "--metrics-port") {
      const char* value = next(arg.c_str());
      if (value == nullptr) return false;
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed < 0 || parsed > 65535) {
        std::fprintf(stderr, "%s 必须在 0..65535，收到 %s\n", arg.c_str(), value);
        return false;
      }
      if (arg == "--port") {
        overrides->has_port = true;
        overrides->port = static_cast<std::uint16_t>(parsed);
      } else {
        overrides->has_metrics_port = true;
        overrides->metrics_port = static_cast<std::uint16_t>(parsed);
      }
    } else if (arg == "--bind") {
      const char* value = next("--bind");
      if (value == nullptr) return false;
      overrides->has_bind_ip = true;
      overrides->bind_ip = value;
    } else if (arg == "--log-level") {
      const char* value = next("--log-level");
      if (value == nullptr) return false;
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed < 0 || parsed > 5) {
        std::fprintf(stderr, "--log-level 必须在 0..5，收到 %s\n", value);
        return false;
      }
      overrides->has_log_level = true;
      overrides->log_level = static_cast<int>(parsed);
    } else if (arg == "--registry") {
      const char* value = next("--registry");
      if (value == nullptr) return false;
      overrides->has_registry = true;
      overrides->registry_endpoint = value;
    } else if (arg == "--service") {
      const char* value = next("--service");
      if (value == nullptr) return false;
      // 服务名必须与客户端查询的名字一致，否则注册中心里查不到（最常见的配置错）。
      overrides->has_service_name = true;
      overrides->service_name = value;
    } else {
      std::fprintf(stderr, "未知参数: %s（--help 看用法）\n", arg.c_str());
      return false;
    }
  }
  return true;
}

// 看护线程：轮询信号标志，置位就调 Stop()。
// 这样 Stop() 始终运行在普通线程上下文里，不需要在信号处理函数里碰锁。
void WatchForSignal(qzrpc::rpc::Server* server) {
  while (!g_signal_seen.load(std::memory_order_relaxed)) {
    qzrpc::TimeUtil::SleepMs(100);
  }
  QZ_LOG_INFO << "收到退出信号，开始优雅停机…";
  server->Stop();
}

// 关于 `--registry`：注册逻辑**不在 example 里**，而是 Server 的内建能力
// （`ServerOptions::registry_endpoint` → `server.cpp` 的 RegistryLoop）。
// 早期这里是 example 自己的一段 Registrar 旁路线程，后来上收到框架：
//   * 每个 Server 实例都该能被发现，而不是只有 echo_server 会注册；
//   * 旁路线程要自己维护 Channel/心跳/重连，和框架的注册语义迟早会漂移。
// 这个开关现在只做一件事：把地址写进配置。
}  // namespace

int main(int argc, char** argv) {
  qzrpc::rpc::ServerOptions options;
  CliOverrides overrides;
  std::string conf_path;
  bool dump_conf = false;
  bool show_help = false;

  if (!ParseArgs(argc, argv, &overrides, &conf_path, &dump_conf, &show_help)) {
    return 2;
  }
  if (show_help) {
    PrintUsage(argv[0]);
    return 0;
  }

  if (!conf_path.empty()) {
    qzrpc::Config config;
    std::string err;
    if (!config.LoadFromFile(conf_path, &err)) {
      std::fprintf(stderr, "读取配置失败 %s: %s\n", conf_path.c_str(), err.c_str());
      return 1;
    }
    // FromConfig 的约定：缺失项保留当前默认值，非法项写入 err 并返回 false。
    if (!options.FromConfig(config, "Server", &err)) {
      std::fprintf(stderr, "配置项非法: %s\n", err.c_str());
      return 1;
    }
    QZ_LOG_INFO << "已加载配置 " << conf_path;
  }

  // example 的对端（echo_client）在配置里用 service_name = demo.EchoService。
  // 不指定 --conf 时 ServerOptions 的默认服务名是通用的 "qzrpc-server"，
  // 注册进去会与客户端查询的名字对不上 —— 演示里这是最容易踩的一脚，所以这里兜底。
  if (conf_path.empty() && !overrides.has_service_name) {
    options.service_name = "demo.EchoService";
  }

  // 配置文件加载完之后再让命令行覆盖生效：`--port` 的语义是"我就要这个端口"，
  // 不能被 --conf 里的值反悔（否则并行起两个实例的演示必然撞端口）。
  overrides.ApplyTo(&options);

  if (dump_conf) {
    std::fputs(options.ToIniText().c_str(), stdout);
    return 0;
  }

  qzrpc::Logger::Instance().SetLevel(static_cast<qzrpc::Logger::Level>(options.log_level));

  std::string err;
  if (!options.Validate(&err)) {
    std::fprintf(stderr, "配置非法: %s\n", err.c_str());
    return 1;
  }

  qzrpc::rpc::Server server(options);
  if (!server.valid()) {
    std::fprintf(stderr, "Server 构造失败: %s\n", server.error_message().c_str());
    return 1;
  }

  // 内建指标要在 Start 之前注册（进程级注册表，重复调用幂等）。
  qzrpc::rpc::Server::RegisterDefaultMetrics();

  // 业务实现先构造：Server::mutable_dispatcher() 的契约是"只能在 Start() 之前调用"。
  demo::EchoServiceImpl service(options.port);
  std::unique_ptr<demo::EchoServiceDispatcher> registration =
      demo::RegisterEchoService(&service, server.mutable_dispatcher(), &err);
  if (registration == nullptr) {
    // 这里最常见的失败原因是 method_id 冲突（L1/L2 都会拦），错误信息已经把
    // 冲突的两个全名和 id 打出来了，直接透传给用户。
    std::fprintf(stderr, "注册 EchoService 失败: %s\n", err.c_str());
    return 1;
  }

  std::signal(SIGINT, OnSignal);
#ifdef SIGTERM
  std::signal(SIGTERM, OnSignal);
#endif

  if (!server.Start(&err)) {
    std::fprintf(stderr, "启动失败: %s\n", err.c_str());
    return 1;
  }

  // port = 0 时只有 Start 之后才知道实际端口；served_by 要报真实值。
  service.set_listening_port(server.port());

  // 注册中心：`--registry` / 配置里的 registry_endpoint 会被 Server 在 Start() 内
  // 用作自注册（见 server.cpp 的 RegistryLoop）。这里不需要任何额外代码 ——
  // 这正是把它从 example 上收到框架的收益。
  // 这一行是给脚本用的：demo 启动两个实例时要靠它拿到系统分配的端口。
  std::printf("QZRPC_READY service=demo.EchoService port=%u metrics_port=%u\n",
              static_cast<unsigned>(server.port()),
              static_cast<unsigned>(options.metrics_port));
  std::fflush(stdout);
  QZ_LOG_INFO << "监听 " << options.bind_ip << ":" << server.port() << "  " << options.ToString();
  if (options.metrics_port != 0) {
    QZ_LOG_INFO << "metrics: http://" << options.metrics_bind_ip << ":" << options.metrics_port
                << "/metrics";
  }

  std::thread watcher(WatchForSignal, &server);
  server.Run();  // 阻塞，直到 Stop() 被调用

  // 排空在飞请求并等线程退出；结束后 watcher 的轮询也会随标志退出，但主线程
  // 已经要退出了，先 join 它以免 detached 线程在静态析构期还在跑。
  watcher.join();
  if (!server.Wait(static_cast<int>(options.graceful_shutdown_timeout_ms))) {
    QZ_LOG_WARN << "线程未能在 " << options.graceful_shutdown_timeout_ms << "ms 内全部退出";
  }

  std::printf("QZRPC_STOPPED port=%u echo_total=%llu notify_total=%llu\n",
              static_cast<unsigned>(server.port()),
              static_cast<unsigned long long>(service.echo_total()),
              static_cast<unsigned long long>(service.notify_total()));
  return 0;
}
