// 轻舟 RPC (Qingzhou RPC)
// tests/test_dispatcher.cpp
//
// 执行期路由表（Dispatcher）与调用上下文（ServerContext）的契约测试（T-10）。
//
// 为什么这两块要一起测：
//   Dispatcher 决定"这个 method_id 由谁来执行"，ServerContext 决定"执行者看到什么"。
//   它们合起来才是"请求被正确投递"这件事。错了的形态都很安静：
//     * 注册冲突被放过 → 后注册的方法悄悄顶掉先注册的，线上表现为"某个接口调到了另一个实现"；
//     * Find 未命中却改写了 *full_name → 日志里出现"未注册的方法却有个像样的名字"，排障被误导；
//     * deadline 判定反了（提前判超时 / 永不判超时）→ 要么白白丢弃本可完成的请求，
//       要么业务跑到底才发现调用方早走了，白烧 CPU。
//
// 三条重点路径（对应验收要求）：
//   1) method_id 未知 —— Find 返回 nullptr、fallback 生效或框架回 kNotFound；
//   2) payload 截断 —— 处理器解码失败回 kSerializeError，**状态原样透传**，不被框架改写；
//   3) 处理器返回错误码 —— 非 ok 时框架不使用 out_payload（否则会把半截响应发出去）。

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/status.h"
#include "common/time_util.h"
#include "protocol/frame.h"
#include "serialize/reader.h"
#include "server/dispatcher.h"
#include "test_main.h"

namespace {

using qzrpc::Status;
using qzrpc::StatusCode;
using qzrpc::protocol::Metadata;
using qzrpc::rpc::Dispatcher;
using qzrpc::rpc::MethodHandler;
using qzrpc::rpc::ServerContext;

// demo.EchoService.Echo 的 method_id（与 codegen 的常量一致，见 test_codec 的 golden）
constexpr std::uint32_t kEchoId = 0x21678893U;
constexpr std::uint32_t kNotifyId = 0x9AF18CAFU;
constexpr std::uint32_t kMetricsId = 0x11111111U;  // 仅本文件使用的任意值

Status EchoHandler(const char* payload, std::size_t payload_len, std::string* out_payload,
                   ServerContext* ctx) {
  (void)ctx;
  if (out_payload != nullptr) {
    out_payload->assign(payload != nullptr ? payload : "", payload_len);
  }
  return Status();
}

// ===========================================================================
// A. 注册：拒绝规则与幂等
// ===========================================================================

void TestRegisterRejectsInvalidInput() {
  Dispatcher dispatcher;
  std::string err;

  // 空 handler：注册了也执行不了，入口就拒。
  err.clear();
  QZ_CHECK(!dispatcher.Register(kEchoId, "demo.EchoService.Echo", MethodHandler(), &err));
  QZ_CHECK(!err.empty());
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{0});

  // method_id 0：全框架的"未指定"哨兵，注册上去会在路由表里留一个能被误命中的洞。
  err.clear();
  QZ_CHECK(!dispatcher.Register(0, "demo.Bad.Method", EchoHandler, &err));
  QZ_CHECK(err.find("reserved") != std::string::npos);
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{0});

  // 同一 id + 同一全名：**幂等成功**，且不覆盖已有 handler。
  // 这条在多份生成代码各自调 RegisterXxxService 时是正常路径，不是错误。
  int first_calls = 0;
  int second_calls = 0;
  QZ_CHECK(dispatcher.Register(kEchoId, "demo.EchoService.Echo",
                               [&first_calls](const char*, std::size_t, std::string*,
                                              ServerContext*) {
                                 ++first_calls;
                                 return Status();
                               },
                               &err));
  QZ_CHECK(dispatcher.Register(kEchoId, "demo.EchoService.Echo",
                               [&second_calls](const char*, std::size_t, std::string*,
                                               ServerContext*) {
                                 ++second_calls;
                                 return Status();
                               },
                               &err));
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{1});
  const MethodHandler* handler = dispatcher.Find(kEchoId);
  QZ_CHECK(handler != nullptr);
  if (handler != nullptr) {
    std::string out;
    ServerContext ctx;
    (*handler)("", 0, &out, &ctx);
  }
  QZ_CHECK_EQ(first_calls, 1);   // 先注册的赢
  QZ_CHECK_EQ(second_calls, 0);  // 后来的被忽略，而不是悄悄顶掉

  // 同一 id + 不同全名：**哈希冲突**，必须失败并把两个名字都报出来。
  // 这是要人工介入的问题（改 IDL 或换包名），不能让服务静默起来。
  err.clear();
  QZ_CHECK(!dispatcher.Register(kEchoId, "demo.OtherService.Other", EchoHandler, &err));
  QZ_CHECK(err.find("demo.EchoService.Echo") != std::string::npos);
  QZ_CHECK(err.find("demo.OtherService.Other") != std::string::npos);
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{1});
}

