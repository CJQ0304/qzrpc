// 轻舟 RPC (Qingzhou RPC)
// src/common/status.h
//
// 职责：全框架统一的调用结果类型。所有可能失败的 public API 一律返回 Status。
//
// 设计要点：
//   * 枚举值直接就是线协议头部的 16 位 status 字段取值（见 doc 03），不需要映射表。
//   * Status 是值类型，可拷贝、可移动、可比较；失败时携带一条人类可读 message。
//   * 不使用异常：C++ 异常在跨 DLL / 跨线程边界时行为不可控，框架全程用返回值。
//
// 线程安全：Status 是不可变值对象，跨线程传递安全。
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace qzrpc {

// 调用结果码。数值即线协议 status 字段取值，**不得随意改动已分配的值**。
enum class StatusCode : std::uint16_t {
  kOk = 0,                  // 成功
  kInvalidArgument = 1,     // 入参非法（序列化失败 / 缺必填 / 帧格式错误也归此类）
  kNotFound = 2,            // method_id 未注册 / 服务不存在
  kServiceUnavailable = 3,  // 无可用 endpoint、连接失败、服务端正在停机
  kTimeout = 4,             // 客户端等待超时，或服务端业务超时
  kOverload = 5,            // 服务端过载主动拒绝（队列满 / 自适应限流）
  kInternal = 6,            // 框架内部错误
  kSerializeError = 7,      // 编解码失败（与 kInvalidArgument 区分：这是 payload 层问题）
  kCancelled = 8,           // 调用被取消（连接失效 / 客户端停机）
};

// 返回状态码的稳定英文名（用于日志与 metrics label），永不返回 nullptr。
const char* StatusCodeName(StatusCode code) noexcept;

// 一次调用的结果。默认构造为 kOk / 空 message。
class Status {
 public:
  // 构造一个成功状态。
  Status() = default;

  // 用给定码与描述构造。code == kOk 时 message 会被忽略。
  // 参数：code - 结果码；message - 失败原因，仅用于日志与调试，不上线（线协议只传 16 位码）。
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  // 便捷构造：只用码，无描述。
  explicit Status(StatusCode code) : code_(code) {}

  // 工厂：成功。
  static Status Ok() { return Status(); }

  // 以下工厂等价于 Status(StatusCode::kXxx, msg)。
  static Status InvalidArgument(std::string msg) {
    return Status(StatusCode::kInvalidArgument, std::move(msg));
  }
  static Status NotFound(std::string msg) {
    return Status(StatusCode::kNotFound, std::move(msg));
  }
  static Status ServiceUnavailable(std::string msg) {
    return Status(StatusCode::kServiceUnavailable, std::move(msg));
  }
  static Status Timeout(std::string msg) {
    return Status(StatusCode::kTimeout, std::move(msg));
  }
  static Status Overload(std::string msg) {
    return Status(StatusCode::kOverload, std::move(msg));
  }
  static Status Internal(std::string msg) {
    return Status(StatusCode::kInternal, std::move(msg));
  }
  static Status SerializeError(std::string msg) {
    return Status(StatusCode::kSerializeError, std::move(msg));
  }
  static Status Cancelled(std::string msg) {
    return Status(StatusCode::kCancelled, std::move(msg));
  }

  // 是否成功。
  bool ok() const noexcept { return code_ == StatusCode::kOk; }

  // 结果码。
  StatusCode code() const noexcept { return code_; }

  // 失败描述，成功时为空串。
  const std::string& message() const noexcept { return message_; }

  // 修改描述（框架内部用于追加上下文，如 "timeout after 200ms, endpoint=1.2.3.4:80"）。
  void set_message(std::string msg) { message_ = std::move(msg); }

  // "NotFound: method 0x1a2b3c4d not registered" 形式，便于日志。
  std::string ToString() const;

  // 只比较码，不比较 message（message 是诊断信息，不参与语义）。
  bool operator==(StatusCode code) const noexcept { return code_ == code; }
  bool operator!=(StatusCode code) const noexcept { return code_ != code; }
  bool operator==(const Status& other) const noexcept { return code_ == other.code_; }
  bool operator!=(const Status& other) const noexcept { return code_ != other.code_; }

 private:
  StatusCode code_{StatusCode::kOk};
  std::string message_;
};

}  // namespace qzrpc
