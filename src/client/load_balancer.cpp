// 轻舟 RPC (Qingzhou RPC)
// src/client/load_balancer.cpp
//
// 职责：三种候选集选择策略。它们被多线程并发 Select，同时被注册中心线程
//       UpdateEndpoints —— 这个"高频读 + 低频写"的形态决定了实现方式。
//
// ★ 一致性哈希的两条硬约束（团队裁决）：
//   1) **锁外构造环，锁内换指针**。重建 160*N 个虚拟节点要算 160*N 次 FNV-1a，
//      持锁重建会让并发 Select 全部排队；正确做法是在锁外把整份 RingData 建好，
//      进锁只做一次 shared_ptr 交换（O(1)）。
//   2) **Select 不持锁**。拿到 shared_ptr 快照就解锁，查找在不可变数据上做 ——
//      快照不可变，所以不需要任何同步，也不会读到"半个环"。
//
// 线程安全：三个策略的全部方法线程安全（内部 mutex / atomic / 不可变快照）。

#include "client/load_balancer.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/string_util.h"

namespace qzrpc {
namespace rpc {

namespace {

// 判断 endpoints 里是否已有该地址（Endpoint 没有 operator==，只能逐字段比）。
bool Contains(const std::vector<net::Endpoint>& eps, const net::Endpoint& ep) {
  return std::find_if(eps.begin(), eps.end(), [&ep](const net::Endpoint& e) {
           return e.port == ep.port && e.ip == ep.ip;
         }) != eps.end();
}

// 去重并丢弃非法端点（port == 0 / ip 为空）。注册中心可能返回半残数据，
// 在入口处一次性丢掉，Select 里就不必再判。
std::vector<net::Endpoint> Sanitize(const std::vector<net::Endpoint>& in) {
  std::vector<net::Endpoint> out;
  out.reserve(in.size());
  for (const net::Endpoint& ep : in) {
    if (ep.port == 0 || ep.ip.empty()) continue;
    if (Contains(out, ep)) continue;
    out.push_back(ep);
  }
  return out;
}

}  // namespace

// 虚析构必须有定义：它是唯一允许出现在 .cpp 里的基类成员。
LoadBalancer::~LoadBalancer() = default;

// ---------------------------------------------------------------------------
// 轮询
// ---------------------------------------------------------------------------

struct RoundRobinBalancer::Impl {
  mutable std::mutex mutex;
  std::vector<net::Endpoint> endpoints;
  // 全局游标。用 atomic 是为了让"取号"这一步不进锁 —— 高 QPS 下这把锁是热点。
  std::atomic<std::uint64_t> cursor{0};
};

RoundRobinBalancer::RoundRobinBalancer() : impl_(new Impl()) {}

RoundRobinBalancer::~RoundRobinBalancer() = default;

void RoundRobinBalancer::UpdateEndpoints(const std::vector<net::Endpoint>& endpoints) {
  std::vector<net::Endpoint> clean = Sanitize(endpoints);
  std::lock_guard<std::mutex> lk(impl_->mutex);
  impl_->endpoints.swap(clean);
}

bool RoundRobinBalancer::Select(const std::string& key, std::size_t attempt,
                                net::Endpoint* out) {
  (void)key;  // 轮询不看 key
  if (out == nullptr) return false;

  // 先取号再判空：即使列表为空也消耗一个号，避免"空列表期间所有线程都拿到 0"，
  // 列表恢复后立刻出现一批调用打到同一个实例。
  const std::uint64_t ticket = impl_->cursor.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(impl_->mutex);
  if (impl_->endpoints.empty()) return false;
  // ★ **不要**在这里加 attempt 偏移。曾经写成 (ticket + attempt) % size，
  //   而 ticket 每调用一次 Select 就 +1、attempt 每次重试也 +1 —— 两个增量在同一个模里
  //   互相抵消：size == 2 时 (0,0)(1,1)(2,2) 全部落到下标 0，重试永远打在同一个实例上。
  //   实测症状：双实例挂一个时 20 次调用 10 成功 / 7388ms（本该 20 成功 / 200ms 量级），
  //   而三实例反而看不出问题（(0,0)(1,1)(2,2) → 0,2,1 会轮），属于最难在验收里暴露的那类。
  //   游标本身已经保证"相邻两次 Select 落在相邻下标"，这就是我们需要的换实例语义。
  (void)attempt;  // 其他策略（随机/一致性哈希）用它做避让，轮询不需要
  const std::size_t index = static_cast<std::size_t>(ticket % impl_->endpoints.size());
  *out = impl_->endpoints[index];
  return true;
}

std::size_t RoundRobinBalancer::size() const noexcept {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->endpoints.size();
}

// ---------------------------------------------------------------------------
// 随机
// ---------------------------------------------------------------------------

struct RandomBalancer::Impl {
  explicit Impl(std::uint64_t seed) : rng(seed != 0 ? seed : std::random_device{}()) {}

