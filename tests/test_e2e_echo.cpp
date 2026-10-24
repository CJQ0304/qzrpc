// 轻舟 RPC (Qingzhou RPC)
// tests/test_e2e_echo.cpp
//
// 端到端往返测试（T-11）：**同一个进程内**起一个真实 Server，再用真实 Channel 打过去。
//
// 为什么这条用例在整份交付里最值钱：
//   其余单测都是"喂字节、看字节"，它们能证明每个零件对，但证明不了"装起来还能跑"。
//   这条用例一次同时证明四件事：
//     1. 协议对：请求帧能被解析、响应帧能被组装（帧头 + metadata + payload 三层）；
//     2. 链路通：acceptor → io 线程 → worker 线程 → 写回 → 客户端拆包，全链路无断点；
//     3. seq 匹配对：并发多请求时响应会**乱序**返回，客户端必须按 seq 认领而不是按到达顺序；
//     4. 错误语义对：服务端的业务错误码原样透传，且**不被重试**。
//
// 与 example/ 的分工：example 是给人看的演示（三进程、真实端口、metrics 页），
//   这里是给 CI 看的断言（进程内、端口 0、无外部依赖）。两者不互相替代。
//
// ★ 端口用 0（系统分配），不写死 16161：
//   写死端口在 CI 上必然和"上一次没退干净的进程"打架，而这类失败是**偶发**的 ——
//   偶发红比稳定红更贵，因为它会被当成环境噪音忽略掉。Server::port() 在 Start() 之后
//   给出真实值，ClientOptions 用它构造 endpoint 即可。
//
// ★ 不睡眠等"服务端就绪"：Server::Start() 返回时监听 socket 已经 bind+listen，
//   TCP 内核队列会接住紧随其后的 connect。用 sleep 等就绪只是掩盖真正的竞态。
//
// 用例清单：
//   A. 单次往返（全部字段形态：string / 默认值字段 / packed repeated / repeated string /
//      optional / 嵌套 message / bytes / enum）
//   B. 并发多请求 —— 响应乱序但必须不串号（按 seq 匹配）
//   C. 客户端超时 + 迟到响应被丢弃（超时不能污染下一条调用）
//   D. 服务端业务错误码透传，且业务错误不进重试循环
//   E. 大载荷（跨多个 TCP 段）与二进制载荷（含 '\0'）
//   F. 停机后调用快速失败，不挂死

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client/channel.h"
#include "client/client_options.h"
#include "common/logger.h"
#include "common/status.h"
#include "echo.qz.h"
#include "server/server.h"
#include "server/server_options.h"
#include "test_main.h"

