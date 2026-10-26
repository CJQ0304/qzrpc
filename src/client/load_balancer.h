// 轻舟 RPC (Qingzhou RPC)
// src/client/load_balancer.h
//
// 职责：在候选 endpoint 集合上做选择。三种策略 + 可插拔接口。
//
// 契约（实现者必须遵守）：
//   * **`Select` 每被调用一次，就应当推进内部选择状态**（轮询/随机天然满足）。
//     调用方**依赖**这一点来实现"重试换实例"：它在一次调用内连续 `Select` 并跳过
//     已失败的 endpoint，**不做任何 attempt 记账**。反过来说，如果某个实现让
//     "连续两次 Select 返回同一实例"成为常态，重试就会一直打在同一个故障节点上
//     ——故障转移会静默失效（不报错、不超时、只是成功率对折）；
//   * `attempt` 参数**仅供需要"按尝试次数避让"的策略**使用（随机、一致性哈希），
//     **轮询实现可以完全忽略它**（当前实现就是 `(void)attempt`）。调用方与测试
//     **不得假设"不同 attempt 必然返回不同 endpoint"**——`size() == 1` 时必然相同，
//     `size() == n` 时轮询的换实例语义来自游标推进而非 attempt；
//   * **取号与判空是两步，顺序不能换**：实现先 `fetch_add` 取一个序号，再取快照判空。
//     因此**列表为空期间每调用一次 Select 也消耗一个号**。这不是漏判：若把取号挪到
//     判空之后，空列表期间没有线程推进游标，列表恢复后一批线程会同时拿到 0，
//     瞬间全打到同一个实例——那正是轮询要避免的"一拥而上"。序号出现空洞的代价
//     （恢复后下标不连续，最多让分布偏一格）远小于集中打点的代价；
//   * UpdateEndpoints 会在注册中心刷新时被调用（可能来自后台线程），
//     实现必须自己保证线程安全；允许做 CPU 开销的重建（一致性哈希要算 160*N 次哈希），
//     但**不得做 IO**（不能在里面建连接），且重建过程不能让并发 Select 长时间排队；
//   * endpoint 列表为空时 Select 返回 false（调用方据此回 kServiceUnavailable）。
//
// 线程安全：接口方法均要求线程安全（可能被多个调用线程同时 Select）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/socket_util.h"

namespace qzrpc {
namespace rpc {

// 负载均衡策略接口。
class LoadBalancer {
 public:
  virtual ~LoadBalancer();

  // 更新候选 endpoint 列表。允许传入空列表（表示当前无可用实例）。
  virtual void UpdateEndpoints(const std::vector<net::Endpoint>& endpoints) = 0;

  // 选择 endpoint。**每调用一次就推进内部状态**（见文件头契约第一条）。
  // 参数：key - 哈希键（仅一致性哈希策略使用，通常用请求的业务主键；
  //             为空串时一致性哈希退化为轮询）；
  //       attempt - 第几次尝试，从 0 开始。**轮询策略忽略它**；只有"按尝试次数避让"
  //                 的策略（随机 / 一致性哈希）才读。不得据此推断返回值的差异；
  //       out - 输出选中的 endpoint（非空）。
  // 返回：true 选中；false 表示当前没有可用 endpoint（此时内部游标**已**推进）。
  virtual bool Select(const std::string& key, std::size_t attempt, net::Endpoint* out) = 0;

  // 策略名（"round_robin" / "random" / "consistent_hash"），用于日志与指标。
  virtual const char* name() const noexcept = 0;

  // 当前候选数量。
  virtual std::size_t size() const noexcept = 0;
};

// 轮询：全局游标 `ticket % size`。**调用即推进**，因此"重试换实例"由调用方的连续
// Select 自然获得，本实现**完全忽略 attempt**。
// 为什么不能"在游标基础上偏移 attempt"（早期写法 `(ticket + attempt) % size`）：
// ticket 每次 Select +1、attempt 每次重试 +1，两个增量在同一个模里互相抵消 ——
// size == 2 时 (0,0)(1,1)(2,2) 全部落到下标 0，重试永远打回同一个实例；
// 三实例反而看不出问题（(0,0)(1,1)(2,2) → 0,2,1 会轮），属于最难在验收里暴露的一类。
class RoundRobinBalancer : public LoadBalancer {
 public:
  RoundRobinBalancer();
  ~RoundRobinBalancer() override;