  mutable std::mutex mutex;
  std::vector<net::Endpoint> endpoints;
  std::mt19937_64 rng;
  // 上一次选中的下标，用于 attempt>0 时"尽力避开同一个实例"。
  std::size_t last_index{0};
};

RandomBalancer::RandomBalancer(std::uint64_t seed) : impl_(new Impl(seed)) {}

RandomBalancer::~RandomBalancer() = default;

void RandomBalancer::UpdateEndpoints(const std::vector<net::Endpoint>& endpoints) {
  std::vector<net::Endpoint> clean = Sanitize(endpoints);
  std::lock_guard<std::mutex> lk(impl_->mutex);
  impl_->endpoints.swap(clean);
  impl_->last_index = 0;
}

bool RandomBalancer::Select(const std::string& key, std::size_t attempt, net::Endpoint* out) {
  (void)key;  // 随机策略不看 key
  if (out == nullptr) return false;

  std::lock_guard<std::mutex> lk(impl_->mutex);
  const std::size_t n = impl_->endpoints.size();
  if (n == 0) return false;

  std::size_t index = 0;
  if (n == 1) {
    index = 0;
  } else {
    std::uniform_int_distribution<std::size_t> dist(0, n - 1);
    index = dist(impl_->rng);
    if (attempt > 0 && index == impl_->last_index) {
      // 重试却抽回同一个实例：在 [0, n-2] 里取一个，再跳过 last_index 映射回去。
      // 这样"避开上一次"是确定的，而不是靠多抽几次碰运气。
      std::uniform_int_distribution<std::size_t> retry_dist(0, n - 2);
      index = retry_dist(impl_->rng);
      if (index >= impl_->last_index) ++index;
    }
  }
  impl_->last_index = index;
  *out = impl_->endpoints[index];
  return true;
}

std::size_t RandomBalancer::size() const noexcept {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->endpoints.size();
}

// ---------------------------------------------------------------------------
// 一致性哈希
// ---------------------------------------------------------------------------

namespace {

// 环的不可变快照。构造完成后绝不修改，因此可以在锁外被任意线程并发查询。
struct HashRing {
  std::vector<net::Endpoint> endpoints;
  // 环位置 -> endpoints 下标。用 map 而不是排序 vector：刷新时插入 N*160 个节点，
  // map 的插入是 O(log n) 且不需要二次排序，刷新延迟更稳定。
  std::map<std::uint64_t, std::size_t> ring;
};

using HashRingPtr = std::shared_ptr<const HashRing>;

// FNV-1a 的**雪崩补强**（splitmix64 finalizer）。
//
// ★ 这不是画蛇添足，是自检实测出来的必要修正：
//   FNV-1a64 对"只差最后一个字节"的短 key 雪崩极差。实测 "user-0".."user-199"
//   的哈希全部落在 f84b9e.. ~ f84ba5.. 这个 2^40 宽的窄带里（高 16 位完全相同），
//   而 480 个虚拟节点的平均间距约 2^55 —— 于是 200 个 key **全部**落进同一段环区间，
//   整个一致性哈希退化成"永远打到同一个实例"。
//   这个缺陷不会报错、不会崩，只会让负载长期压在一个节点上，是最难被发现的那类问题。
//   加一轮 finalizer 后分布恢复到接近均匀（3→4 节点时 key 迁移率约 25%）。
//
// 可选方案对比：
//   * 换哈希（xxhash/murmurhash）：要多写几十行且引入新的"两个实现必须一致"的面；
//   * 在 FNV 上补一轮 finalizer：10 行、无新依赖、仍然是 FNV-1a 派生值。
//   选后者。**环位置与 key 必须用同一个混合函数**，否则两者落在不同的数值域，
//   一旦只改一边就会整体错位（比不混合更糟）。
std::uint64_t Mix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// 环位置：FNV-1a 派生 + 雪崩补强。
std::uint64_t RingPosition(const std::string& text) noexcept {
  return Mix64(StringUtil::Fnv1a64(text));
}

std::string VirtualNodeKey(const net::Endpoint& ep, std::size_t index) {
  std::string s = ep.ip;
  s += ':';
  s += std::to_string(ep.port);
  s += '#';
  s += std::to_string(index);
  return s;
}

// 在环上定位。空环返回 false。
bool LookupRing(const HashRing& ring, std::uint64_t hash, net::Endpoint* out) {
  if (ring.ring.empty()) return false;
  auto it = ring.ring.lower_bound(hash);
  if (it == ring.ring.end()) it = ring.ring.begin();  // 环形回绕
  *out = ring.endpoints[it->second];
  return true;
}

}  // namespace

struct ConsistentHashBalancer::Impl {
  explicit Impl(std::size_t vnodes) : virtual_nodes(vnodes == 0 ? 1 : vnodes) {}