// ===========================================================================
// B. 查找：命中与未命中
// ===========================================================================

void TestFindSemantics() {
  Dispatcher dispatcher;
  QZ_CHECK(dispatcher.Register(kEchoId, "demo.EchoService.Echo", EchoHandler));
  QZ_CHECK(dispatcher.Register(kNotifyId, "demo.EchoService.Notify", EchoHandler));

  const char* name = nullptr;
  QZ_CHECK(dispatcher.Find(kEchoId, &name) != nullptr);
  QZ_CHECK(name != nullptr);
  if (name != nullptr) QZ_CHECK_EQ(std::string(name), std::string("demo.EchoService.Echo"));

  // 未命中：返回 nullptr，且 **不修改** *full_name。
  // 这条看起来很细，但它是"日志里把上一个方法的名字贴到这次未命中上"的唯一防线。
  const char* untouched = "sentinel";
  QZ_CHECK(dispatcher.Find(kMetricsId, &untouched) == nullptr);
  QZ_CHECK_EQ(std::string(untouched), std::string("sentinel"));

  // full_name 传 nullptr：不能崩（内部要跳过写）。
  QZ_CHECK(dispatcher.Find(kMetricsId, nullptr) == nullptr);
  QZ_CHECK(dispatcher.Find(kEchoId, nullptr) != nullptr);

  // 未注册任何东西的 Dispatcher 也是合法的（不是错误状态）。
  Dispatcher empty;
  QZ_CHECK_EQ(empty.size(), std::size_t{0});
  QZ_CHECK(empty.Find(kEchoId) == nullptr);
  QZ_CHECK(empty.method_ids().empty());
}

// ===========================================================================
// C. 调用：payload 原样进出、ctx 字段正确、状态码原样透传
// ===========================================================================

