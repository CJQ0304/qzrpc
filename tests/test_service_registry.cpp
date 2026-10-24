// 轻舟 RPC (Qingzhou RPC)
// tests/test_service_registry.cpp
//
// 服务/方法元信息目录（T-05）。它不参与热路径路由，但它是**三层 method_id 冲突检测里的
// L2**，也是"线上 ERROR 日志里那串 0x21678893 到底是什么方法"的唯一翻译表，所以它的
// 拒绝规则比它的查询功能重要得多。
//
// 这里钉住的判定规则（与 service_registry.h / .cpp 的注释逐条对齐）：
//   1. full_name 为空 / method_id == 0 → 拒绝（0 是保留值，表示"不关心"）；
//   2. 哈希派生（method_id_overridden == false）→ 重算 MethodIdOfName(full_name) 必须相等，
//      不等即判 codegen bug 并拒绝 —— 这是"生成常量与名字不一致导致全员 404"的唯一防线；
//   3. 显式指定（method_id_overridden == true）→ 跳过重算比对（运维逃生口），
//      但**仍参与冲突检测**：逃逸不等于放弃唯一性；
//   4. 同一全名重复登记是**幂等**的，且用最新元信息覆盖（codegen 重新生成后类型名会变）；
//   5. 同一全名带两个不同 id → 拒绝（两个索引会指向不同地方，SelfCheck 也救不回来）。
//
// 为什么"幂等覆盖"值得一条断言：多份生成代码（每个 .qz 一个 TU）都会调
// RegisterXxxService，同一个方法被登记两次是**正常情况**，不是错误。
//
// 注意：ServiceRegistry 是进程级单例，每个用例开头必须 Clear()，否则用例顺序会影响结果
// （ctest 每个测试是独立进程，所以不会跨文件污染，但文件内会）。

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "protocol/protocol_const.h"
#include "server/service_registry.h"
#include "test_main.h"