  void UpdateEndpoints(const std::vector<net::Endpoint>& endpoints) override;
  bool Select(const std::string& key, std::size_t attempt, net::Endpoint* out) override;
  const char* name() const noexcept override { return "round_robin"; }
  std::size_t size() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 随机：均匀随机，每次调用都重抽一次（天然满足"调用即推进"）。
// attempt > 0 时**尽力避开上一次**选中的实例：n > 1 时是**确定**避开（在 [0, n-2] 里抽
// 一个再跳过 last_index 映射回去，不是靠多抽几次碰运气）；n == 1 时只能选它。
// 注意这只是"避开上一次"，不保证与更早的历史选择不重复。
class RandomBalancer : public LoadBalancer {
 public:
  // 参数：seed - 随机种子，0 表示用时间与地址熵自动播种。
  explicit RandomBalancer(std::uint64_t seed = 0);
  ~RandomBalancer() override;

  void UpdateEndpoints(const std::vector<net::Endpoint>& endpoints) override;
  bool Select(const std::string& key, std::size_t attempt, net::Endpoint* out) override;
  const char* name() const noexcept override { return "random"; }
  std::size_t size() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 一致性哈希：虚拟节点环 + FNV-1a（带终态混合）。同一 key 恒定位到同一 endpoint（实例不变时）。
// 实现要点：
//   * 每个真实节点生成 virtual_nodes 个虚拟节点，环位置 = **环位置函数**（见下）。
//     虚拟节点的 key 是 `"ip:port#i"`，环位置与请求 key 必须用**同一个**环位置函数，
//     否则两者落在不同数值域，一旦只改一边就整体错位（比不混合更糟）；
//   * **环位置 = FNV-1a64 之后再跑一轮 5 步终态混合（splitmix64 finalizer）**，具体实现见 .cpp。
//     为什么不能只用裸 FNV-1a：它对"只差末几个字符"的短 key 雪崩极差，实测
//     `user-0`..`user-199` 的哈希全部落在 2^40 宽的窄带里，导致 200 个 key 全部打到同一个实例
//     （不报错、不崩、只是负载长期歪在一个节点上）。混合后分布恢复接近均匀。
//     混合的具体常量与步骤**属于实现细节，可以改**，改的时候必须两边一起改。
//   * ⚠️ **这个混合只影响环位置，绝不影响 `MethodIdOfName`。** 后者是跨语言契约
//     （doc 03 §7.3 的 FNV-1a，Python/C++ 必须逐字一致），混用会导致 C++ 与 codegen
//     算出不同的 method_id —— 那是"调不到方法"级别的灾难，不是性能问题；
//   * 环用有序结构（`std::map<uint64, size_t>`）保存，查找 O(log n)；
//   * `UpdateEndpoints` **在锁外重建整份环**（160*N 次哈希 + 插入），锁内只做一次
//     `shared_ptr` 交换；`Select` 取一次快照就解锁，后续查找在不可变数据上做。
//     所以刷新期间并发 Select 不会被阻塞（早期设计稿里的"脏标记 + 惰性重建"没有采用）；
//   * 端点列表会先做一次清洗：丢弃 `port == 0` / `ip` 为空的项、按 `ip:port` 去重
//     （注册中心可能返回半残数据，清洗在入口做一次，`Select` 里就不必再判）；
//   * **`key` 为空时退化为轮询**（用内部 cursor），这是契约行为，不是兜底 bug：
//     没给哈希键就没有"亲和性"可谈，硬算一个固定位置只会让所有调用挤到同一实例。
//     退化分支与轮询策略**用同一套语义**：`ticket % size`，**忽略 attempt**
//     （曾经的 `(ticket + attempt) % size` 会与游标增量互相抵消，`size == 2` 时重试不换实例）。
class ConsistentHashBalancer : public LoadBalancer {
 public:
  // 参数：virtual_nodes - 每个真实节点的虚拟节点数，默认 160（与常见实现一致）。
  explicit ConsistentHashBalancer(std::size_t virtual_nodes = 160);
  ~ConsistentHashBalancer() override;

  void UpdateEndpoints(const std::vector<net::Endpoint>& endpoints) override;
  bool Select(const std::string& key, std::size_t attempt, net::Endpoint* out) override;
  const char* name() const noexcept override { return "consistent_hash"; }
  std::size_t size() const noexcept override;

  // 虚拟节点数。
  std::size_t virtual_nodes() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 策略枚举（配置里用字符串或数字指定）。
enum class LoadBalancerType {
  kRoundRobin = 0,
  kRandom = 1,
  kConsistentHash = 2,
};

// 字符串转枚举，大小写不敏感，接受 "round_robin"/"rr"、"random"、"consistent_hash"/"ch"。
// 无法识别时返回 kRoundRobin 并（可选）在 err 写提示。
LoadBalancerType ParseLoadBalancerType(const std::string& s, std::string* err = nullptr);

// 工厂。
// 参数：type - 策略；virtual_nodes - 仅一致性哈希用。
std::unique_ptr<LoadBalancer> CreateLoadBalancer(LoadBalancerType type,
                                                 std::size_t virtual_nodes = 160);

}  // namespace rpc
}  // namespace qzrpc