void TestHandlerInvocationAndContext() {
  Dispatcher dispatcher;

  std::string seen_payload;
  std::size_t seen_len = 0;
  std::uint32_t seen_seq = 0;
  std::uint32_t seen_method = 0;
  std::string seen_full_name;
  std::string seen_peer;
  bool metadata_non_null = false;

  QZ_CHECK(dispatcher.Register(
      kEchoId, "demo.EchoService.Echo",
      [&](const char* payload, std::size_t payload_len, std::string* out_payload,
          ServerContext* ctx) {
        seen_payload.assign(payload, payload_len);
        seen_len = payload_len;
        seen_seq = ctx->seq;
        seen_method = ctx->method_id;
        seen_full_name = ctx->method_full_name != nullptr ? ctx->method_full_name : "<null>";
        seen_peer = ctx->peer_addr;
        metadata_non_null = ctx->metadata != nullptr;
        if (out_payload != nullptr) out_payload->assign("pong");
        return Status();
      }));

  const char* full_name = nullptr;
  const MethodHandler* handler = dispatcher.Find(kEchoId, &full_name);
  QZ_CHECK(handler != nullptr);
  if (handler == nullptr) return;

  Metadata metadata;
  metadata.SetTraceId("trace-1");
  ServerContext ctx;
  ctx.seq = 42;
  ctx.method_id = kEchoId;
  ctx.method_full_name = full_name;
  ctx.peer_addr = "127.0.0.1:55555";
  ctx.metadata = &metadata;
  ctx.start_ms = qzrpc::TimeUtil::NowMs();

  // payload 含 '\0'：长度必须按字节算（任何 strlen 都会在这里断掉）。
  const std::string payload("a\0b", 3);
  std::string response;
  const Status status = (*handler)(payload.data(), payload.size(), &response, &ctx);

  QZ_CHECK(status.ok());
  QZ_CHECK_EQ(seen_len, std::size_t{3});
  QZ_CHECK(seen_payload == payload);
  QZ_CHECK_EQ(seen_seq, 42U);
  QZ_CHECK_EQ(seen_method, kEchoId);
  QZ_CHECK_EQ(seen_full_name, std::string("demo.EchoService.Echo"));
  QZ_CHECK_EQ(seen_peer, std::string("127.0.0.1:55555"));
  QZ_CHECK(metadata_non_null);
  QZ_CHECK_EQ(response, std::string("pong"));

  // 空 payload（payload == nullptr 且 len == 0）：框架允许，handler 必须自己判空。
  std::string empty_out;
  QZ_CHECK((*handler)(nullptr, 0, &empty_out, &ctx).ok());
  QZ_CHECK_EQ(seen_len, std::size_t{0});

  // out_payload 传 nullptr：表示"不关心响应体"，不能崩（客户端不读响应的场景）。
  QZ_CHECK((*handler)("x", 1, nullptr, &ctx).ok());
}

// 处理器返回非 ok：状态码必须**原样**透传到调用方（框架不许改写、不许加壳），
// 且 out_payload 的内容不该被当成有效响应使用。
void TestHandlerErrorStatusPassesThrough() {
  Dispatcher dispatcher;
  std::string last_out;

  QZ_CHECK(dispatcher.Register(kEchoId, "demo.EchoService.Echo",
                               [&last_out](const char*, std::size_t, std::string* out_payload,
                                           ServerContext*) {
                                 // 典型的手写错误：先往 out_payload 写了半截，再返回错误。
                                 // 框架的契约是"非 ok 时忽略 out_payload"，
                                 // 所以这段内容绝不能变成线上响应。
                                 if (out_payload != nullptr) out_payload->assign("half-written");
                                 last_out = "half-written";
                                 return Status(StatusCode::kInvalidArgument, "bad arg");
                               }));

  const MethodHandler* handler = dispatcher.Find(kEchoId);
  QZ_CHECK(handler != nullptr);
  if (handler == nullptr) return;

  ServerContext ctx;
  std::string response;
  const Status status = (*handler)("payload", 7, &response, &ctx);

  QZ_CHECK(!status.ok());
  QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kInvalidArgument));
  // 状态码是 16 位线上的唯一载体；message 只用于日志，不上线。
  QZ_CHECK(!status.message().empty());

  // 各种状态码都不能被框架"顺手改成 kInternal"或"改成 ok"。
  const StatusCode codes[] = {StatusCode::kNotFound,      StatusCode::kServiceUnavailable,
                              StatusCode::kTimeout,       StatusCode::kOverload,
                              StatusCode::kInternal,      StatusCode::kSerializeError,
                              StatusCode::kCancelled};
  for (StatusCode code : codes) {
    Dispatcher fresh;
    QZ_CHECK(fresh.Register(kEchoId, "demo.EchoService.Echo",
                            [code](const char*, std::size_t, std::string*, ServerContext*) {
                              return Status(code, "x");
                            }));
    const MethodHandler* h = fresh.Find(kEchoId);
    QZ_CHECK(h != nullptr);
    if (h == nullptr) continue;
    std::string out;
    ServerContext c;
    const Status s = (*h)("", 0, &out, &c);
    QZ_CHECK_EQ(static_cast<int>(s.code()), static_cast<int>(code));
  }
}

