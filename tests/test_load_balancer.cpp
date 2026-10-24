// 轻舟 RPC (Qingzhou RPC)
// tests/test_load_balancer.cpp
//
// 三个负载均衡策略的契约测试（T-08）。
//
// 为什么这条用例值得单独写：
//   负载均衡错了**不会报错**。请求全部打到同一个实例、或者重试永远打在坏实例上，
//   程序照跑、日志干干净净，只有在"某个实例挂了"或"看各实例 metrics 严重不均"时才暴露。
//   下面每一条断言都对应一个"错了也看不出来"的具体失效模式。
//
// 三个策略的分工与各自的坑：
//   * RoundRobin      —— attempt **不参与**计算（见 TestRoundRobinIgnoresAttempt 的注释，
//                        这里和 load_balancer.h 第 51 行的旧描述不一致，以 .cpp 为准）；
//                        换实例靠全局游标，所以"相邻两次 Select 落到不同实例"是它的语义。
//   * Random          —— 种子决定一切：同种子同序列必须可复现，否则压测/回放无法重放。
//                        attempt>0 时**确定地**避开上一次的实例（不是"多抽几次碰运气"）。
//   * ConsistentHash  —— 同一 key 恒定同实例（亲和性）；空 key 退化为轮询。
//                        环位置函数必须带终态混合，否则 user-0..user-199 会全部落到一个节点
//                        （裸 FNV-1a 对只差末字符的短 key 雪崩极差，实测 200/200）。
//
// 共同契约：endpoint 列表会先清洗（丢 port==0 / 空 ip / 重复），空列表时 Select 返回 false。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client/load_balancer.h"
#include "net/socket_util.h"
#include "test_main.h"

namespace {

using qzrpc::net::Endpoint;
using qzrpc::rpc::ConsistentHashBalancer;
using qzrpc::rpc::CreateLoadBalancer;
using qzrpc::rpc::LoadBalancer;
using qzrpc::rpc::LoadBalancerType;
using qzrpc::rpc::ParseLoadBalancerType;
using qzrpc::rpc::RandomBalancer;
using qzrpc::rpc::RoundRobinBalancer;

// Endpoint 没有 operator== / operator<（它是网络层类型，不该为测试扩大接口），
// 所以在测试里给它两个本地工具函数。
bool Same(const Endpoint& a, const Endpoint& b) { return a.ip == b.ip && a.port == b.port; }

std::string Describe(const Endpoint& ep) {
  return ep.ip + ":" + std::to_string(static_cast<unsigned>(ep.port));
}

// 造 N 个合法端点：127.0.0.1:17000, 17001, ...
std::vector<Endpoint> MakeEndpoints(std::size_t count, std::uint16_t base_port = 17000) {
  std::vector<Endpoint> endpoints;
  endpoints.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    Endpoint ep;
    ep.ip = "127.0.0.1";
    ep.port = static_cast<std::uint16_t>(base_port + i);
    endpoints.push_back(ep);
  }
  return endpoints;
}

// ===========================================================================
// A. RoundRobinBalancer
// ===========================================================================

void TestRoundRobinBasics() {
  RoundRobinBalancer balancer;
  QZ_CHECK_EQ(std::string(balancer.name()), std::string("round_robin"));

  // 空列表：Select 必须返回 false 且**不写** out。
  Endpoint untouched;
  untouched.ip = "sentinel";
  untouched.port = 1234;
  QZ_CHECK(!balancer.Select("k", 0, &untouched));
  QZ_CHECK_EQ(untouched.ip, std::string("sentinel"));  // 没被覆盖
  QZ_CHECK_EQ(untouched.port, static_cast<std::uint16_t>(1234));
  QZ_CHECK_EQ(balancer.size(), std::size_t{0});

  const std::vector<Endpoint> endpoints = MakeEndpoints(3);
  balancer.UpdateEndpoints(endpoints);
  QZ_CHECK_EQ(balancer.size(), std::size_t{3});

  // 300 次 = 3 的整数倍 -> 每个端点**恰好** 100 次。
  // 用"恰好"而不是"差不多"：轮询是确定性算法，任何偏差都说明取模或游标写错了。
  std::map<std::string, int> hits;
  for (int i = 0; i < 300; ++i) {
    Endpoint picked;
    QZ_CHECK(balancer.Select("ignored-key", 0, &picked));
    ++hits[Describe(picked)];
  }
  QZ_CHECK_EQ(hits.size(), std::size_t{3});
  for (const auto& item : hits) {
    QZ_CHECK_EQ(item.second, 100);
  }
}