namespace {

using qzrpc::Status;
using qzrpc::StatusCode;
using qzrpc::net::Endpoint;
using qzrpc::rpc::CallOptions;
using qzrpc::rpc::Channel;
using qzrpc::rpc::ClientOptions;
using qzrpc::rpc::Server;
using qzrpc::rpc::ServerOptions;

// 每次调用的默认超时。给得比实际往返（亚毫秒级）宽两个数量级，
// 是为了让"超时用例"之外的地方不会因为机器抖动偶发失败。
constexpr int kCallTimeoutMs = 2000;

std::uint64_t NowMs() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

// ---------------------------------------------------------------------------
// 被测业务实现
//
// 刻意做成"确定性 + 可用消息内容触发特定行为"，不引入随机数、不读时钟：
// 一条用例要能重复跑出同一个结果，否则 CI 上偶发红就没人看了。
//
// 三个魔法消息值（只在本文件内有意义）：
//   "sleep"     → 业务睡 300ms（用于制造客户端超时与迟到响应）
//   "not-found" → 返回 kNotFound
//   "invalid"   → 返回 kInvalidArgument
// 其余 → 原样回显 + served_by = 监听端口
//
// 为什么字段完整性校验写在**服务端**而不是在客户端比对响应：
//   EchoResponse 只有 message / served_by 两个字段，而 IDL 是生成器的 golden 基准，
//   不能为了测试往里加字段。所以让业务实现校验它收到的请求是不是"全字段都对"，
//   不对就返回 kInvalidArgument 并说明是哪个字段 —— 测试只需断言 ok()。
//   这样"每个字段都完整过了线"这件事仍然被证明了，而且失败时能指出具体字段。
// ---------------------------------------------------------------------------
class TestEchoService final : public demo::EchoServiceBase {
 public:
  Status Echo(const demo::EchoRequest& request, demo::EchoResponse* response) override {
    echo_calls_.fetch_add(1, std::memory_order_relaxed);

    const std::string& message = request.message();
    if (message == "sleep") {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    } else if (message == "not-found") {
      return Status(StatusCode::kNotFound, "no such thing");
    } else if (message == "invalid") {
      return Status(StatusCode::kInvalidArgument, "bad argument");
    } else if (message == "rich") {
      // 期望值写死在服务端：客户端必须发出完全一致的一份请求才能通过。
      if (request.repeat() != 7) return Mismatch("repeat");
      if (request.ids_size() != 3 || request.ids(0) != 10 || request.ids(1) != 20 ||
          request.ids(2) != 30) {
        return Mismatch("ids");
      }
      if (request.tags_size() != 2 || request.tags(0) != "x" || request.tags(1) != "yy") {
        return Mismatch("tags");
      }
      if (!request.has_lang() || request.lang() != "zh-CN") return Mismatch("lang");
      if (!request.has_body()) return Mismatch("body.presence");
      if (request.body().text() != "inner") return Mismatch("body.text");
      if (request.body().weight() != -5) return Mismatch("body.weight");
      if (request.body().score() != 1.5) return Mismatch("body.score");
      if (!request.body().valid()) return Mismatch("body.valid");
      if (request.raw() != std::string("A\0B", 3)) return Mismatch("raw");
      if (request.site() != demo::SiteType::NEWS) return Mismatch("site");
      response->set_message("rich-ok");
      response->set_served_by(static_cast<std::int32_t>(port_.load(std::memory_order_relaxed)));
      return Status();
    }

    response->set_message(message);
    response->set_served_by(static_cast<std::int32_t>(port_.load(std::memory_order_relaxed)));
    return Status();
  }

  Status Notify(const demo::EchoRequest& request, demo::EchoResponse* response) override {
    notify_calls_.fetch_add(1, std::memory_order_relaxed);
    response->set_message("notify:" + request.message());
    response->set_served_by(static_cast<std::int32_t>(port_.load(std::memory_order_relaxed)));
    return Status();
  }

  void set_port(std::uint16_t port) noexcept {
    port_.store(port, std::memory_order_relaxed);
  }

  int echo_calls() const noexcept { return echo_calls_.load(std::memory_order_relaxed); }
  int notify_calls() const noexcept { return notify_calls_.load(std::memory_order_relaxed); }

 private:
  static Status Mismatch(const char* field) {
    return Status(StatusCode::kInvalidArgument, std::string("field mismatch: ") + field);
  }

  std::atomic<int> echo_calls_{0};
  std::atomic<int> notify_calls_{0};
  std::atomic<std::uint16_t> port_{0};
};

// ---------------------------------------------------------------------------
// 装置：一个真实 Server + 它的业务实现 + 注册句柄
//
// 构造顺序有意：server 先于 service 构造、析构顺序反过来 —— 注册句柄（持 service 指针）
// 必须在 service 与 server 之前销毁，否则停止过程中若还有在飞请求，就是 use-after-free。
// ---------------------------------------------------------------------------
class Rig {
 public:
  Rig() = default;
  ~Rig() { Shutdown(); }

  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  bool Start(std::string* err) {
    ServerOptions options;
    options.bind_ip = "127.0.0.1";
    options.port = 0;  // 系统分配，避免与残留进程抢端口
    options.io_thread_count = 1;
    options.worker_thread_count = 4;
    options.metrics_port = 0;                    // 单测不开 /metrics HTTP 端口
    options.log_level = qzrpc::Logger::kOff;     // 不让日志淹没 ctest 输出
    options.graceful_shutdown_timeout_ms = 2000;  // 够 "sleep" 用例排空

    // 进程级指标注册表是全局的，重复调用幂等；放在 Start 之前与 example 保持一致。
    Server::RegisterDefaultMetrics();

    server_.reset(new Server(options));
    if (!server_->valid()) {
      if (err != nullptr) *err = server_->error_message();
      return false;
    }

    service_.reset(new TestEchoService());
    registration_ = demo::RegisterEchoService(service_.get(), server_->mutable_dispatcher(), err);
    if (!registration_) return false;

    if (!server_->Start(err)) return false;
    // port=0 时只有 Start() 之后才知道真实端口；served_by 报的就是它。
    service_->set_port(server_->port());
    return true;
  }

