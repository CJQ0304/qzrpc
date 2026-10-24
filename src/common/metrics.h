// 轻舟 RPC (Qingzhou RPC)
// src/common/metrics.h
//
// 职责：框架内建指标系统。phxrpc 的 Monitor 是一堆默认空的虚函数，
//       本框架把它反过来：指标是框架自己维护的，`Monitor` 只是可选的导出适配层。
//
// 提供两类指标：
//   * Counter   —— 单调累计量（QPS 分子、错误数、拒绝数、连接数）。原子自增，无锁读。
//   * Histogram —— 延迟分布。用 32 个 2 的幂次桶做对数分桶，可算出 P50/P90/P99/P999，
//                  误差上界为一个桶宽（≤ 2 倍），对 RPC 延迟观测完全够用，
//                  且内存恒定（32 个 atomic），不会因为长尾导致内存膨胀。
//
// 线程安全：全部方法线程安全。Counter/Histogram 用 atomic 实现；
//           MetricsRegistry 的 GetXxx 内部加锁（只在初始化路径与偶尔的注册时调用）。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qzrpc {

// 单调递增计数器。
class Counter {
 public:
  Counter() = default;

  // 自增 n（默认 1）。线程安全，无锁。
  void Inc(std::uint64_t n = 1) noexcept;

  // 当前累计值。线程安全。
  std::uint64_t value() const noexcept;

  // 归零。线程安全。
  void Reset() noexcept;

 private:
  std::atomic<std::uint64_t> value_{0};
};

// 延迟直方图（微秒为单位，对数分桶）。
class Histogram {
 public:
  // 桶数量：桶 i 覆盖 [2^i, 2^(i+1)) 微秒，即 1us .. 2^32us(≈71 分钟)。
  static constexpr std::size_t kBucketCount = 32;

  Histogram() = default;

  // 记录一次观测值。线程安全，无锁。
  // 参数：value_us - 延迟**微秒**数（单位固定，不做换算）；超过 2^32 us 的值会落进最后一个桶。
  // 命名建议：注册直方图时名字带 `_us` 后缀（如 "qzrpc.server.request_latency_us"），
  // 否则看板读者无法从名字判断单位。
  void Observe(std::uint64_t value_us) noexcept;

  // 观测次数。
  std::uint64_t count() const noexcept;

  // 观测值总和（微秒），用于算平均值。
  std::uint64_t sum_us() const noexcept;

  // 观测到的最大值（微秒）。注意这是精确值，不受分桶影响。
  std::uint64_t max_us() const noexcept;

  // 分位数，p ∈ [0,1]（例如 0.99 表示 P99）。
  // 返回：近似值，count == 0 时返回 0。
  // 算法：定位到样本所在的桶，取 `min(桶上界, max_us())`。
  //   - 取桶上界而不是桶下界，是为了让分位数偏保守（报大不报小）；
  //   - **再用 max_us() 封顶**，否则会算出 > 实际最大值的分位数（单样本 5us 会落在 [4,8)
  //     桶、报成 7us；更极端地，一个 10s 的长尾落在末尾桶会被报成 4.29s），
  //     运维看到 `P99 > max` 只会怀疑指标坏了。
  // 若将来要改成"纯桶上界"，必须同时接受"分位数可能大于精确最大值"这一反直觉现象。
  std::uint64_t Percentile(double p) const noexcept;

  // 平均值（微秒），count == 0 时返回 0。
  std::uint64_t MeanUs() const noexcept;

  // 归零。
  void Reset() noexcept;

  // 桶 i 的观测次数（供 Prometheus histogram 导出）。
  std::uint64_t bucket_count(std::size_t i) const noexcept;

 private:
  std::array<std::atomic<std::uint64_t>, kBucketCount> buckets_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_us_{0};
  std::atomic<std::uint64_t> max_us_{0};
};

// 全局指标注册表（单例）。
// 生命周期：进程级常驻，GetCounter/GetHistogram 返回的指针永不失效，可在热路径缓存。
class MetricsRegistry {
 public:
  // 取全局实例。线程安全。
  static MetricsRegistry& Instance();

  // 取（必要时创建）一个计数器。
  // 参数：name - **注册名**，建议点分格式（"qzrpc.server.request_total"）便于代码里阅读。
  //       注意点分名只是内部注册键；Prometheus 指标名只允许 [a-zA-Z0-9_:]，
  //       DumpText() 会把 `.` / `-` 统一替换为 `_`（→ `qzrpc_server_request_total`）。
  //       也支持调用方自带 label：`"name{code=\"OK\"}"`，此时只清洗 `{` 之前的部分。
  //       **面板/抓取端要按导出后的下划线名配置，不是点分名。**
  // 返回：非空指针，生命周期同进程。线程安全。
  Counter* GetCounter(const std::string& name);

  // 取（必要时创建）一个直方图。参数与命名规则同 GetCounter。线程安全。
  Histogram* GetHistogram(const std::string& name);

  // 导出为 Prometheus 文本格式（/metrics 端点直接返回它）。
  // 除标准 histogram 族（_bucket/_count/_sum）外，还会为每个直方图额外导出便捷 gauge：
  //   <name>_p50 / _p90 / _p99 / _p999 / _max_us / _mean_us
  // 这样前端与告警规则不必自己从 le 边界反推分位数（反推需要假设置信区间，容易算错）。
  // 注意：这些 gauge 与同名 histogram 不属于同一指标族（Prometheus 要求族内类型一致）。
  std::string DumpText() const;

  // 导出为 JSON（便于前端页面 / 简单 HTTP 接口直接吃）。
  std::string DumpJson() const;

  // 清空全部指标（仅测试使用）。
  void ResetAll();

 private:
  MetricsRegistry() = default;

  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<Counter>> counters_;
  std::map<std::string, std::unique_ptr<Histogram>> histograms_;
};

}  // namespace qzrpc
