// 轻舟 RPC 示例：内置注册中心
// example/registry_main.cpp
//
// 这个进程的存在意义是"框架自举"：注册中心本身就是一个 qzrpc::rpc::Server，
// 只是把 Register / Heartbeat / Fetch / Deregister 四个方法注册进去，再加一个
// TTL 清扫线程。它跑得通，说明框架的 Server + Dispatcher + 序列化是自洽的。
//
// 用法：
//   registry_main                                  # 默认 0.0.0.0:16162
//   registry_main --conf example/conf/registry.conf
//   registry_main --port 0                         # 系统分配，启动后打印实际端口

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>
#include <thread>

#include "common/logger.h"
#include "common/time_util.h"
#include "registry/registry_server.h"
#include "server/server_options.h"

namespace {

std::atomic<bool> g_signal_seen{false};

extern "C" void OnSignal(int /*sig*/) {
  g_signal_seen.store(true, std::memory_order_relaxed);
}

void PrintUsage(const char* argv0) {
  std::printf(
      "轻舟 RPC 注册中心\n"
      "用法: %s [选项]\n"
      "  --conf <path>       读取 INI 配置（[Server] 与 [Registry] 两段）\n"
      "  --port <n>          监听端口（默认 %u；0 = 系统分配）\n"
      "  --ttl <ms>          实例租约时长（默认 15000）\n"
      "  --sweep <ms>        清扫线程周期（默认 3000）\n"
      "  --log-level <0..5>  日志级别\n"
      "  -h, --help          显示本帮助\n",
      argv0, static_cast<unsigned>(qzrpc::protocol::kDefaultRegistryPort));
}

}  // namespace

int main(int argc, char** argv) {
  qzrpc::registry::RegistryServerOptions options;
  options.server.port = qzrpc::protocol::kDefaultRegistryPort;
  std::string conf_path;
  bool show_help = false;

  // 命令行覆盖项：**必须**等到 --conf 加载完之后再生效。
  // FromConfig() 会用文件里的 port / log_level 覆盖当前字段，[Registry] 段同理；
  // 若在解析参数时直接写 options，`--conf x.conf --port 16160` 会被 x.conf 里的
  // 16162 反悔，脚本以为自己在起 16160 的注册中心、实际起在 16162。
  bool has_port = false;
  std::uint16_t cli_port = 0;
  bool has_ttl = false;
  int cli_ttl = 0;
  bool has_sweep = false;
  int cli_sweep = 0;
  bool has_log_level = false;
  int cli_log_level = 0;

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
      show_help = true;
    } else if (arg == "--conf") {
      const char* value = next("--conf");
      if (value == nullptr) return 2;
      conf_path = value;
    } else if (arg == "--port") {
      const char* value = next("--port");
      if (value == nullptr) return 2;
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed < 0 || parsed > 65535) {
        std::fprintf(stderr, "--port 必须在 0..65535\n");
        return 2;
      }
      has_port = true;
      cli_port = static_cast<std::uint16_t>(parsed);
    } else if (arg == "--ttl") {
      const char* value = next("--ttl");
      if (value == nullptr) return 2;
      has_ttl = true;
      cli_ttl = static_cast<int>(std::strtol(value, nullptr, 10));
    } else if (arg == "--sweep") {
      const char* value = next("--sweep");
      if (value == nullptr) return 2;
      has_sweep = true;
      cli_sweep = static_cast<int>(std::strtol(value, nullptr, 10));
    } else if (arg == "--log-level") {
      const char* value = next("--log-level");
      if (value == nullptr) return 2;
      has_log_level = true;
      cli_log_level = static_cast<int>(std::strtol(value, nullptr, 10));
    } else {
      std::fprintf(stderr, "未知参数: %s（--help 看用法）\n", arg.c_str());
      return 2;
    }
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
    if (!options.server.FromConfig(config, "Server", &err)) {
      std::fprintf(stderr, "[Server] 段配置非法: %s\n", err.c_str());
      return 1;
    }
    // [Registry] 段是本进程独有的两项：租约时长与清扫周期。
    if (config.Has("Registry", "instance_ttl_ms")) {
      options.instance_ttl_ms = static_cast<int>(config.GetInt("Registry", "instance_ttl_ms", 15000));
    }
    if (config.Has("Registry", "sweep_interval_ms")) {
      options.sweep_interval_ms =
          static_cast<int>(config.GetInt("Registry", "sweep_interval_ms", 3000));
    }
    if (config.Has("Registry", "allow_overwrite")) {
      options.allow_overwrite = config.GetBool("Registry", "allow_overwrite", true);
    }
    QZ_LOG_INFO << "已加载配置 " << conf_path;
  }

  // 配置加载完再应用命令行覆盖（理由见 main 开头的注释）。
  if (has_port) options.server.port = cli_port;
  if (has_ttl) options.instance_ttl_ms = cli_ttl;
  if (has_sweep) options.sweep_interval_ms = cli_sweep;
  if (has_log_level) options.server.log_level = cli_log_level;

  qzrpc::Logger::Instance().SetLevel(static_cast<qzrpc::Logger::Level>(options.server.log_level));

  {
    std::string err;
    if (!options.Validate(&err)) {
      std::fprintf(stderr, "配置非法: %s\n", err.c_str());
      return 1;
    }
  }

  qzrpc::registry::RegistryServer server(options);
  if (!server.valid()) {
    std::fprintf(stderr, "RegistryServer 构造失败: %s\n", server.error_message().c_str());
    return 1;
  }

  std::signal(SIGINT, OnSignal);
#ifdef SIGTERM
  std::signal(SIGTERM, OnSignal);
#endif

  std::string err;
  if (!server.Start(&err)) {
    std::fprintf(stderr, "启动失败: %s\n", err.c_str());
    return 1;
  }

  // 给脚本用的一行（与 echo_server 同风格），便于 e2e 脚本等注册中心就绪。
  std::printf("QZRPC_READY service=%s port=%u\n", qzrpc::registry::kRegistryServiceFullName,
              static_cast<unsigned>(server.port()));
  std::fflush(stdout);

  // 主循环：每 3 秒打一行活跃度，信号到来就退出。
  // 这里不用 Run()，因为 Run() 只做 wait，而我们需要周期性地把"服务/实例数"
  // 打印出来——注册中心的健康状态就是它的全部价值，静默跑着等于没有。
  while (!g_signal_seen.load(std::memory_order_relaxed)) {
    qzrpc::TimeUtil::SleepMs(3000);
    if (g_signal_seen.load(std::memory_order_relaxed)) break;
    QZ_LOG_INFO << "注册中心: 服务 " << server.service_count() << " 个 / 实例 "
                << server.instance_count() << " 个"
                << "（register=" << server.register_total()
                << " heartbeat=" << server.heartbeat_total()
                << " fetch=" << server.fetch_total() << " expired=" << server.expired_total()
                << "）";
  }

  server.Stop();
  if (!server.running()) {
    std::printf("QZRPC_STOPPED port=%u services=%llu instances=%llu\n",
                static_cast<unsigned>(server.port()),
                static_cast<unsigned long long>(server.service_count()),
                static_cast<unsigned long long>(server.instance_count()));
  }
  return 0;
}
