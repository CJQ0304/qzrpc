// 轻舟 RPC (Qingzhou RPC)
// src/common/time_util.cpp
//
// 时间源实现。要点（对应 docs/00-file-map.md C-02）：
//   * NowMs/NowUs 一律基于 std::chrono::steady_clock（单调，不受系统时间调整影响）；
//   * UnixMs 基于 system_clock，只用于日志时间戳与跨机 deadline 传递；
//   * 测试偏移量用 std::atomic<int64_t>，任意线程可读写；
//   * localtime_s(Windows) / localtime_r(POSIX) 的差异**只在这里 #ifdef**，
//     公共头文件保持零平台依赖。

#include "common/time_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

namespace qzrpc {
namespace {

// 测试用的时钟偏移量（毫秒）。生产路径上恒为 0，一次 relaxed load。
std::atomic<std::int64_t> g_now_ms_delta{0};

std::int64_t SteadyMicros() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

}  // namespace

std::uint64_t TimeUtil::NowMs() noexcept {
  const std::int64_t delta = g_now_ms_delta.load(std::memory_order_relaxed);
  const std::int64_t base = SteadyMicros() / 1000;
  const std::int64_t value = base + delta;
  return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

std::uint64_t TimeUtil::NowUs() noexcept {
  const std::int64_t delta = g_now_ms_delta.load(std::memory_order_relaxed);
  // 偏移量以"毫秒"为单位给出，换算成微秒后再加到单调时钟上，
  // 保证 NowUs 与 NowMs 的偏移方向、幅度完全一致。
  const std::int64_t value = SteadyMicros() + delta * 1000;
  return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

std::uint64_t TimeUtil::UnixMs() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  return ms > 0 ? static_cast<std::uint64_t>(ms) : 0U;
}

void TimeUtil::SleepMs(std::uint32_t ms) {
  if (ms == 0) {
    std::this_thread::yield();  // 0 表示"只让出时间片"，不做系统调用的定时等待
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string TimeUtil::FormatUnixMs(std::uint64_t unix_ms) {
  const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000U);
  const std::uint64_t millis = unix_ms % 1000U;

  std::tm tm_value{};
#if defined(_MSC_VER)
  if (localtime_s(&tm_value, &seconds) != 0) {
    return std::string("1970-01-01 00:00:00.000");
  }
#else
  if (localtime_r(&seconds, &tm_value) == nullptr) {
    return std::string("1970-01-01 00:00:00.000");
  }
#endif

  char buffer[32] = {0};
  const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_value);
  if (written == 0) {
    return std::string("1970-01-01 00:00:00.000");
  }
  // 定长 23 字符："YYYY-MM-DD HH:MM:SS.mmm"
  std::snprintf(buffer + written, sizeof(buffer) - written, ".%03u",
                static_cast<unsigned>(millis));
  return std::string(buffer);
}

std::string TimeUtil::FormatDuration(std::uint64_t ms) {
  char buffer[48] = {0};
  if (ms < 1000U) {
    std::snprintf(buffer, sizeof(buffer), "%llums", static_cast<unsigned long long>(ms));
    return std::string(buffer);
  }
  const std::uint64_t total_seconds = ms / 1000U;
  const std::uint64_t millis_part = ms % 1000U;
  if (total_seconds < 60U) {
    std::snprintf(buffer, sizeof(buffer), "%llu.%03llus",
                  static_cast<unsigned long long>(total_seconds),
                  static_cast<unsigned long long>(millis_part));
    return std::string(buffer);
  }
  const std::uint64_t total_minutes = total_seconds / 60U;
  const std::uint64_t seconds_part = total_seconds % 60U;
  if (total_minutes < 60U) {
    std::snprintf(buffer, sizeof(buffer), "%llum%llu.%03llus",
                  static_cast<unsigned long long>(total_minutes),
                  static_cast<unsigned long long>(seconds_part),
                  static_cast<unsigned long long>(millis_part));
    return std::string(buffer);
  }
  const std::uint64_t hours = total_minutes / 60U;
  const std::uint64_t minutes_part = total_minutes % 60U;
  std::snprintf(buffer, sizeof(buffer), "%lluh%llum%llu.%03llus",
                static_cast<unsigned long long>(hours),
                static_cast<unsigned long long>(minutes_part),
                static_cast<unsigned long long>(seconds_part),
                static_cast<unsigned long long>(millis_part));
  return std::string(buffer);
}

void TimeUtil::SetNowMsForTest(std::int64_t delta_ms) noexcept {
  g_now_ms_delta.store(delta_ms, std::memory_order_relaxed);
}

std::int64_t TimeUtil::now_ms_test_delta() noexcept {
  return g_now_ms_delta.load(std::memory_order_relaxed);
}

}  // namespace qzrpc
