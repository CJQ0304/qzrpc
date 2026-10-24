// 轻舟 RPC (Qingzhou RPC)
// src/common/logger.cpp
//
// 日志实现。要点（对应 docs/00-file-map.md C-01）：
//   * Impl 持 std::mutex + FILE*，Write() **组装完整一行**后一次 fwrite，
//     保证多线程下日志行不交错（这是"一次加锁 + 一次写"的全部意义）。
//   * kFatal 写完立刻 abort()：这类日志只用于"继续跑下去毫无意义"的编程错误。
//   * 未打开文件时自动落到 stderr；打开文件成功后仍可按 SetAlsoLogToStderr 决定是否回显。
//   * 不做异步落盘队列：v1 追求"崩溃前的最后一行一定在文件里"，所以每行 fflush。
//
// MSVC 注意：fopen/localtime 之类的 CRT 函数在 /W4 下会报 C4996（deprecated），
// 本项目不允许用 #pragma warning(disable) 掩盖警告，因此这里用 fopen_s 分支。

#include "common/logger.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "common/time_util.h"

namespace qzrpc {
namespace {

// 当前线程 id，仅用于日志行内的可读标识，不参与任何逻辑判断。
unsigned long CurrentThreadId() noexcept {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentThreadId());
#elif defined(__linux__)
  return static_cast<unsigned long>(::syscall(SYS_gettid));
#else
  return static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

// 只保留文件名部分：日志里写 "buffer.cpp:123" 比写完整绝对路径可读得多。
const char* BaseName(const char* path) noexcept {
  if (path == nullptr) {
    return "";
  }
  const char* base = path;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

}  // namespace

struct Logger::Impl {
  std::mutex mutex;
  std::FILE* file{nullptr};
  bool also_stderr{true};
  std::atomic<int> level{static_cast<int>(Logger::kInfo)};
};

Logger& Logger::Instance() {
  static Logger instance;
  return instance;
}

Logger::Logger() : impl_(new Impl()) {}

Logger::~Logger() {
  if (impl_ != nullptr && impl_->file != nullptr) {
    std::fclose(impl_->file);
    impl_->file = nullptr;
  }
}

Logger::Level Logger::SetLevel(Level level) noexcept {
  const int previous = impl_->level.exchange(static_cast<int>(level), std::memory_order_relaxed);
  return static_cast<Level>(previous);
}

Logger::Level Logger::level() const noexcept {
  return static_cast<Level>(impl_->level.load(std::memory_order_relaxed));
}

bool Logger::SetLogFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->file != nullptr) {
    std::fclose(impl_->file);
    impl_->file = nullptr;
  }
  if (path.empty()) {
    return true;  // 空串 = 只输出到 stderr，是一个合法请求而不是失败
  }
  std::FILE* file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, path.c_str(), "ab") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "ab");
#endif
  if (file == nullptr) {
    // 打开失败 -> 降级为 stderr-only，不抛错、不终止进程。
    impl_->also_stderr = true;
    return false;
  }
  impl_->file = file;
  return true;
}

void Logger::SetAlsoLogToStderr(bool on) noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->also_stderr = on;
}

bool Logger::ShouldLog(Level level) const noexcept {
  // ⚠️ 契约歧义（已上报顾图南裁决）：
  //   logger.h 的 Level 注释写"级别 N 生效时会输出所有 <= N 的日志"，
  //   但同一枚举里 kOff = 6 的注释是"关闭所有日志"。两者不能同时成立：
  //     * 按 "<=" 字面实现，SetLevel(kOff) 会输出全部级别 —— 与"关闭"正好相反；
  //     * 按 "<=" 字面实现，默认 kInfo 会连 kTrace/kDebug 一起输出，
  //       "级别"这个旋钮就只剩下 Off 一个有效档位，其余全是"全开"。
  //   因此这里按**枚举的数值设计**实现（数值越大越严重，输出 level >= 当前级别），
  //   这是 kOff 能成立、且默认 kInfo 只输出 INFO/WARN/ERROR/FATAL 的唯一读法。
  //   若最终裁决以注释为准，只需把下面的 ">=" 改成 "<=" —— 过滤点只有这一处。
  const int current = impl_->level.load(std::memory_order_relaxed);
  if (current == static_cast<int>(kOff)) {
    return false;
  }
  return static_cast<int>(level) >= current;
}

void Logger::Write(Level level, const char* file, int line, const std::string& msg) noexcept {
  // QZ_LOG_* 宏无法在拼接前过滤级别（宏只负责构造 LogMessage），
  // 所以级别过滤的落点在这里：低于当前级别的日志一律丢弃。
  if (!ShouldLog(level)) {
    return;
  }

  std::string text;
  text.reserve(msg.size() + 72);
  text += '[';
  text += LevelName(level);
  text += "][";
  text += std::to_string(CurrentThreadId());
  text += "][";
  text += TimeUtil::FormatUnixMs(TimeUtil::UnixMs());
  text += "][";
  text += BaseName(file);
  text += ':';
  text += std::to_string(line);
  text += "] ";
  text += msg;
  text += '\n';

  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file != nullptr) {
      std::fwrite(text.data(), 1, text.size(), impl_->file);
      std::fflush(impl_->file);
    }
    if (impl_->also_stderr || impl_->file == nullptr) {
      std::fwrite(text.data(), 1, text.size(), stderr);
      std::fflush(stderr);
    }
  }

  if (level == kFatal) {
    std::fflush(nullptr);  // 冲刷所有已打开的流，保证崩溃现场完整
    std::abort();
  }
}

const char* Logger::LevelName(Level level) noexcept {
  switch (level) {
    case kTrace:
      return "TRACE";
    case kDebug:
      return "DEBUG";
    case kInfo:
      return "INFO";
    case kWarn:
      return "WARN";
    case kError:
      return "ERROR";
    case kFatal:
      return "FATAL";
    case kOff:
      return "OFF";
  }
  return "UNKNOWN";
}

LogMessage::LogMessage(Logger::Level level, const char* file, int line) noexcept
    : level_(level), file_(file), line_(line) {}

LogMessage::~LogMessage() {
  // 析构时一次性写出：流式拼接只发生在调用线程自己的 ostringstream 上，无锁。
  Logger::Instance().Write(level_, file_, line_, oss_.str());
}

}  // namespace qzrpc