  // 锁内只保护 data 指针；环本身不可变。
  mutable std::mutex mutex;
  HashRingPtr data;
  std::size_t virtual_nodes;
  // 空 key（未指定哈希键）时退化为轮询。
  std::atomic<std::uint64_t> cursor{0};
};

ConsistentHashBalancer::ConsistentHashBalancer(std::size_t virtual_nodes)
    : impl_(new Impl(virtual_nodes)) {
  impl_->data = std::make_shared<const HashRing>();
}

ConsistentHashBalancer::~ConsistentHashBalancer() = default;

void ConsistentHashBalancer::UpdateEndpoints(const std::vector<net::Endpoint>& endpoints) {
  const std::vector<net::Endpoint> clean = Sanitize(endpoints);

  // ★ 锁外构造整份新环。160*N 次 FNV 计算 + N*160 次 map 插入都在这段里完成，
  //   完全不碰 impl_->mutex，因此并发 Select 不会被刷新阻塞。
  std::shared_ptr<HashRing> fresh = std::make_shared<HashRing>();
  fresh->endpoints = clean;
  for (std::size_t i = 0; i < clean.size(); ++i) {
    for (std::size_t v = 0; v < impl_->virtual_nodes; ++v) {
      const std::uint64_t h = RingPosition(VirtualNodeKey(clean[i], v));
      // 哈希碰撞时保留先到的节点：丢掉后到的即可，环的均匀性不受影响
      // （碰撞本身意味着两个虚拟节点落在同一位置，谁赢都一样）。
      fresh->ring.emplace(h, i);
    }
  }

  // ★ 锁内只做一次指针交换。
  HashRingPtr old;
  {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    old.swap(impl_->data);
    impl_->data = std::move(fresh);
  }
  // old 在这里析构：可能是一次几十 KB 的释放，**不在锁内**做。
}

bool ConsistentHashBalancer::Select(const std::string& key, std::size_t attempt,
                                    net::Endpoint* out) {
  if (out == nullptr) return false;

  // 取一次快照就解锁：后续查找在不可变数据上做，无锁。
  HashRingPtr snapshot;
  {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    snapshot = impl_->data;
  }
  if (!snapshot || snapshot->endpoints.empty()) return false;

  if (key.empty()) {
    // 没有哈希键 → 退化为轮询（契约要求，不是偷懒）。
    //
    // ★ 这里**不要**加 attempt 偏移，与 RoundRobinBalancer::Select 同一个坑：
    //   cursor 每调用一次就 +1、attempt 每次重试也 +1，两个增量在同一个模里互相抵消，
    //   size == 2 时 (0,0)(1,1)(2,2) 全部落到下标 0 —— 重试永远打回同一个实例，
    //   而那正是上一次失败的那个。三实例时 (0,2,1) 会轮，所以验收脚本很难撞上；
    //   曾经就是靠"双实例挂一个，10/20 成功、耗时 7000ms"才定位到的。
    //   重试要避开已失败的节点，靠 Channel::Call 里的 failed_endpoints 去重，
    //   不靠这里偏移。
    const std::uint64_t ticket = impl_->cursor.fetch_add(1, std::memory_order_relaxed);
    *out = snapshot->endpoints[static_cast<std::size_t>(ticket % snapshot->endpoints.size())];
    return true;
  }

  std::uint64_t hash = RingPosition(key);
  if (attempt > 0) {
    // 重试换节点：把 attempt 混进 key 再算一次。确定性映射意味着
    // "同一个 key 的第 n 次重试"总是落在同一个节点上，便于复现问题。
    hash = RingPosition(key + "#retry" + std::to_string(attempt));
  }
  return LookupRing(*snapshot, hash, out);
}

std::size_t ConsistentHashBalancer::size() const noexcept {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->data ? impl_->data->endpoints.size() : 0;
}

std::size_t ConsistentHashBalancer::virtual_nodes() const noexcept {
  return impl_->virtual_nodes;
}

// ---------------------------------------------------------------------------
// 枚举解析与工厂
// ---------------------------------------------------------------------------

LoadBalancerType ParseLoadBalancerType(const std::string& s, std::string* err) {
  const std::string lower = StringUtil::ToLower(s);
  if (lower.empty() || lower == "round_robin" || lower == "roundrobin" || lower == "rr") {
    return LoadBalancerType::kRoundRobin;
  }
  if (lower == "random" || lower == "rand") {
    return LoadBalancerType::kRandom;
  }
  if (lower == "consistent_hash" || lower == "consistenthash" || lower == "ch") {
    return LoadBalancerType::kConsistentHash;
  }
  if (err != nullptr) {
    *err = "unknown load balancer '" + s +
           "', expected round_robin / random / consistent_hash; falling back to round_robin";
  }
  return LoadBalancerType::kRoundRobin;
}

std::unique_ptr<LoadBalancer> CreateLoadBalancer(LoadBalancerType type,
                                                std::size_t virtual_nodes) {
  switch (type) {
    case LoadBalancerType::kRandom:
      return std::unique_ptr<LoadBalancer>(new RandomBalancer());
    case LoadBalancerType::kConsistentHash:
      return std::unique_ptr<LoadBalancer>(new ConsistentHashBalancer(virtual_nodes));
    case LoadBalancerType::kRoundRobin:
    default:
      return std::unique_ptr<LoadBalancer>(new RoundRobinBalancer());
  }
}

}  // namespace rpc
}  // namespace qzrpc