// ★ 这条钉的是实现里的一处**刻意取舍**，且它和头文件第 51 行的旧描述不一致：
//   `load_balancer.h` 写的是"attempt>0 时在计数器基础上偏移 attempt"，但 .cpp 明确
//   `(void)attempt`，注释给的理由很有说服力：
//     曾经写成 (ticket + attempt) % size，而 ticket 每调一次 +1、attempt 每次重试也 +1，
//     两个增量在同一个模里互相抵消 —— size == 2 时 (0,0)(1,1)(2,2) 全部落到下标 0，
//     **重试永远打在同一个实例上**；三实例反而看不出问题（(0,0)(1,1)(2,2) → 0,2,1 会轮）。
//   后果是"双实例挂一个"时 20 次调用 10 成功却要 7.4 秒——属于验收里最难暴露的那类。
//
//   所以这里断言的是**语义结果**（相邻两次 Select 必须换实例），而不是 attempt 的算法：
//   将来谁把 attempt 偏移加回来，只要破坏了"换实例"，这条就会红。
void TestRoundRobinIgnoresAttempt() {
  const std::vector<Endpoint> endpoints = MakeEndpoints(2);
  RoundRobinBalancer balancer;
  balancer.UpdateEndpoints(endpoints);

  // size == 2：连续两次 Select（一次 attempt=0、一次 attempt=1，模拟一次重试）
  // 必须落在**不同**实例上。这正是"故障转移"能不能生效的判据。
  int differed = 0;
  for (int i = 0; i < 100; ++i) {
    Endpoint first;
    Endpoint second;
    QZ_CHECK(balancer.Select("k", 0, &first));
    QZ_CHECK(balancer.Select("k", 1, &second));
    if (!Same(first, second)) ++differed;
  }
  // 游标每调一次 +1，size=2 时相邻两次必然不同 -> 100/100。
  QZ_CHECK_EQ(differed, 100);

  // size == 1：无论 attempt 是多少都只能是同一个实例 —— "重试换节点"在单实例下无解，
  // 实现不许因此返回 false 或崩溃。
  std::vector<Endpoint> single = MakeEndpoints(1);
  balancer.UpdateEndpoints(single);
  for (std::size_t attempt = 0; attempt < 5; ++attempt) {
    Endpoint picked;
    QZ_CHECK(balancer.Select("k", attempt, &picked));
    QZ_CHECK(Same(picked, single[0]));
  }
}

