// 轻舟 RPC (Qingzhou RPC)
// src/common/status.cpp
//
// Status / StatusCode 的实现。
// 约定：StatusCode 的数值就是线协议帧头里的 16 位 status 字段，因此这里
//       **不做任何映射表**，只提供一个稳定的英文名（供日志与 metrics label 使用）。
//       用 switch 而不是数组：非法强制转换出来的值会落到 default，不会越界访问。

#include "common/status.h"

namespace qzrpc {

const char* StatusCodeName(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk:
      return "Ok";
    case StatusCode::kInvalidArgument:
      return "InvalidArgument";
    case StatusCode::kNotFound:
      return "NotFound";
    case StatusCode::kServiceUnavailable:
      return "ServiceUnavailable";
    case StatusCode::kTimeout:
      return "Timeout";
    case StatusCode::kOverload:
      return "Overload";
    case StatusCode::kInternal:
      return "Internal";
    case StatusCode::kSerializeError:
      return "SerializeError";
    case StatusCode::kCancelled:
      return "Cancelled";
  }
  return "Unknown";
}

std::string Status::ToString() const {
  const char* name = StatusCodeName(code_);
  if (message_.empty()) {
    return std::string(name);
  }
  std::string result;
  result.reserve(message_.size() + 16);
  result += name;
  result += ": ";
  result += message_;
  return result;
}

}  // namespace qzrpc