  void Shutdown() {
    if (server_) {
      server_->Stop();
      server_->Wait(5000);
    }
    registration_.reset();
    service_.reset();
    server_.reset();
  }

  std::uint16_t port() const { return server_ ? server_->port() : 0; }
  Server* server() { return server_.get(); }
  TestEchoService* service() { return service_.get(); }

 private:
  std::unique_ptr<Server> server_;
  std::unique_ptr<TestEchoService> service_;
  std::unique_ptr<demo::EchoServiceDispatcher> registration_;
};

// 建一条指向该端口、**不重试**的 Channel。
// 重试关掉的理由：本文件的断言多数是"调用 1 次 → 服务端执行 1 次"，
// 开着重试会把"客户端重发"和"服务端重复执行"混进计数里。需要验证重试语义的用例
// 会显式传 max_retries。
std::unique_ptr<Channel> MakeChannel(std::uint16_t port, int max_retries = 0) {
  ClientOptions options;
  options.service_name = demo::EchoServiceStub::kServiceFullName;
  options.endpoints.push_back(Endpoint{"127.0.0.1", port});
  options.connect_timeout_ms = 500;
  options.request_timeout_ms = kCallTimeoutMs;
  options.acquire_timeout_ms = 500;
  options.max_retries = max_retries;
  options.retry_backoff_ms = 5;
  options.connections_per_endpoint = 2;
  options.enable_metrics = false;  // 不污染进程级指标，也不让用例之间互相影响
  return std::unique_ptr<Channel>(new Channel(options));
}

// 起装置 + 起 Channel 的样板，失败时把原因打出来（QZ_CHECK 只打表达式，不打 err）。
// 返回 false 表示已记过失败，调用方直接 return。
bool Boot(Rig* rig, std::unique_ptr<Channel>* channel, std::string* err) {
  if (!rig->Start(err)) {
    std::printf("  (server start failed: %s)\n", err->c_str());
    return false;
  }
  *channel = MakeChannel(rig->port());
  if (!(*channel)->valid()) {
    std::printf("  (client options invalid: %s)\n", (*channel)->error_message().c_str());
    return false;
  }
  if (!(*channel)->Start(err)) {
    std::printf("  (channel start failed: %s)\n", err->c_str());
    return false;
  }
  return true;
}

// ===========================================================================
// A. 单次往返：全部字段形态过一遍线
// ===========================================================================
void TestSingleCallRoundTrip() {
  Rig rig;
  std::unique_ptr<Channel> channel;
  std::string err;
  if (!Boot(&rig, &channel, &err)) {
    QZ_CHECK(false);
    return;
  }
  QZ_CHECK(rig.port() != 0);

  demo::EchoServiceStub stub(*channel);

  // A-1 最简单的回显
  {
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("hello");
    const Status status = stub.Echo(request, &response);
    QZ_CHECK(status.ok());
    QZ_CHECK_EQ(response.message(), std::string("hello"));
    // served_by 就是监听端口：这是"响应确实来自这个实例"的证据，而不是本地臆造的。
    QZ_CHECK_EQ(response.served_by(), static_cast<std::int32_t>(rig.port()));
  }

  // A-2 全字段形态（服务端逐字段校验，见 TestEchoService 的注释）
  {
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("rich");
    request.set_repeat(7);
    request.add_ids(10);
    request.add_ids(20);
    request.add_ids(30);
    request.add_tags("x");
    request.add_tags("yy");
    request.set_lang("zh-CN");
    demo::EchoBody body;
    body.set_text("inner");
    body.set_weight(-5);
    body.set_score(1.5);
    body.set_valid(true);
    request.set_body(body);
    request.set_raw(std::string("A\0B", 3));  // 二进制安全：中间有 '\0'
    request.set_site(demo::SiteType::NEWS);

    const Status status = stub.Echo(request, &response);
    QZ_CHECK(status.ok());
    QZ_CHECK_EQ(response.message(), std::string("rich-ok"));
  }

  // A-3 第二个方法（同一服务的另一条路由必须各自独立）
  {
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("ping");
    const Status status = stub.Notify(request, &response);
    QZ_CHECK(status.ok());
    QZ_CHECK_EQ(response.message(), std::string("notify:ping"));
    QZ_CHECK_EQ(rig.service()->notify_calls(), 1);
  }
}

// ===========================================================================
// B. 并发多请求：响应会乱序返回，客户端必须按 seq 认领
// ===========================================================================
//
// 这条用例的价值全在"乱"字上：单线程顺序调用时，即使客户端按到达顺序配对响应也永远
// 是对的（因为只有一条在飞）。只有并发才能把"按到达顺序配对"这个错误暴露出来 ——
// 它的症状是**偶发串号**：A 拿到 B 的结果，两边都是合法消息、都不报错，线上表现是
// "数据偶尔对不上"，归因极其困难。
//
// 每个请求携带唯一字符串（线程号 + 序号），响应必须逐字节等于它。
void TestConcurrentCallsDoNotCross() {
  Rig rig;
  std::unique_ptr<Channel> channel;
  std::string err;
  if (!Boot(&rig, &channel, &err)) {
    QZ_CHECK(false);
    return;
  }

  demo::EchoServiceStub stub(*channel);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 25;
  std::atomic<int> matched{0};
  std::atomic<int> mismatched{0};
  std::mutex report_mutex;
  std::string first_mismatch;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string expected = "t" + std::to_string(t) + "-r" + std::to_string(i);
        demo::EchoRequest request;
        demo::EchoResponse response;
        request.set_message(expected);
        const Status status = stub.Echo(request, &response);
        if (status.ok() && response.message() == expected) {
          matched.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        mismatched.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(report_mutex);
        if (first_mismatch.empty()) {
          first_mismatch = "sent '" + expected + "', got status " +
                           qzrpc::StatusCodeName(status.code()) + " / message '" +
                           response.message() + "'";
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  // QZ_CHECK 会改全局计数器，**不能**在子线程里用（会数据竞争），所以只在 join 之后断言。
  if (!first_mismatch.empty()) {
    std::printf("  (first mismatch: %s)\n", first_mismatch.c_str());
  }
  QZ_CHECK_EQ(mismatched.load(), 0);
  QZ_CHECK_EQ(matched.load(), kThreads * kPerThread);
  QZ_CHECK_EQ(rig.service()->echo_calls(), kThreads * kPerThread);

  // 连接复用：200 次调用只该占用极少量连接（每条连接是多次往返，不是一次一连接）。
  // 上限给 4 而不是精确值，因为 io 线程数与连接池实现细节都不该被这条断言锁死。
  QZ_CHECK(rig.server()->connection_count() <= static_cast<std::size_t>(4));
}

// ===========================================================================
// C. 客户端超时 + 迟到响应被丢弃
// ===========================================================================
//
// 两个断言，第二个才是这条用例的目的：
//   1) 超时必须**按时**返回（100ms 的预算不能被拖成秒级）——
//      "每次尝试都重新算一份超时"是这类框架最常见的缺陷；
//   2) 超时之后紧跟着的那次调用必须**拿到自己的结果**，而不是那条迟到的旧响应。
//      实现若按到达顺序配对（而不是按 seq），这里会拿到 message="sleep" 的响应。
//      这条路径线上表现为"偶发的莫名其妙的结果"，且只在高延迟时出现。
void TestTimeoutThenLateResponseIsDiscarded() {
  Rig rig;
  std::unique_ptr<Channel> channel;
  std::string err;
  if (!Boot(&rig, &channel, &err)) {
    QZ_CHECK(false);
    return;
  }
  demo::EchoServiceStub stub(*channel);

  // C-1 超时本身
  {
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("sleep");
    CallOptions options;
    options.timeout_ms = 100;
    options.max_retries = 0;  // 超时重试会把耗时翻倍，这里只要一次尝试

    const std::uint64_t start = NowMs();
    const Status status = stub.Echo(request, &response, options);
    const std::uint64_t elapsed = NowMs() - start;

    QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kTimeout));
    // 上界给 1500ms：业务要睡 300ms，若实现把超时当成"等完对端"就会远超这个值。
    QZ_CHECK(elapsed < 1500);
  }

  // C-2 迟到响应落在下一次调用期间：下一次调用不能被串号
  {
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("after-timeout");
    CallOptions options;
    options.timeout_ms = 1500;
    const Status status = stub.Echo(request, &response, options);
    QZ_CHECK(status.ok());
    QZ_CHECK_EQ(response.message(), std::string("after-timeout"));
  }

  // C-3 channel 必须仍然可用（超时不能让连接池进入坏状态）
  for (int i = 0; i < 3; ++i) {
    demo::EchoRequest request;
    demo::EchoResponse response;
    const std::string expected = "recover-" + std::to_string(i);
    request.set_message(expected);
    const Status status = stub.Echo(request, &response);
    QZ_CHECK(status.ok());
    QZ_CHECK_EQ(response.message(), expected);
  }
  channel->Stop();
}

// ===========================================================================
// D. 服务端业务错误码透传，且业务错误不重试
// ===========================================================================
//
// 为什么"不重试"要在这里单独钉：客户端默认 max_retries=2，如果业务错误进了重试循环，
// 同一个不合法请求会被重复执行 3 次 —— 对非幂等接口就是重复扣款。
// doc 02 §4.3 的矩阵把"业务错误永不重试"列为硬约束，这里用**服务端执行次数**来验证，
// 而不是看客户端返回码（返回码在两种情况下是一样的，看不出重试）。
//
// 注意：只断言状态码，不断言 message —— 线协议只传 16 位码（见 status.h），
// 客户端看到的文案是本地生成的，服务端那句 "no such thing" 根本不过网。
void TestServerErrorPassthroughIsNotRetried() {
  Rig rig;
  std::unique_ptr<Channel> channel;
  std::string err;
  if (!Boot(&rig, &channel, &err)) {
    QZ_CHECK(false);
    return;
  }
  channel->Stop();
  // 换一条开着重试的 Channel：默认 2 次重试正是要验证的东西。
  channel = MakeChannel(rig.port(), 2);
  QZ_CHECK(channel->Start(&err));
  demo::EchoServiceStub stub(*channel);

  {
    const int before = rig.service()->echo_calls();
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("not-found");
    const Status status = stub.Echo(request, &response);
    QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kNotFound));
    QZ_CHECK_EQ(rig.service()->echo_calls() - before, 1);  // 只执行一次 = 没重试
  }
  {
    const int before = rig.service()->echo_calls();
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("invalid");
    CallOptions options;
    options.max_retries = 5;  // 逐次覆盖也不能让业务错误被重试
    const Status status = stub.Echo(request, &response, options);
    QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kInvalidArgument));
    QZ_CHECK_EQ(rig.service()->echo_calls() - before, 1);
  }
}