// 并发下必须"均匀且不越界"。均匀性在并发场景里最容易出问题：
// 用非原子游标会丢失增量（分布偏），用非原子容器会读到正在被替换的 vector（越界/崩溃）。
//
// 注意：QZ_CHECK 会改全局计数，**不能**在子线程里用，所以线程内只累加局部计数，
// join 之后统一断言。
void TestRoundRobinConcurrentDistribution() {
  constexpr std::size_t kEndpoints = 3;
  constexpr int kThreads = 200;
  constexpr int kPerThread = 1000;

  RoundRobinBalancer balancer;
  balancer.UpdateEndpoints(MakeEndpoints(kEndpoints));

  // 每个线程各自计数，避免 20 万次 fetch_add 把测量本身变成瓶颈（那样测的是原子争用，不是分布）。
  std::vector<std::vector<int>> per_thread(static_cast<std::size_t>(kThreads),
                                           std::vector<int>(kEndpoints, 0));
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kThreads));
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&balancer, &per_thread, &failures, t] {
      std::vector<int>& counts = per_thread[static_cast<std::size_t>(t)];
      for (int i = 0; i < kPerThread; ++i) {
        Endpoint picked;
        if (!balancer.Select("k", 0, &picked)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const unsigned index = static_cast<unsigned>(picked.port) - 17000U;
        if (index >= kEndpoints) {  // 越界 = 读到非法 endpoint
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        ++counts[index];
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  QZ_CHECK_EQ(failures.load(), 0);

  std::vector<long long> totals(kEndpoints, 0);
  long long sum = 0;
  for (const std::vector<int>& counts : per_thread) {
    for (std::size_t i = 0; i < kEndpoints; ++i) {
      totals[i] += counts[i];
      sum += counts[i];
    }
  }
  const long long total = static_cast<long long>(kThreads) * kPerThread;
  QZ_CHECK_EQ(sum, total);

  // 期望每个端点 total/3；允许 ±10%。轮询**理论上**是精确均匀的（游标 mod 3），
  // 之所以给 10% 的余量：游标的起点取决于线程调度，最后不一定是 3 的整数倍，
  // 而且"更严格的断言"在这里没有额外收益 —— 它测的是调度器，不是被测代码。
  const long long expected = total / static_cast<long long>(kEndpoints);
  const long long tolerance = expected / 10;
  if (totals[0] < expected - tolerance || totals[0] > expected + tolerance) {
    std::printf("  (endpoint 0 got %lld, expected ~%lld +- %lld)\n", totals[0], expected,
                tolerance);
  }
  for (std::size_t i = 0; i < kEndpoints; ++i) {
    QZ_CHECK(totals[i] >= expected - tolerance);
    QZ_CHECK(totals[i] <= expected + tolerance);
  }
}

// 所有策略都必须清洗入口数据：注册中心可能返回半残条目（port 0 / 空 ip / 重复）。
// "在入口清一次"比"每次 Select 再判"省一条热路径分支，也让 size() 的语义变准。
void TestEndpointSanitizing() {
  std::vector<Endpoint> dirty;
  Endpoint good_a{"127.0.0.1", 17000};
  Endpoint good_b{"127.0.0.1", 17001};
  Endpoint dup_a{"127.0.0.1", 17000};
  Endpoint zero_port{"127.0.0.1", 0};
  Endpoint empty_ip{"", 17002};
  dirty.push_back(good_a);
  dirty.push_back(dup_a);
  dirty.push_back(zero_port);
  dirty.push_back(empty_ip);
  dirty.push_back(good_b);

  {
    RoundRobinBalancer balancer;
    balancer.UpdateEndpoints(dirty);
    QZ_CHECK_EQ(balancer.size(), std::size_t{2});
    for (int i = 0; i < 20; ++i) {
      Endpoint picked;
      QZ_CHECK(balancer.Select("k", 0, &picked));
      QZ_CHECK(Same(picked, good_a) || Same(picked, good_b));
      QZ_CHECK(picked.port != 0);
      QZ_CHECK(!picked.ip.empty());
    }
  }
  {
    RandomBalancer balancer(1);
    balancer.UpdateEndpoints(dirty);
    QZ_CHECK_EQ(balancer.size(), std::size_t{2});
  }
  {
    ConsistentHashBalancer balancer;
    balancer.UpdateEndpoints(dirty);
    QZ_CHECK_EQ(balancer.size(), std::size_t{2});
    Endpoint picked;
    QZ_CHECK(balancer.Select("some-key", 0, &picked));
    QZ_CHECK(Same(picked, good_a) || Same(picked, good_b));
  }

  // 全部非法 -> 视同空列表。
  {
    RoundRobinBalancer balancer;
    balancer.UpdateEndpoints({zero_port, empty_ip});
    QZ_CHECK_EQ(balancer.size(), std::size_t{0});
    Endpoint picked;
    QZ_CHECK(!balancer.Select("k", 0, &picked));
  }
}

// ===========================================================================
// B. RandomBalancer
// ===========================================================================

// 种子必须决定一切：同种子 + 同调用序列 => 同结果。
// 没有这条，压测复现、故障回放、以及"这个分布偏差是代码问题还是运气"的争论都无从谈起。
void TestRandomIsReproducibleWithSeed() {
  const std::vector<Endpoint> endpoints = MakeEndpoints(4);

  RandomBalancer left(12345);
  RandomBalancer right(12345);
  left.UpdateEndpoints(endpoints);
  right.UpdateEndpoints(endpoints);

  int same = 0;
  for (int i = 0; i < 200; ++i) {
    Endpoint a;
    Endpoint b;
    QZ_CHECK(left.Select("k", 0, &a));
    QZ_CHECK(right.Select("k", 0, &b));
    if (Same(a, b)) ++same;
  }
  QZ_CHECK_EQ(same, 200);

  // 不同种子应当产生不同的序列（否则种子根本没参与）。
  RandomBalancer other(999);
  other.UpdateEndpoints(endpoints);
  int identical_prefix = 0;
  RandomBalancer baseline(12345);
  baseline.UpdateEndpoints(endpoints);
  for (int i = 0; i < 200; ++i) {
    Endpoint a;
    Endpoint b;
    QZ_CHECK(baseline.Select("k", 0, &a));
    QZ_CHECK(other.Select("k", 0, &b));
    if (Same(a, b)) ++identical_prefix;
  }
  // 4 个实例，200 次全同的概率是 4^-200，实际为 0；给一个宽松上界即可。
  QZ_CHECK(identical_prefix < 120);
}

void TestRandomDistributionAndRetryAvoidance() {
  const std::vector<Endpoint> endpoints = MakeEndpoints(4);
  RandomBalancer balancer(20240501);
  balancer.UpdateEndpoints(endpoints);

  // 分布：4000 次 / 4 实例 = 每个 ~1000，允许 ±25%（均匀分布 4000 次四选一，
  // 标准差 ~27，±250 是 9 个标准差，几乎不可能误报）。
  std::map<std::string, int> hits;
  for (int i = 0; i < 4000; ++i) {
    Endpoint picked;
    QZ_CHECK(balancer.Select("k", 0, &picked));
    ++hits[Describe(picked)];
  }
  QZ_CHECK_EQ(hits.size(), std::size_t{4});
  for (const auto& item : hits) {
    QZ_CHECK(item.second > 750);
    QZ_CHECK(item.second < 1250);
  }

  // attempt>0 必须**确定地**避开上一次的实例：实现不是"再抽一次碰运气"，
  // 而是当抽回同一个时在 [0, n-2] 里重取再跳过 last —— 所以这是硬保证，不是概率。
  int violated = 0;
  Endpoint previous;
  QZ_CHECK(balancer.Select("k", 0, &previous));
  for (int i = 0; i < 500; ++i) {
    Endpoint retry;
    QZ_CHECK(balancer.Select("k", 1, &retry));
    if (Same(retry, previous)) ++violated;
    previous = retry;  // 每次重试后又成为"上一次"，继续验证
  }
  QZ_CHECK_EQ(violated, 0);

  // 单实例时"避开上一次"没有解：必须返回那唯一的实例，而不是 false。
  std::vector<Endpoint> single = MakeEndpoints(1);
  balancer.UpdateEndpoints(single);
  Endpoint picked;
  QZ_CHECK(balancer.Select("k", 3, &picked));
  QZ_CHECK(Same(picked, single[0]));
}

// ===========================================================================
// C. ConsistentHashBalancer
// ===========================================================================

// 亲和性：同一 key 无论在什么时候、被多少个别的 key 夹在中间，都必须落到同一实例。
// 这是"同一个用户/同一个订单始终打同一台缓存"的全部依据 —— 破坏了不会报错，
// 只会让缓存命中率悄悄掉下来。
void TestConsistentHashAffinity() {
  ConsistentHashBalancer balancer;
  balancer.UpdateEndpoints(MakeEndpoints(3));
  QZ_CHECK_EQ(std::string(balancer.name()), std::string("consistent_hash"));
  QZ_CHECK_EQ(balancer.virtual_nodes(), std::size_t{160});

  std::vector<std::string> keys;
  std::vector<Endpoint> first_choice;
  for (int i = 0; i < 50; ++i) keys.push_back("user-" + std::to_string(i));

  for (const std::string& key : keys) {
    Endpoint picked;
    QZ_CHECK(balancer.Select(key, 0, &picked));
    first_choice.push_back(picked);
  }
  // 中间插入大量别的 key（会推进内部轮询游标、也会走一遍环查找）。
  for (int i = 0; i < 500; ++i) {
    Endpoint ignored;
    QZ_CHECK(balancer.Select("noise-" + std::to_string(i), 0, &ignored));
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    Endpoint again;
    QZ_CHECK(balancer.Select(keys[i], 0, &again));
    QZ_CHECK(Same(again, first_choice[i]));
  }
}

// ★ 环位置函数必须带终态混合（splitmix64 finalizer）的回归测试。
//
// 裸 FNV-1a 对"只差末几个字符"的短 key 雪崩极差：user-0..user-199 的哈希全部落在
// 2^40 宽的窄带里，200 个 key **全部**打到同一个实例。症状是负载长期歪在一个节点上，
// 不报错、不崩溃、日志干净 —— 只有盯各实例 metrics 才看得出来。
//
// 这里断言的是分布结果而不是哈希值本身：混合常量属于实现细节允许改，
// 但"200 个末字符不同的 key 必须散开"是契约。
void TestConsistentHashSpreadsShortKeys() {
  ConsistentHashBalancer balancer;
  balancer.UpdateEndpoints(MakeEndpoints(3));

  std::map<std::string, int> hits;
  for (int i = 0; i < 200; ++i) {
    Endpoint picked;
    QZ_CHECK(balancer.Select("user-" + std::to_string(i), 0, &picked));
    ++hits[Describe(picked)];
  }

  if (hits.size() < 3) {
    for (const auto& item : hits) {
      std::printf("  (%s -> %d of 200)\n", item.first.c_str(), item.second);
    }
  }
  // 三个实例都必须被用到。裸 FNV 时这里是 1（200/200 全落一个节点）。
  QZ_CHECK_EQ(hits.size(), std::size_t{3});
  // 任何单个实例不应超过 60%：160 虚拟节点 × 3 下的理论值约 33%，
  // 给到 60% 是留出"环位置略不均"的余量，同时仍能挡住"几乎全落一个节点"的退化。
  for (const auto& item : hits) {
    QZ_CHECK(item.second < 120);
    QZ_CHECK(item.second > 20);
  }
}

// 空 key 退化为轮询 —— 契约行为，不是兜底。
// 理由：没有哈希键就没有亲和性可谈，硬算一个固定位置只会让所有调用挤到同一实例。
void TestConsistentHashEmptyKeyFallsBackToRoundRobin() {
  ConsistentHashBalancer balancer;
  const std::vector<Endpoint> endpoints = MakeEndpoints(3);
  balancer.UpdateEndpoints(endpoints);

  std::map<std::string, int> hits;
  for (int i = 0; i < 30; ++i) {
    Endpoint picked;
    QZ_CHECK(balancer.Select("", 0, &picked));
    ++hits[Describe(picked)];
  }
  QZ_CHECK_EQ(hits.size(), std::size_t{3});
  for (const auto& item : hits) {
    QZ_CHECK_EQ(item.second, 10);  // 30 次 / 3 = 每实例恰好 10 次
  }
}

// 重试换节点：attempt>0 会把 attempt 混进 key 再算一次环位置。
// 注意这是**概率性**换节点（不同环位置仍可能落在同一实例），所以断言比例而不是全等。
void TestConsistentHashAttemptMovesToAnotherNode() {
  ConsistentHashBalancer balancer;
  balancer.UpdateEndpoints(MakeEndpoints(3));

  int differed = 0;
  constexpr int kKeys = 300;
  for (int i = 0; i < kKeys; ++i) {
    Endpoint first;
    Endpoint retry;
    const std::string key = "order-" + std::to_string(i);
    QZ_CHECK(balancer.Select(key, 0, &first));
    QZ_CHECK(balancer.Select(key, 1, &retry));
    if (!Same(first, retry)) ++differed;
  }
  // 三节点随机重映射，期望约 2/3 会换；给 50% 的下界，避免用一条统计断言绑死实现细节。
  std::printf("  (consistent_hash: %d/%d keys switched node on retry)\n", differed, kKeys);
  QZ_CHECK(differed > kKeys / 2);
}

void TestConsistentHashEmptyAndCustomVirtualNodes() {
  ConsistentHashBalancer balancer(64);
  QZ_CHECK_EQ(balancer.virtual_nodes(), std::size_t{64});

  Endpoint picked;
  QZ_CHECK(!balancer.Select("k", 0, &picked));  // 还没配置端点
  QZ_CHECK_EQ(balancer.size(), std::size_t{0});

  balancer.UpdateEndpoints(MakeEndpoints(2));
  QZ_CHECK_EQ(balancer.size(), std::size_t{2});
  QZ_CHECK(balancer.Select("k", 0, &picked));

  // 清空后必须重新返回 false（注册中心把服务摘光的场景）。
  balancer.UpdateEndpoints({});
  QZ_CHECK_EQ(balancer.size(), std::size_t{0});
  QZ_CHECK(!balancer.Select("k", 0, &picked));
}

// 刷新期间并发 Select 不能被长时间挡住（契约要求重建在锁外做）。
// 这条用例只验证"不崩、不返回脏数据、刷新后立刻生效"，不断言性能数字 ——
// 吞吐类断言在共享 CI 机器上必然抖，属于"看起来严格、实际会偶发红"的坏断言。
void TestConsistentHashConcurrentRefresh() {
  ConsistentHashBalancer balancer;
  balancer.UpdateEndpoints(MakeEndpoints(3));

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<long long> selected{0};

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&balancer, &stop, &failures, &selected, t] {
      int i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        Endpoint picked;
        if (!balancer.Select("key-" + std::to_string(t) + "-" + std::to_string(i), 0, &picked)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        } else if (picked.port < 17000 || picked.port > 17005 || picked.ip != "127.0.0.1") {
          failures.fetch_add(1, std::memory_order_relaxed);  // 返回了不在任何一版列表里的地址
        } else {
          selected.fetch_add(1, std::memory_order_relaxed);
        }
        ++i;
      }
    });
  }

  // 交替刷新两版列表；旧快照仍可被正在进行的 Select 持有（shared_ptr），这是设计允许的。
  for (int round = 0; round < 100; ++round) {
    balancer.UpdateEndpoints(MakeEndpoints(3, static_cast<std::uint16_t>(17000 + (round % 2) * 3)));
    std::this_thread::yield();
  }
  balancer.UpdateEndpoints(MakeEndpoints(3));
  stop.store(true);
  for (std::thread& thread : readers) thread.join();

  QZ_CHECK_EQ(failures.load(), 0);
  QZ_CHECK(selected.load() > 0);
  QZ_CHECK_EQ(balancer.size(), std::size_t{3});
  Endpoint picked;
  QZ_CHECK(balancer.Select("after-refresh", 0, &picked));
  QZ_CHECK(picked.port >= 17000 && picked.port <= 17002);
}