namespace {

using qzrpc::rpc::MethodInfo;
using qzrpc::rpc::ServiceRegistry;

ServiceRegistry& Registry() { return ServiceRegistry::Instance(); }

// 从 full_name 拆出 package / service / method（只用于构造测试数据）。
MethodInfo MakeInfo(const std::string& full_name, std::uint32_t method_id,
                    bool overridden = false) {
  MethodInfo info;
  info.full_name = full_name;
  info.method_id = method_id;
  info.method_id_overridden = overridden;

  // "demo.EchoService.Echo" -> package=demo, service=EchoService, method=Echo
  const std::size_t last = full_name.rfind('.');
  if (last != std::string::npos) {
    info.method_name = full_name.substr(last + 1);
    const std::string prefix = full_name.substr(0, last);
    const std::size_t second = prefix.rfind('.');
    if (second == std::string::npos) {
      info.service_name = prefix;  // 无 package："Service.Method"
    } else {
      info.package_name = prefix.substr(0, second);
      info.service_name = prefix.substr(second + 1);
    }
  }
  info.request_type = "demo.EchoRequest";
  info.response_type = "demo.EchoResponse";
  return info;
}

// 按名字算出"正确"的 method_id，等价于 codegen 应该生成的常量。
std::uint32_t IdOf(const std::string& full_name) {
  return qzrpc::protocol::MethodIdOfName(full_name);
}

MethodInfo MakeGood(const std::string& full_name) { return MakeInfo(full_name, IdOf(full_name)); }

// ===========================================================================
// A. 登记与查询
// ===========================================================================

void TestRegisterAndFind() {
  Registry().Clear();
  QZ_CHECK_EQ(Registry().size(), std::size_t{0});

  const MethodInfo info = MakeGood("demo.EchoService.Echo");
  std::string err;
  QZ_CHECK(Registry().RegisterMethod(info, &err));
  QZ_CHECK(err.empty());
  QZ_CHECK_EQ(Registry().size(), std::size_t{1});

  // 按 id 查
  const MethodInfo* by_id = Registry().FindMethod(info.method_id);
  QZ_CHECK(by_id != nullptr);
  if (by_id != nullptr) {
    QZ_CHECK_EQ(by_id->full_name, std::string("demo.EchoService.Echo"));
    QZ_CHECK_EQ(by_id->package_name, std::string("demo"));
    QZ_CHECK_EQ(by_id->service_name, std::string("EchoService"));
    QZ_CHECK_EQ(by_id->method_name, std::string("Echo"));
    QZ_CHECK_EQ(by_id->request_type, std::string("demo.EchoRequest"));
    QZ_CHECK_EQ(by_id->response_type, std::string("demo.EchoResponse"));
  }

  // 按名字查：必须指到同一个对象（两个索引共享同一份 MethodInfo）
  const MethodInfo* by_name = Registry().FindByName("demo.EchoService.Echo");
  QZ_CHECK(by_name != nullptr);
  QZ_CHECK(by_name == by_id);

  // 未登记：两种查法都返回 nullptr（而不是抛/崩）
  QZ_CHECK(Registry().FindMethod(0xDEADBEEFU) == nullptr);
  QZ_CHECK(Registry().FindByName("demo.NoSuchService.NoSuchMethod") == nullptr);

  // 空串名字也不该崩
  QZ_CHECK(Registry().FindByName(std::string()) == nullptr);
}

// DescribeMethod 的**未登记格式**是运维脚本与日志解析依赖的稳定契约：
// "unknown#0x1234abcd"（小写、零填充 8 位）。改格式会让"按日志 grep"的脚本静默失效。
void TestDescribeMethod() {
  Registry().Clear();
  const MethodInfo info = MakeGood("demo.EchoService.Echo");
  QZ_CHECK(Registry().RegisterMethod(info, nullptr));

  QZ_CHECK_EQ(Registry().DescribeMethod(info.method_id), std::string("demo.EchoService.Echo"));
  QZ_CHECK_EQ(Registry().DescribeMethod(0x1234ABCDU), std::string("unknown#0x1234abcd"));
  QZ_CHECK_EQ(Registry().DescribeMethod(0U), std::string("unknown#0x00000000"));
  QZ_CHECK_EQ(Registry().DescribeMethod(0xFFFFFFFFU), std::string("unknown#0xffffffff"));
}

// ===========================================================================
// B. 非法入参
// ===========================================================================

void TestRejectsInvalidInput() {
  Registry().Clear();

  {
    MethodInfo info = MakeGood("demo.EchoService.Echo");
    info.full_name.clear();
    std::string err;
    QZ_CHECK(!Registry().RegisterMethod(info, &err));
    QZ_CHECK(err.find("empty full_name") != std::string::npos);
    QZ_CHECK(!Registry().RegisterMethod(info, nullptr));  // err 为空也不能崩
  }
  {
    // method_id 0 是保留值（帧头里 0 表示"不关心"，不能当一个真方法）
    MethodInfo info = MakeInfo("demo.EchoService.Echo", 0);
    std::string err;
    QZ_CHECK(!Registry().RegisterMethod(info, &err));
    QZ_CHECK(err.find("reserved") != std::string::npos);
    QZ_CHECK(!Registry().RegisterMethod(info, nullptr));
  }
  QZ_CHECK_EQ(Registry().size(), std::size_t{0});
}

// ===========================================================================
// C. L2 重算比对（codegen bug 防线）
// ===========================================================================

void TestMethodIdMismatchIsRejected() {
  Registry().Clear();
  // 名字对、id 错（比如生成器用了别的哈希）——必须拒绝，否则线上全员 404。
  const MethodInfo bad = MakeInfo("demo.EchoService.Echo", 0x0000BEEFU, false);
  std::string err;
  QZ_CHECK(!Registry().RegisterMethod(bad, &err));
  QZ_CHECK(err.find("mismatch") != std::string::npos);
  QZ_CHECK_EQ(Registry().size(), std::size_t{0});
  QZ_CHECK(Registry().FindMethod(0x0000BEEFU) == nullptr);

  // 同理，err 传 nullptr 也只返回 false（不能崩、不能静默登记成功）。
  QZ_CHECK(!Registry().RegisterMethod(bad, nullptr));
  QZ_CHECK_EQ(Registry().size(), std::size_t{0});
}

// 显式指定 method_id（IDL 里写了 [method_id = 0x...]）是**合法逃生口**：
// 跳过重算比对，但必须真的登记进去、且 SelfCheck 不把它当问题
// （否则每用一次逃生口就多一条噪音，真问题会被淹掉）。
void TestExplicitMethodIdOverride() {
  Registry().Clear();
  const MethodInfo pinned = MakeInfo("demo.EchoService.Echo", 0x0000ABCDU, true);
  std::string err;
  QZ_CHECK(Registry().RegisterMethod(pinned, &err));
  QZ_CHECK(err.empty());
  QZ_CHECK_EQ(Registry().size(), std::size_t{1});
  QZ_CHECK(Registry().FindMethod(0x0000ABCDU) != nullptr);
  // 哈希值那个 id 不该被占用
  QZ_CHECK(Registry().FindMethod(IdOf("demo.EchoService.Echo")) == nullptr);

  const std::vector<std::string> problems = Registry().SelfCheck();
  QZ_CHECK(problems.empty());

  // 反例对照：同样的 id，但没标 overridden → 必须被拒。
  Registry().Clear();
  const MethodInfo sneaky = MakeInfo("demo.EchoService.Echo", 0x0000ABCDU, false);
  QZ_CHECK(!Registry().RegisterMethod(sneaky, nullptr));
  QZ_CHECK_EQ(Registry().size(), std::size_t{0});
}

// 哈希冲突：两个不同全名撞同一个 id。第二个必须被拒 —— 包括它标了 overridden 的情况
// （逃生口只放行"id 与哈希不一致"，不放行"两个名字共用一个 id"）。
void TestMethodIdCollisionRejected() {
  Registry().Clear();
  QZ_CHECK(Registry().RegisterMethod(MakeGood("demo.EchoService.Echo"), nullptr));

  const std::uint32_t taken = IdOf("demo.EchoService.Echo");
  {
    // 冲突 + 哈希派生：先是重算不匹配？不，这里 id 与自己的哈希也不匹配，
    // 但冲突检查在重算比对**之前**，所以报的是 collision。
    std::string err;
    QZ_CHECK(!Registry().RegisterMethod(MakeInfo("demo.OtherService.Call", taken, true), &err));
    QZ_CHECK(err.find("collision") != std::string::npos);
  }
  QZ_CHECK_EQ(Registry().size(), std::size_t{1});
  // 原来的方法还在，没被覆盖
  const MethodInfo* kept = Registry().FindMethod(taken);
  QZ_CHECK(kept != nullptr);
  if (kept != nullptr) { QZ_CHECK_EQ(kept->full_name, std::string("demo.EchoService.Echo")); }
  QZ_CHECK(Registry().FindByName("demo.OtherService.Call") == nullptr);
}

// ===========================================================================
// D. 重复登记（正常情况：每份生成代码都会调一次 RegisterXxxService）
// ===========================================================================

void TestRepeatedRegistrationIsIdempotent() {
  Registry().Clear();
  const std::string name = "demo.EchoService.Echo";
  const MethodInfo first = MakeGood(name);
  QZ_CHECK(Registry().RegisterMethod(first, nullptr));
  QZ_CHECK(Registry().size() == std::size_t{1});

  // 再登记一次：成功、不重复计数。
  QZ_CHECK(Registry().RegisterMethod(MakeGood(name), nullptr));
  QZ_CHECK_EQ(Registry().size(), std::size_t{1});

  // 元信息更新：第二次登记的内容必须生效（codegen 重新生成后类型名/幂等标记会变）。
  MethodInfo updated = MakeGood(name);
  updated.request_type = "demo.EchoRequestV2";
  updated.idempotent = true;
  QZ_CHECK(Registry().RegisterMethod(updated, nullptr));
  QZ_CHECK_EQ(Registry().size(), std::size_t{1});
  const MethodInfo* slot = Registry().FindByName(name);
  QZ_CHECK(slot != nullptr);
  if (slot != nullptr) {
    QZ_CHECK_EQ(slot->request_type, std::string("demo.EchoRequestV2"));
    QZ_CHECK(slot->idempotent);
  }

  // 同一全名但两个不同 id → 拒绝。两个索引会指向不同条目，SelfCheck 也救不回来。
  MethodInfo conflicting = MakeGood(name);
  conflicting.method_id = 0x0000FEEDU;
  conflicting.method_id_overridden = true;  // 绕过重算，仍然要撞在"同名字不同 id"上
  std::string err;
  QZ_CHECK(!Registry().RegisterMethod(conflicting, &err));
  QZ_CHECK(err.find("two ids") != std::string::npos);
  QZ_CHECK_EQ(Registry().size(), std::size_t{1});
}

// ===========================================================================
// E. 列表与排序
// ===========================================================================

void TestAllMethodsAndServiceNames() {
  Registry().Clear();
  QZ_CHECK(Registry().RegisterMethod(MakeGood("demo.EchoService.Echo"), nullptr));
  QZ_CHECK(Registry().RegisterMethod(MakeGood("demo.EchoService.Notify"), nullptr));
  QZ_CHECK(Registry().RegisterMethod(MakeGood("demo.AdminService.Metrics"), nullptr));
  QZ_CHECK_EQ(Registry().size(), std::size_t{3});

  // AllMethods：按 method_id 升序（调用方/serializer 依赖稳定顺序做 diff 与快照）。
  const std::vector<MethodInfo> methods = Registry().AllMethods();
  QZ_CHECK_EQ(methods.size(), std::size_t{3});
  QZ_CHECK(std::is_sorted(methods.begin(), methods.end(),
                          [](const MethodInfo& a, const MethodInfo& b) {
                            return a.method_id < b.method_id;
                          }));
  if (methods.size() == 3) {
    // 三个名字都在，一个不多一个不少（顺序由上面那条断言负责）。
    std::vector<std::string> names;
    for (const MethodInfo& method : methods) { names.push_back(method.full_name); }
    std::sort(names.begin(), names.end());
    QZ_CHECK_EQ(names[0], std::string("demo.AdminService.Metrics"));
    QZ_CHECK_EQ(names[1], std::string("demo.EchoService.Echo"));
    QZ_CHECK_EQ(names[2], std::string("demo.EchoService.Notify"));
  }

  // ServiceNames：去重 + 升序
  const std::vector<std::string> services = Registry().ServiceNames();
  QZ_CHECK_EQ(services.size(), std::size_t{2});
  if (services.size() == 2) {
    QZ_CHECK_EQ(services[0], std::string("AdminService"));
    QZ_CHECK_EQ(services[1], std::string("EchoService"));
  }
}

// service_name 没填时从 full_name 推导，避免调用方拿到空名字。
// 两种形态都要覆盖："pkg.Service.Method" 与无 package 的 "Service.Method"。
//
// ★ 推导规则是"去掉最后一段（方法名）后剩下的全部"，**保留 package 前缀**：
//   demo.EchoService.Echo -> "demo.EchoService"，PingService.Ping -> "PingService"。
//   这不是随手写的：service_name 要能当全局标识用（注册中心的 key、metrics label、
//   日志里的 svc 字段），只留短名会让 demo.EchoService 与 other.EchoService 撞在一起。
//   头文件（冻结）只承诺"去重、升序"，没承诺短名形式，所以这里以实现为准并钉死。
void TestServiceNameDerivation() {
  Registry().Clear();

  MethodInfo no_service = MakeGood("demo.EchoService.Echo");
  no_service.service_name.clear();
  QZ_CHECK(Registry().RegisterMethod(no_service, nullptr));

  MethodInfo no_package = MakeGood("PingService.Ping");
  no_package.service_name.clear();
  no_package.package_name.clear();
  QZ_CHECK(Registry().RegisterMethod(no_package, nullptr));

  const std::vector<std::string> services = Registry().ServiceNames();
  QZ_CHECK_EQ(services.size(), std::size_t{2});
  if (services.size() == 2) {
    // 升序是 std::set 给的字典序：'P'(0x50) < 'd'(0x64)。这里是关键：
    // 推导出来的名字带小写 package 前缀，排序结果与"短名"直觉不同，所以顺序要显式钉住。
    QZ_CHECK_EQ(services[0], std::string("PingService"));
    QZ_CHECK_EQ(services[1], std::string("demo.EchoService"));
  }
}

// ===========================================================================
// F. SelfCheck
// ===========================================================================

void TestSelfCheckClean() {
  Registry().Clear();
  QZ_CHECK(Registry().RegisterMethod(MakeGood("demo.EchoService.Echo"), nullptr));
  QZ_CHECK(Registry().RegisterMethod(MakeGood("demo.EchoService.Notify"), nullptr));
  QZ_CHECK(Registry().SelfCheck().empty());

  // 清空后也应当是"干净"的（空注册表不是问题，只是没登记）。
  Registry().Clear();
  QZ_CHECK(Registry().SelfCheck().empty());
}

// 必填元信息缺失不影响运行，但会让日志/metrics 变成一锅粥，所以 SelfCheck 要报出来。
void TestSelfCheckReportsMissingMetadata() {
  Registry().Clear();
  MethodInfo info = MakeGood("demo.EchoService.Echo");
  info.service_name.clear();   // 注意：ServiceNames() 会推导，但 SelfCheck 仍然要报
  QZ_CHECK(Registry().RegisterMethod(info, nullptr));

  const std::vector<std::string> problems = Registry().SelfCheck();
  QZ_CHECK(!problems.empty());
  bool has_service_problem = false;
  for (const std::string& text : problems) {
    if (text.find("empty service_name") != std::string::npos) { has_service_problem = true; }
  }
  QZ_CHECK(has_service_problem);

  Registry().Clear();
  MethodInfo no_types = MakeGood("demo.EchoService.Echo");
  no_types.request_type.clear();
  QZ_CHECK(Registry().RegisterMethod(no_types, nullptr));
  bool has_type_problem = false;
  for (const std::string& text : Registry().SelfCheck()) {
    if (text.find("request_type") != std::string::npos) { has_type_problem = true; }
  }
  QZ_CHECK(has_type_problem);
}

// ===========================================================================
// G. 单例与并发读
// ===========================================================================

void TestSingletonIdentity() {
  QZ_CHECK(&Registry() == &ServiceRegistry::Instance());
  QZ_CHECK(&Registry() == &ServiceRegistry::Instance());
}

// 并发读：FindMethod / FindByName / DescribeMethod 都走 shared_lock，
// 多个读者必须能同时跑。这里只验证"不崩、结果正确"；真正的数据竞争要靠 TSAN/ASAN
// （本机只有 MSVC，没有 TSAN，所以这条只是烟测）。
//
// 注意：QZ_CHECK 会改全局非原子计数器，**不能**在子线程里用。子线程只累积 atomic 计数，
// join 之后再断言。
void TestConcurrentReaders() {
  Registry().Clear();
  const std::string name = "demo.EchoService.Echo";
  const MethodInfo info = MakeGood(name);
  QZ_CHECK(Registry().RegisterMethod(info, nullptr));

  constexpr int kThreads = 4;
  constexpr int kIterations = 500;
  std::atomic<int> hits{0};
  std::atomic<int> misses{0};
  std::vector<std::thread> readers;
  readers.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    readers.emplace_back([&name, &hits, &misses, id = info.method_id]() {
      for (int n = 0; n < kIterations; ++n) {
        const MethodInfo* by_id = ServiceRegistry::Instance().FindMethod(id);
        const MethodInfo* by_name = ServiceRegistry::Instance().FindByName(name);
        if (by_id != nullptr && by_name != nullptr &&
            ServiceRegistry::Instance().DescribeMethod(id) == name) {
          hits.fetch_add(1, std::memory_order_relaxed);
        } else {
          misses.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& worker : readers) { worker.join(); }

  QZ_CHECK_EQ(hits.load(), kThreads * kIterations);
  QZ_CHECK_EQ(misses.load(), 0);
}

// 与 codegen 的跨语言 golden 再确认一次：注册表的核心用途就是把这个 4 字节翻回名字。
// 0x21678893 这个值在 codegen 的 test_methodid.py 与 test_codec 里是同一组交叉验证过的。
void TestCodegenAgreement() {
  Registry().Clear();
  constexpr std::uint32_t kGoldenId = 0x21678893U;
  const std::string name = "demo.EchoService.Echo";
  QZ_CHECK_EQ(qzrpc::protocol::MethodIdOfName(name), kGoldenId);

  // 用"生成器给出的常量"（这里就是 golden 值）登记，必须通过：
  // 若哪天 FNV 实现或名字拼接规则变了，这条会先红。
  QZ_CHECK(Registry().RegisterMethod(MakeInfo(name, kGoldenId), nullptr));
  QZ_CHECK_EQ(Registry().DescribeMethod(kGoldenId), name);
}

}  // namespace

int main() {
  // A. 登记与查询
  TestRegisterAndFind();
  TestDescribeMethod();

  // B. 非法入参
  TestRejectsInvalidInput();

  // C. L2 重算比对
  TestMethodIdMismatchIsRejected();
  TestExplicitMethodIdOverride();
  TestMethodIdCollisionRejected();

  // D. 重复登记
  TestRepeatedRegistrationIsIdempotent();

  // E. 列表与排序
  TestAllMethodsAndServiceNames();
  TestServiceNameDerivation();

  // F. SelfCheck
  TestSelfCheckClean();
  TestSelfCheckReportsMissingMetadata();

  // G. 单例与并发
  TestSingletonIdentity();
  TestConcurrentReaders();
  TestCodegenAgreement();

  ServiceRegistry::Instance().Clear();
  return qztest::Summary("test_service_registry");
}
