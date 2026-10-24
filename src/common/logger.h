// 轻舟 RPC (Qingzhou RPC)
// src/common/logger.h
//
// 职责：框架唯一的日志出口。所有模块通过 QZ_LOG_* 宏写日志，禁止 printf/std::cout。
//
// 设计要点：
//   * 分级（Trace..Fatal）。kFatal 在写出日志后立即 abort()，用于不可恢复的编程错误。
//   * 输出格式固定为：`[级别][线程id][2024-05-01 12:34:56.789][file.cpp:123] 正文`。
//   * 同时支持"只写文件"与"文件 + stderr"两种模式（文件写失败自动降级为 stderr-only，不抛错）。
//   * 每行日志一次加锁、一次 write，保证多线程下行不交错；不做异步落盘队列（v1 追求简单可读）。
//
// 线程安全：全部方法线程安全。流式宏在单线程内拼接，析构时整体加锁写出。
#pragma once

#include <atomic>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

#include "common/noncopyable.h"

namespace qzrpc {

// 全局日志器（单例）。
class Logger : private NonCopyable {
 public:
  // 日志级别。数值递增，**级别 N 生效时输出所有 >= N 的日志**。
  // 例：kInfo 输出 Info/Warn/Error/Fatal，压制 Trace/Debug；kOff 关闭全部。
  // （早期版本的注释写成 "<= N"，那会让 kOff 输出全部日志、kInfo 连 TRACE 一起输出，
  //   级别旋钮退化为全开/全关，语义自相矛盾。以实现为准，注释已更正。）
  enum Level : int {
    kTrace = 0,
    kDebug = 1,
    kInfo = 2,
    kWarn = 3,
    kError = 4,
    kFatal = 5,
    kOff = 6,  // 关闭所有日志
  };

  // 取全局唯一实例。线程安全（C++11 函数内 static）。
  static Logger& Instance();

  // 设置级别，返回旧级别。线程安全。
  Level SetLevel(Level level) noexcept;

  // 当前级别。线程安全。
  Level level() const noexcept;

  // 设置日志文件路径。
  // 参数：path - 文件路径；空串表示只输出到 stderr。父目录必须已存在。
  // 返回：true 表示文件已打开；false 表示打开失败，此时自动降级为 stderr-only。
  bool SetLogFile(const std::string& path);

  // 是否启用 stderr 回显（默认 true）。设为 false 则只写文件。
  void SetAlsoLogToStderr(bool on) noexcept;

  // 该级别是否会被输出（用于避免昂贵的参数拼接）。
  bool ShouldLog(Level level) const noexcept;

  // 真正写出一行。一般由 LogMessage 调用，业务代码不应直接调用。
  // 参数：level - 级别；file - 源文件名（__FILE__）；line - 行号；msg - 已格式化正文。
  void Write(Level level, const char* file, int line, const std::string& msg) noexcept;

  // 级别名（"TRACE"/"DEBUG"/...）。永不返回 nullptr。
  static const char* LevelName(Level level) noexcept;

 private:
  Logger();
  ~Logger();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// RAII 流式日志消息：构造时记录元信息，析构时一次性写出。
// 用法：QZ_LOG_INFO << "connected to " << addr;
class LogMessage {
 public:
  LogMessage(Logger::Level level, const char* file, int line) noexcept;
  ~LogMessage();

  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(const LogMessage&) = delete;

  // 返回底层流，供 operator<< 使用。
  std::ostream& stream() noexcept { return oss_; }

 private:
  Logger::Level level_;
  const char* file_;
  int line_;
  std::ostringstream oss_;
};

// 把 ostream 吞掉的空操作对象。配合三目运算符实现"级别不匹配时连参数拼接都不做"。
// 原理：`pred ? (void)0 : Voidify() & LogMessage(...).stream() << args`
//   —— `<<` 优先级高于 `&`，所以右半边先得到一个 ostream&，
//      再被 operator& 吞成 void；三目两侧同为 void，表达式合法且短路。
class LogMessageVoidify {
 public:
  void operator&(std::ostream&) noexcept {}
};

}  // namespace qzrpc

// 通用日志宏。level 必须是 ::qzrpc::Logger::Level 枚举值。
//
// 关键点：`ShouldLog` 判定放在**构造 LogMessage 之前**。若写成
//   `LogMessage(level, ...).stream()`，即使该级别被关掉，`<<` 的参数拼接
//   （含临时的 std::string / ostringstream 构造）依然会全部执行，
//   级别旋钮只能省下最后那次 write，省不掉真正的开销。
#define QZ_LOG(level)                                                    \
  !::qzrpc::Logger::Instance().ShouldLog(level)                          \
      ? (void)0                                                          \
      : ::qzrpc::LogMessageVoidify() &                                   \
            ::qzrpc::LogMessage((level), __FILE__, __LINE__).stream()

#define QZ_LOG_TRACE QZ_LOG(::qzrpc::Logger::kTrace)
#define QZ_LOG_DEBUG QZ_LOG(::qzrpc::Logger::kDebug)
#define QZ_LOG_INFO QZ_LOG(::qzrpc::Logger::kInfo)
#define QZ_LOG_WARN QZ_LOG(::qzrpc::Logger::kWarn)
#define QZ_LOG_ERROR QZ_LOG(::qzrpc::Logger::kError)

// Fatal 日志：写出后进程 abort()。仅用于"继续跑下去毫无意义"的场景。
#define QZ_LOG_FATAL QZ_LOG(::qzrpc::Logger::kFatal)