// payload 截断：处理器解码失败 → 回 kSerializeError。
// 这里用真实的 Reader 模拟生成代码的解码路径：截断的 varint 必须被 Reader 判为失败，
// 而不是"读到一个偏小的值继续跑"（后者是静默的数据损坏）。
void TestTruncatedPayloadFailsDecode() {
  Dispatcher dispatcher;

  QZ_CHECK(dispatcher.Register(
      kEchoId, "demo.EchoService.Echo",
      [](const char* payload, std::size_t payload_len, std::string* out_payload,
         ServerContext*) {
        // 生成代码就是这么干的：Reader 覆盖恰好 payload_len 字节，解析失败回 kSerializeError。
        qzrpc::serde::Reader reader(payload, payload_len);
        std::uint32_t field = 0;
        std::uint8_t wire = 0;
        if (!reader.ReadTag(&field, &wire)) {
          return Status(StatusCode::kSerializeError, "truncated: cannot read tag");
        }
        std::string text;
        if (wire == 2 && !reader.ReadString(&text)) {
          return Status(StatusCode::kSerializeError, "truncated: cannot read string body");
        }
        if (out_payload != nullptr) out_payload->assign(text);
        return Status();
      }));

  const MethodHandler* handler = dispatcher.Find(kEchoId);
  QZ_CHECK(handler != nullptr);
  if (handler == nullptr) return;
  ServerContext ctx;

  // 完整的一条 string 字段：0x0A len "hi"
  {
    const std::string full("\x0A\x02hi", 4);
    std::string out;
    QZ_CHECK((*handler)(full.data(), full.size(), &out, &ctx).ok());
    QZ_CHECK_EQ(out, std::string("hi"));
  }
  // 声明长度 2 却只给了 1 个字节
  {
    const std::string truncated("\x0A\x02h", 3);
    std::string out;
    const Status status = (*handler)(truncated.data(), truncated.size(), &out, &ctx);
    QZ_CHECK(!status.ok());
    QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kSerializeError));
  }
  // 只剩半个 tag（varint 被切断）
  {
    const std::string half_tag("\x80", 1);
    std::string out;
    const Status status = (*handler)(half_tag.data(), half_tag.size(), &out, &ctx);
    QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kSerializeError));
  }
  // 空 payload：Reader 直接判定结束，ReadTag 失败（一个字段都没有不是"成功"）。
  {
    std::string out;
    const Status status = (*handler)(nullptr, 0, &out, &ctx);
    QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kSerializeError));
  }
}

// ===========================================================================
// D. fallback：未注册方法的兜底
// ===========================================================================

void TestFallbackHandler() {
  Dispatcher dispatcher;
  QZ_CHECK(dispatcher.fallback() == nullptr);  // 默认没有兜底

  std::atomic<int> fallback_calls{0};
  std::uint32_t seen_method = 0;
  dispatcher.SetFallback([&](const char*, std::size_t, std::string*, ServerContext* ctx) {
    fallback_calls.fetch_add(1);
    seen_method = ctx->method_id;
    return Status(StatusCode::kNotFound, "no such method");
  });
  QZ_CHECK(dispatcher.fallback() != nullptr);

  // 兜底不改变 Find 的语义：未命中的 id 依然返回 nullptr，
  // 由调用方（server.cpp）决定"没找到就用 fallback"。
  QZ_CHECK(dispatcher.Find(kMetricsId) == nullptr);
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{0});

  const MethodHandler* fb = dispatcher.fallback();
  std::string out;
  ServerContext ctx;
  ctx.method_id = kMetricsId;
  const Status status = (*fb)("", 0, &out, &ctx);
  QZ_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::kNotFound));
  QZ_CHECK_EQ(fallback_calls.load(), 1);
  QZ_CHECK_EQ(seen_method, kMetricsId);

  // 已注册的方法不受兜底影响。
  QZ_CHECK(dispatcher.Register(kEchoId, "demo.EchoService.Echo", EchoHandler));
  QZ_CHECK(dispatcher.Find(kEchoId) != nullptr);
}

