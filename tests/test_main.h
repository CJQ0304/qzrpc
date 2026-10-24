// 轻舟 RPC (Qingzhou RPC)
// tests/test_main.h
//
// 极简单元测试框架：不引入 gtest/catch2（项目的硬约束是零第三方依赖）。
//
// 用法：
//   #include "test_main.h"
//
//   void TestSomething() {
//     QZ_CHECK(ptr != nullptr);          // 失败则打印并 return（后续断言不再执行）
//     QZ_CHECK_EQ(actual, expected);     // 失败则打印 文件:行 + 两侧取值，并 return
//     QZ_EXPECT(flag);                   // 失败只记录，继续执行下面的用例
//   }
//
//   int main() {
//     TestSomething();
//     return qztest::Summary("test_xxx");
//   }
//
// 设计取舍：
//   * QZ_CHECK/QZ_CHECK_EQ 是"致命"断言（内部 return），所以只能用在返回 void 的函数里；
//     致命断言的价值在于失败点之后不再产生一串连带的噪音失败。
//   * 计数用 inline 变量（C++17），头文件即整个框架，不需要单独的 .cpp。
//   * Summary 在全部通过时返回 0，否则返回 1 —— 直接作为进程退出码给 ctest 用。

#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>

namespace qztest {

inline int g_checks = 0;
inline int g_failures = 0;

// 把任意可流式输出的值转成字符串；char 系列按数字打印（否则会打出不可见字符）。
template <typename T>
std::string Str(const T& value) {
  using Decayed = std::decay_t<T>;
  std::ostringstream oss;
  if constexpr (std::is_same_v<Decayed, char> || std::is_same_v<Decayed, signed char> ||
                std::is_same_v<Decayed, unsigned char>) {
    oss << static_cast<int>(value);
  } else if constexpr (std::is_same_v<Decayed, bool>) {
    oss << (value ? "true" : "false");
  } else {
    oss << value;
  }
  return oss.str();
}

inline void ReportPass() { ++g_checks; }

inline void ReportFailure(const char* file, int line, const std::string& what) {
  ++g_checks;
  ++g_failures;
  std::printf("FAIL %s:%d: %s\n", file, line, what.c_str());
}

// 打印汇总并返回进程退出码。
inline int Summary(const char* suite) {
  if (g_failures == 0) {
    std::printf("[PASS] %s: %d checks\n", suite, g_checks);
    return 0;
  }
  std::printf("[FAIL] %s: %d of %d checks failed\n", suite, g_failures, g_checks);
  return 1;
}

}  // namespace qztest

// 软断言：记录失败，继续执行。
#define QZ_EXPECT(cond)                                                                  \
  do {                                                                                   \
    if (cond) {                                                                          \
      ::qztest::ReportPass();                                                            \
    } else {                                                                             \
      ::qztest::ReportFailure(__FILE__, __LINE__, std::string("expected: ") + #cond);     \
    }                                                                                    \
  } while (0)

// 致命断言：记录失败后从当前测试函数返回。
#define QZ_CHECK(cond)                                                                   \
  do {                                                                                   \
    if (cond) {                                                                          \
      ::qztest::ReportPass();                                                            \
    } else {                                                                             \
      ::qztest::ReportFailure(__FILE__, __LINE__, std::string("assertion failed: ") + #cond); \
      return;                                                                            \
    }                                                                                    \
  } while (0)

// 致命相等断言：失败时把两侧取值一起打印出来。
#define QZ_CHECK_EQ(actual, expected)                                                    \
  do {                                                                                   \
    const auto& qz_actual = (actual);                                                    \
    const auto& qz_expected = (expected);                                                \
    if (qz_actual == qz_expected) {                                                      \
      ::qztest::ReportPass();                                                            \
    } else {                                                                             \
      ::qztest::ReportFailure(                                                           \
          __FILE__, __LINE__,                                                            \
          std::string(#actual " == " #expected " (actual=") + ::qztest::Str(qz_actual) +  \
              ", expected=" + ::qztest::Str(qz_expected) + ")");                          \
      return;                                                                            \
    }                                                                                    \
  } while (0)