// ===========================================================================
// E. 大载荷与二进制载荷
// ===========================================================================
//
// 300 KiB 远大于单次 TCP 段的承载量（loopback MTU 通常 64 KiB），所以这条用例真正
// 覆盖的是**服务端与客户端的拆包/组包**：一帧被切成多个 read 事件，两端都必须能
// 正确重组，而不是"读到一半就当完整帧处理"。
void TestLargeAndBinaryPayload() {
  Rig rig;
  std::unique_ptr<Channel> channel;
  std::string err;
  if (!Boot(&rig, &channel, &err)) {
    QZ_CHECK(false);
    return;
  }
  demo::EchoServiceStub stub(*channel);

  // E-1 300 KiB 可打印字符（内容可校验，出错时能看出是从哪里断的）
  {
    std::string big;
    big.reserve(300 * 1024);
    for (std::size_t i = 0; i < 300 * 1024; ++i) {
      big.push_back(static_cast<char>('a' + (i % 26)));
    }
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message(big);
    const Status status = stub.Echo(request, &response);
    QZ_CHECK(status.ok());
    QZ_CHECK_EQ(response.message().size(), big.size());
    QZ_CHECK(response.message() == big);
  }

  // E-2 二进制：含 '\0' 与 UTF-8 多字节序列。字符串长度必须按字节算，
  //     任何一处用了 strlen 都会在这里断掉。
  {
    std::string binary("head\0mid", 8);
    binary.append("\xE4\xBD\xA0\xE5\xA5\xBD");  // "你好"
    binary.append("\0tail", 5);
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message(binary);
    const Status status = stub.Echo(request, &response);
    QZ_CHECK(status.ok());
    QZ_CHECK_EQ(response.message().size(), binary.size());
    QZ_CHECK(response.message() == binary);
  }
}