// ===========================================================================
// E. method_ids / size / Clear
// ===========================================================================

void TestMethodIdsAreSorted() {
  Dispatcher dispatcher;
  // 故意乱序注册。
  QZ_CHECK(dispatcher.Register(0x30000000U, "c.C.c", EchoHandler));
  QZ_CHECK(dispatcher.Register(kEchoId, "demo.EchoService.Echo", EchoHandler));
  QZ_CHECK(dispatcher.Register(0x00000001U, "a.A.a", EchoHandler));
  QZ_CHECK(dispatcher.Register(kNotifyId, "demo.EchoService.Notify", EchoHandler));

  const std::vector<std::uint32_t> ids = dispatcher.method_ids();
  QZ_CHECK_EQ(ids.size(), std::size_t{4});
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{4});
  // 升序是契约（日志、启动期自检、/debug 页面都按它输出，便于 diff）。
  std::vector<std::uint32_t> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  QZ_CHECK(ids == sorted);

  // Clear 是"仅测试用"的：连 fallback 一起清空。
  dispatcher.SetFallback([](const char*, std::size_t, std::string*, ServerContext*) {
    return Status();
  });
  dispatcher.Clear();
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{0});
  QZ_CHECK(dispatcher.method_ids().empty());
  QZ_CHECK(dispatcher.fallback() == nullptr);
  QZ_CHECK(dispatcher.Find(kEchoId) == nullptr);
}

// 契约：Register 与 Find 可跨线程并发（读写锁）。
// 注册完成后 Find 是纯读，这里验证"注册还在进行时读也不会崩、不会读到半截名字"。
// 注意：QZ_CHECK 不能进子线程（全局计数非原子），所以子线程只记局部计数。
void TestConcurrentRegisterAndFind() {
  Dispatcher dispatcher;
  QZ_CHECK(dispatcher.Register(kEchoId, "demo.EchoService.Echo", EchoHandler));

  std::atomic<bool> stop{false};
  std::atomic<int> bad_reads{0};
  std::atomic<long long> hits{0};
  std::atomic<int> readers_started{0};

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&dispatcher, &stop, &bad_reads, &hits, &readers_started] {
      readers_started.fetch_add(1, std::memory_order_relaxed);
      // do/while 而不是 while：保证每个线程**至少**读一次，
      // 否则主线程可能在读者还没被调度时就结束注册并置 stop，这条用例会以
      // "hits == 0" 的形式偶发变红 —— 那不是被测代码的问题，是测试自己没制造出并发。
      do {
        const char* name = nullptr;
        const MethodHandler* handler = dispatcher.Find(kEchoId, &name);
        if (handler == nullptr) {
          bad_reads.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (name == nullptr || std::strcmp(name, "demo.EchoService.Echo") != 0) {
          bad_reads.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        hits.fetch_add(1, std::memory_order_relaxed);
      } while (!stop.load(std::memory_order_relaxed));
    });
  }

  // 等四个读者都真的跑起来，再开始并发注册（否则测的是"串行注册 + 之后才读"）。
  while (readers_started.load(std::memory_order_relaxed) < 4) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  for (std::uint32_t i = 0; i < 500; ++i) {
    const std::string name = "demo.Load.Method" + std::to_string(i);
    if (!dispatcher.Register(0x10000000U + i, name, EchoHandler)) {
      bad_reads.fetch_add(1, std::memory_order_relaxed);
    }
  }
  stop.store(true);
  for (std::thread& thread : readers) thread.join();

  QZ_CHECK_EQ(bad_reads.load(), 0);
  // 每个读者至少读完一次 -> hits 至少 4。
  QZ_CHECK(hits.load() >= 4);
  QZ_CHECK_EQ(dispatcher.size(), std::size_t{501});
}

