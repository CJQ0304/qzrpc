// 轻舟 RPC (Qingzhou RPC)
// src/common/time_util.h
//
// 职责：统一的时间源。框架内**只允许**通过本文件取时间，禁止直接调 std::chrono::system_clock
//       或 time()，以保证：
//         1) 计算耗时/超时一律用单调时钟（steady clock），不受系统时间调整影响；
//         2) 单元测试可以注入假时钟（通过 SetNowMsForTest）。
//
// 线程安全：全部静态方法线程安全；测试注入点使用 atomic，任意线程可调用。
#pragma once

#include <cstdint>
#include <string>

#include "common/noncopyable.h"

namespace qzrpc {

// 时间工具集合（纯静态，禁止实例化）。
class TimeUtil : private StaticOnly {
 public:
  // 单调时钟毫秒（等价 std::chrono::steady_clock）。用于所有超时/耗时计算。
  // 返回：从某个任意起点开始的毫秒数，只保证单调递增。
  static std::uint64_t NowMs() noexcept;

  // 单调时钟微秒。用于 metrics 的延迟采样，避免毫秒精度不足。
  static std::uint64_t NowUs() noexcept;

  // 墙上时钟 Unix 毫秒。仅用于日志时间戳与跨机 deadline 传递，**不得**用于算耗时。
  static std::uint64_t UnixMs() noexcept;

  // 挂起当前线程。
  // 参数：ms - 毫秒，0 表示仅让出时间片。
  static void SleepMs(std::uint32_t ms);

  // 把 Unix 毫秒格式化为 "2024-05-01 12:34:56.789"（本地时区）。
  // 返回：定长 23 字符的字符串。
  static std::string FormatUnixMs(std::uint64_t unix_ms);

  // 把毫秒时长格式化为 "1.234s" / "12ms" 这类便于日志阅读的短串。
  static std::string FormatDuration(std::uint64_t ms);

  // ---- 仅用于单元测试 ----
  // 把 NowMs/NowUs 的读数整体偏移 delta_ms（可为负）。
  // 参数：delta_ms - 偏移量；传 0 恢复真实时钟。
  // 线程安全：是。生产代码禁止调用。
  static void SetNowMsForTest(std::int64_t delta_ms) noexcept;

  // 取当前测试偏移量，供测试断言。
  static std::int64_t now_ms_test_delta() noexcept;
};

}  // namespace qzrpc