// ===========================================================================
// D. 解析与工厂
// ===========================================================================

void TestParseLoadBalancerType() {
  std::string err;
  QZ_CHECK(ParseLoadBalancerType("round_robin") == LoadBalancerType::kRoundRobin);
  QZ_CHECK(ParseLoadBalancerType("RR") == LoadBalancerType::kRoundRobin);
  QZ_CHECK(ParseLoadBalancerType("Round_Robin") == LoadBalancerType::kRoundRobin);
  QZ_CHECK(ParseLoadBalancerType("random") == LoadBalancerType::kRandom);
  QZ_CHECK(ParseLoadBalancerType("RANDOM") == LoadBalancerType::kRandom);
  QZ_CHECK(ParseLoadBalancerType("consistent_hash") == LoadBalancerType::kConsistentHash);
  QZ_CHECK(ParseLoadBalancerType("CH") == LoadBalancerType::kConsistentHash);
  QZ_CHECK(ParseLoadBalancerType("ch") == LoadBalancerType::kConsistentHash);

  // 无法识别：退回轮询并把原因写进 err（不抛、不崩）。
  err.clear();
  QZ_CHECK(ParseLoadBalancerType("magic", &err) == LoadBalancerType::kRoundRobin);
  QZ_CHECK(!err.empty());
  // err 传 nullptr 时不能崩。
  QZ_CHECK(ParseLoadBalancerType("magic", nullptr) == LoadBalancerType::kRoundRobin);
  // 空串按"未配置"处理，同样退回轮询。
  QZ_CHECK(ParseLoadBalancerType("") == LoadBalancerType::kRoundRobin);
}