// ===========================================================================
// F. 停机后调用必须快速失败，不能挂死
// ===========================================================================
//
// 判据刻意宽松（只要求"非 ok 且在预算内返回"）：停机瞬间客户端可能拿到
// kServiceUnavailable（连不上）、kCancelled（连接被关）、kTimeout（写出去但没人回），
// 三种都合理，锁死一种等于把实现细节写进契约。
// 真正必须成立的是**耗时有界**：调用方不能因为服务端走了就无限等下去。
void TestCallAfterStopFailsFast() {
  Rig rig;
  std::unique_ptr<Channel> channel;
  std::string err;
  if (!Boot(&rig, &channel, &err)) {
    QZ_CHECK(false);
    return;
  }
  demo::EchoServiceStub stub(*channel);

  // 先确认一条正常调用成功（避免"本来就没通"的情况下这条用例假绿）
  {
    demo::EchoRequest request;
    demo::EchoResponse response;
    request.set_message("before-stop");
    QZ_CHECK(stub.Echo(request, &response).ok());
  }

  rig.Shutdown();

  demo::EchoRequest request;
  demo::EchoResponse response;
  request.set_message("after-stop");
  CallOptions options;
  options.timeout_ms = 300;
  options.max_retries = 0;

  const std::uint64_t start = NowMs();
  const Status status = stub.Echo(request, &response, options);
  const std::uint64_t elapsed = NowMs() - start;

  QZ_CHECK(!status.ok());
  QZ_CHECK(elapsed < 2000);  // 有界：300ms 预算 + 允许一次建连尝试
  const StatusCode code = status.code();
  const bool plausible = code == StatusCode::kServiceUnavailable ||
                         code == StatusCode::kCancelled || code == StatusCode::kTimeout;
  if (!plausible) {
    std::printf("  (unexpected status after stop: %s)\n", qzrpc::StatusCodeName(code));
  }
  QZ_CHECK(plausible);

  channel->Stop();
}

}  // namespace

