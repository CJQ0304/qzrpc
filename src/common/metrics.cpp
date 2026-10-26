// 轻舟 RPC (Qingzhou RPC)
// src/common/metrics.cpp
//
// 指标实现。要点（对应 docs/00-file-map.md C-07）：
//   * Histogram 用 32 个 2 的幂次桶做对数分桶，桶 i 覆盖 [2^i, 2^(i+1)) 微秒，
//     内存恒定（32 个 atomic），不会被长尾撑爆；观察值 > 2^32 一律压到最后一个桶。
//   * Observe 的桶定位就是 `while (v >>= 1) ++i`（等价 BitScanReverse），
//     一次观测只有 1 次 CAS（维护 max）+ 3 次 fetch_add，热路径无锁。
//   * Percentile 在桶内取**桶上界**，并用精确的 max_us_ 做上封顶 ——
//     否则会出现"P99 大于最大值"这种自相矛盾的输出。
//   * DumpText 输出 Prometheus 文本格式（# HELP / # TYPE / name{label="v"} value），
//     example/web/index.html 直接解析它；指标名里的 '.' 是不合法字符，统一换成 '_'。

#include "common/metrics.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace qzrpc {
namespace {

// 桶 i 的取值上界（微秒）。bucket 31 覆盖 [2^31, 2^32)，上界 2^32-1；
// 所有更大的观察值都被压进这个桶，所以它同时也是全局可表示的上限。
std::uint64_t BucketUpperUs(std::size_t bucket) noexcept {
  if (bucket >= Histogram::kBucketCount) {
    return 0xFFFFFFFFULL;
  }
  return (static_cast<std::uint64_t>(1) << (bucket + 1)) - 1ULL;
}

// Prometheus 的指标名字符集是 [a-zA-Z0-9_:]，本项目用点分名（qzrpc.server.qps），
// 必须做替换。若名字里已含 '{'（调用方自己拼好了 label），只清洗 '{' 之前的部分。
std::string SanitizeName(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 8);
  bool in_labels = false;
  for (const char c : raw) {
    if (in_labels) {
      out.push_back(c);
      continue;
    }
    if (c == '{') {
      in_labels = true;
      out.push_back(c);
      continue;
    }
    const bool legal = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == ':';
    out.push_back(legal ? c : '_');
  }
  return out;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

// DumpText 里每条"便捷分位数"都作为独立的 gauge 指标族输出：
// Prometheus 文本格式要求同一指标族内所有样本类型一致，所以不能把 _p99 混进 histogram 族。
void AppendGauge(std::string* out, const std::string& name, const std::string& help,
                 std::uint64_t value) {
  *out += "# HELP " + name + " " + help + "\n";
  *out += "# TYPE " + name + " gauge\n";
  *out += name + " " + std::to_string(value) + "\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Counter
// ---------------------------------------------------------------------------

void Counter::Inc(std::uint64_t n) noexcept {
  value_.fetch_add(n, std::memory_order_relaxed);
}

std::uint64_t Counter::value() const noexcept {
  return value_.load(std::memory_order_relaxed);
}

void Counter::Reset() noexcept {
  value_.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

void Histogram::Observe(std::uint64_t value_us) noexcept {
  // 定位桶：floor(log2(value_us))。value 为 0 或 1 时落在桶 0。
  std::size_t index = 0;
  std::uint64_t shifted = value_us;
  while ((shifted >>= 1) != 0) {
    ++index;
  }
  if (index >= kBucketCount) {
    index = kBucketCount - 1;  // 超过 2^32us 的观察值压到最后一个桶
  }
  buckets_[index].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_us_.fetch_add(value_us, std::memory_order_relaxed);

  // max 用 CAS 更新：写入频率远低于读取，且只有"更大"时才重试。
  std::uint64_t previous = max_us_.load(std::memory_order_relaxed);
  while (value_us > previous &&
         !max_us_.compare_exchange_weak(previous, value_us, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
    // previous 已被 CAS 更新为最新值，循环条件重新判断
  }
}

std::uint64_t Histogram::count() const noexcept {
  return count_.load(std::memory_order_relaxed);
}

std::uint64_t Histogram::sum_us() const noexcept {
  return sum_us_.load(std::memory_order_relaxed);
}

std::uint64_t Histogram::max_us() const noexcept {
  return max_us_.load(std::memory_order_relaxed);
}

std::uint64_t Histogram::Percentile(double p) const noexcept {
  const std::uint64_t total = count();
  if (total == 0) {
    return 0;
  }
  if (!(p > 0.0)) {  // 同时覆盖 NaN
    p = 0.0;
  }
  if (p > 1.0) {
    p = 1.0;
  }
  // 目标名次（1-based）：ceil(p * count)，p=0 时至少取第 1 个样本。
  const double rank_real = std::ceil(p * static_cast<double>(total));
  std::uint64_t rank = rank_real < 1.0 ? 1U : static_cast<std::uint64_t>(rank_real);
  if (rank > total) {
    rank = total;
  }

  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    cumulative += buckets_[i].load(std::memory_order_relaxed);
    if (cumulative >= rank) {
      // 桶内取上界；再用精确最大值封顶，避免 P 值大于 max。
      const std::uint64_t upper = BucketUpperUs(i);
      const std::uint64_t maximum = max_us();
      return upper < maximum ? upper : maximum;
    }
  }
  return max_us();
}

std::uint64_t Histogram::MeanUs() const noexcept {
  const std::uint64_t total = count();
  if (total == 0) {
    return 0;
  }
  return sum_us() / total;
}

void Histogram::Reset() noexcept {
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    buckets_[i].store(0, std::memory_order_relaxed);
  }
  count_.store(0, std::memory_order_relaxed);
  sum_us_.store(0, std::memory_order_relaxed);
  max_us_.store(0, std::memory_order_relaxed);
}

std::uint64_t Histogram::bucket_count(std::size_t i) const noexcept {
  if (i >= kBucketCount) {
    return 0;
  }
  return buckets_[i].load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// MetricsRegistry
// ---------------------------------------------------------------------------

MetricsRegistry& MetricsRegistry::Instance() {
  static MetricsRegistry instance;
  return instance;
}

Counter* MetricsRegistry::GetCounter(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = counters_.find(name);
  if (it != counters_.end()) {
    return it->second.get();
  }
  std::unique_ptr<Counter> created(new Counter());
  Counter* raw = created.get();
  counters_.emplace(name, std::move(created));
  return raw;  // 生命周期同进程，可被热路径缓存
}

Histogram* MetricsRegistry::GetHistogram(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = histograms_.find(name);
  if (it != histograms_.end()) {
    return it->second.get();
  }
  std::unique_ptr<Histogram> created(new Histogram());
  Histogram* raw = created.get();
  histograms_.emplace(name, std::move(created));
  return raw;
}

std::string MetricsRegistry::DumpText() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  out.reserve(2048);

  for (const auto& item : counters_) {
    const std::string name = SanitizeName(item.first);
    out += "# HELP " + name + " Monotonic counter (" + item.first + ").\n";
    out += "# TYPE " + name + " counter\n";
    out += name + " " + std::to_string(item.second->value()) + "\n";
  }

  for (const auto& item : histograms_) {
    const std::string name = SanitizeName(item.first);
    const Histogram& histogram = *item.second;

    out += "# HELP " + name + " Latency histogram in microseconds (" + item.first + ").\n";
    out += "# TYPE " + name + " histogram\n";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < Histogram::kBucketCount; ++i) {
      cumulative += histogram.bucket_count(i);
      out += name + "_bucket{le=\"" + std::to_string(BucketUpperUs(i)) + "\"} " +
             std::to_string(cumulative) + "\n";
    }
    out += name + "_bucket{le=\"+Inf\"} " + std::to_string(histogram.count()) + "\n";
    out += name + "_sum " + std::to_string(histogram.sum_us()) + "\n";
    out += name + "_count " + std::to_string(histogram.count()) + "\n";

    // 便捷 gauge：前端页面不需要自己从桶边界反推分位数。
    AppendGauge(&out, name + "_p50", "Approximate 50th percentile latency (us).",
                histogram.Percentile(0.50));
    AppendGauge(&out, name + "_p90", "Approximate 90th percentile latency (us).",
                histogram.Percentile(0.90));
    AppendGauge(&out, name + "_p99", "Approximate 99th percentile latency (us).",
                histogram.Percentile(0.99));
    AppendGauge(&out, name + "_p999", "Approximate 99.9th percentile latency (us).",
                histogram.Percentile(0.999));
    AppendGauge(&out, name + "_max_us", "Max observed latency (us, exact).",
                histogram.max_us());
    AppendGauge(&out, name + "_mean_us", "Mean observed latency (us).", histogram.MeanUs());
  }
  return out;
}

std::string MetricsRegistry::DumpJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  out.reserve(1024);
  out += "{\"counters\":{";
  bool first = true;
  for (const auto& item : counters_) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += '"' + JsonEscape(item.first) + "\":" + std::to_string(item.second->value());
  }
  out += "},\"histograms\":{";
  first = true;
  for (const auto& item : histograms_) {
    if (!first) {
      out += ',';
    }
    first = false;
    const Histogram& histogram = *item.second;
    out += '"' + JsonEscape(item.first) + "\":{";
    out += "\"count\":" + std::to_string(histogram.count());
    out += ",\"sum_us\":" + std::to_string(histogram.sum_us());
    out += ",\"max_us\":" + std::to_string(histogram.max_us());
    out += ",\"mean_us\":" + std::to_string(histogram.MeanUs());
    out += ",\"p50\":" + std::to_string(histogram.Percentile(0.50));
    out += ",\"p90\":" + std::to_string(histogram.Percentile(0.90));
    out += ",\"p99\":" + std::to_string(histogram.Percentile(0.99));
    out += ",\"p999\":" + std::to_string(histogram.Percentile(0.999));
    out += '}';
  }
  out += "}}";
  return out;
}

void MetricsRegistry::ResetAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  // ★ 不能 counters_.clear() / histograms_.clear()。
  //
  // GetCounter/GetHistogram 的契约是"返回的指针**永不失效**"（见 metrics.h 的
  // MetricsRegistry 类注释），而 Server::Impl::RegisterMetricPointers 与
  // Channel::Impl 都按这个契约在启动时把裸 Counter* / Histogram* 缓存进成员，
  // 之后的每一次 Inc / Observe 都直接用缓存指针。clear() 会析构对象，
  // 缓存里全部变成悬垂指针 —— 下一次 Inc 就是 use-after-free。
  //
  // 更糟的是它只在"测试里 ResetAll 之后还有 Server/Channel 活着"时炸，
  // 而单测恰恰是这个用法（一个进程里连跑多个用例，每个用例起一套 Server）。
  // 正确做法：保留对象、只把值归零，这样 ResetAll 之后所有既有句柄继续可用。
  for (auto& kv : counters_) {
    if (kv.second) kv.second->Reset();
  }
  for (auto& kv : histograms_) {
    if (kv.second) kv.second->Reset();
  }
}

}  // namespace qzrpc