void TestCreateLoadBalancer() {
  std::unique_ptr<LoadBalancer> rr = CreateLoadBalancer(LoadBalancerType::kRoundRobin);
  QZ_CHECK(rr != nullptr);
  QZ_CHECK_EQ(std::string(rr->name()), std::string("round_robin"));
  QZ_CHECK_EQ(rr->size(), std::size_t{0});

  std::unique_ptr<LoadBalancer> random = CreateLoadBalancer(LoadBalancerType::kRandom);
  QZ_CHECK(random != nullptr);
  QZ_CHECK_EQ(std::string(random->name()), std::string("random"));

  std::unique_ptr<LoadBalancer> ch = CreateLoadBalancer(LoadBalancerType::kConsistentHash, 32);
  QZ_CHECK(ch != nullptr);
  QZ_CHECK_EQ(std::string(ch->name()), std::string("consistent_hash"));

  // 工厂必须把 virtual_nodes 透传下去（配置里改它却不生效是很隐蔽的一类问题）。
  ConsistentHashBalancer* concrete = dynamic_cast<ConsistentHashBalancer*>(ch.get());
  QZ_CHECK(concrete != nullptr);
  if (concrete != nullptr) QZ_CHECK_EQ(concrete->virtual_nodes(), std::size_t{32});

  // 通过接口指针使用（调用方只会拿到 LoadBalancer&）。
  ch->UpdateEndpoints(MakeEndpoints(2));
  Endpoint picked;
  QZ_CHECK(ch->Select("k", 0, &picked));
  QZ_CHECK_EQ(ch->size(), std::size_t{2});
}

}  // namespace

int main() {
  // A. 轮询
  TestRoundRobinBasics();
  TestRoundRobinIgnoresAttempt();
  TestRoundRobinConcurrentDistribution();
  TestEndpointSanitizing();

  // B. 随机
  TestRandomIsReproducibleWithSeed();
  TestRandomDistributionAndRetryAvoidance();

  // C. 一致性哈希
  TestConsistentHashAffinity();
  TestConsistentHashSpreadsShortKeys();
  TestConsistentHashEmptyKeyFallsBackToRoundRobin();
  TestConsistentHashAttemptMovesToAnotherNode();
  TestConsistentHashEmptyAndCustomVirtualNodes();
  TestConsistentHashConcurrentRefresh();

  // D. 解析与工厂
  TestParseLoadBalancerType();
  TestCreateLoadBalancer();

  return qztest::Summary("test_load_balancer");
}
