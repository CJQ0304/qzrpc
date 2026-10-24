// 轻舟 RPC (Qingzhou RPC)
// src/client/load_balancer.h
//
// 职责：在候选 endpoint 集合上做选择。三种策略 + 可插拔接口。
//
// 契约（实现者必须遵守）：
//   * Select 的 attempt 参数表示"这是第几次尝试"（0 表示首次）。对同一 key、
//     不同 attempt 应尽量返回**不同**的 endpoint，这样重试才有意义；
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

  // 选择 endpoint。
  // 参数：key - 哈希键（仅一致性哈希策略使用，通常用请求的业务主键；
  //             为空串时一致性哈希退化为轮询）；
  //       attempt - 第几次尝试，从 0 开始；
  //       out - 输出选中的 endpoint（非空）。
  // 返回：true 选中；false 表示当前没有可用 endpoint。
  virtual bool Select(const std::string& key, std::size_t attempt, net::Endpoint* out) = 0;

  // 策略名（"round_robin" / "random" / "consistent_hash"），用于日志与指标。
  virtual const char* name() const noexcept = 0;

  // 当前候选数量。
  virtual std::size_t size() const noexcept = 0;
};

// 轮询：全局计数器取模。attempt>0 时在计数器基础上偏移 attempt，保证重试换节点。
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

// 随机：均匀随机。attempt>0 时排除上一次的选择（尽力而为，不保证全局不重复）。
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