int main() {
  // 每个用例开跑前打一行并 flush。
  //
  // 为什么值得占这几行：进程内起真实 Server/Channel 的用例，**卡住**是一种真实的失败形态
  // （等一个永远不来的响应、join 一个不退出的 io 线程）。没有这行输出时，"ctest 超时"
  // 只告诉你"这个文件挂了"，不告诉你是哪一条；有了它，卡住时最后一行就是凶手。
  // 成功路径上 ctest 不展示 stdout，所以不留噪音。
  auto announce = [](const char* name) {
    std::printf("== %s\n", name);
    std::fflush(stdout);
  };

  announce("TestSingleCallRoundTrip");
  TestSingleCallRoundTrip();
  announce("TestConcurrentCallsDoNotCross");
  TestConcurrentCallsDoNotCross();
  announce("TestTimeoutThenLateResponseIsDiscarded");
  TestTimeoutThenLateResponseIsDiscarded();
  announce("TestServerErrorPassthroughIsNotRetried");
  TestServerErrorPassthroughIsNotRetried();
  announce("TestLargeAndBinaryPayload");
  TestLargeAndBinaryPayload();
  announce("TestCallAfterStopFailsFast");
  TestCallAfterStopFailsFast();
  return qztest::Summary("test_e2e_echo");
}