// ===========================================================================
// F. ServerContext::deadline_exceeded
// ===========================================================================

// 判定规则（实现里写死的优先级）：绝对 deadline > 相对 timeout > 不判。
// 方向反了的后果是两个极端：要么把还能干完的活提前扔掉，要么让早走的调用把 CPU 烧到底。
void TestDeadlineExceeded() {
  ServerContext ctx;
  ctx.start_ms = 1000;

  // 没有 metadata：不判超时（不能因为缺信息就把请求判死）。
  QZ_CHECK(!ctx.deadline_exceeded(999999));

  Metadata metadata;
  ctx.metadata = &metadata;

  // metadata 里既没 deadline 也没 timeout：不判超时。
  QZ_CHECK(!ctx.deadline_exceeded(999999));

  // 相对 timeout = 100ms，已过 50ms -> 没超；已过 150ms -> 超。
  metadata.SetTimeoutMs(100);
  QZ_CHECK(!ctx.deadline_exceeded(1050));
  QZ_CHECK(ctx.deadline_exceeded(1150));
  // 边界：正好等于 timeout 不算超（用 > 而不是 >=，给业务留满整个预算）。
  QZ_CHECK(!ctx.deadline_exceeded(1100));

  // timeout == 0 表示"调用方没声明时限"，不是"立刻超时"。
  Metadata zero_timeout;
  zero_timeout.SetTimeoutMs(0);
  ctx.metadata = &zero_timeout;
  QZ_CHECK(!ctx.deadline_exceeded(999999));

  // 时钟异常：now < start（单调时钟不会倒退，但参数是调用方传的）-> 不误判。
  ctx.metadata = &metadata;
  QZ_CHECK(!ctx.deadline_exceeded(900));

  // 绝对 deadline：已经过去 -> 立刻判超时。
  Metadata absolute;
  const std::uint64_t now_unix = qzrpc::TimeUtil::UnixMs();
  absolute.SetDeadlineUnixMs(now_unix - 1000);  // 1 秒前就该结束
  ctx.metadata = &absolute;
  ctx.start_ms = qzrpc::TimeUtil::NowMs();
  QZ_CHECK(ctx.deadline_exceeded(qzrpc::TimeUtil::NowMs()));

  // 绝对 deadline 在未来 5 秒：只要已耗时没超过剩余额度就不算超。
  Metadata future;
  future.SetDeadlineUnixMs(qzrpc::TimeUtil::UnixMs() + 5000);
  ctx.metadata = &future;
  ctx.start_ms = qzrpc::TimeUtil::NowMs();
  QZ_CHECK(!ctx.deadline_exceeded(qzrpc::TimeUtil::NowMs()));
  // 已耗时超过剩余额度（伪造一个很晚的 now_ms）-> 超时。
  QZ_CHECK(ctx.deadline_exceeded(ctx.start_ms + 60000));

  // 绝对 deadline 优先于相对 timeout：timeout 说"还早"，但 deadline 已过就是已过。
  Metadata both;
  both.SetTimeoutMs(100000);
  both.SetDeadlineUnixMs(qzrpc::TimeUtil::UnixMs() - 1);
  ctx.metadata = &both;
  ctx.start_ms = qzrpc::TimeUtil::NowMs();
  QZ_CHECK(ctx.deadline_exceeded(qzrpc::TimeUtil::NowMs()));
}

}  // namespace

int main() {
  // A. 注册
  TestRegisterRejectsInvalidInput();

  // B. 查找
  TestFindSemantics();

  // C. 调用
  TestHandlerInvocationAndContext();
  TestHandlerErrorStatusPassesThrough();
  TestTruncatedPayloadFailsDecode();

  // D. 兜底
  TestFallbackHandler();

  // E. 视图与生命周期
  TestMethodIdsAreSorted();
  TestConcurrentRegisterAndFind();

  // F. 上下文
  TestDeadlineExceeded();

  return qztest::Summary("test_dispatcher");
}
