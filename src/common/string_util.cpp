// 轻舟 RPC (Qingzhou RPC)
// src/common/string_util.cpp
//
// 字符串工具集合。其中 Fnv1a64 是 method_id 的**唯一实现**（见 docs/03-wire-protocol.md §7.2），
// codegen（Python 侧 methodid.py）必须与这里逐字等价，改动会导致所有已生成的代码失效。
//
// 注意：Format 用 vsnprintf 两段式（先探长度再分配），并硬性把输出截断在 4KB，
//       避免一条畸形日志把内存打爆。

#include "common/string_util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace qzrpc {
namespace {

// FNV-1a 64 参数（docs/03-wire-protocol.md §7.1，不得改动）。
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

// Format 的输出上限：超长直接截断，不报错（日志路径不容许失败）。
constexpr std::size_t kMaxFormatLength = 4096;

bool ContainsChar(const char* set, char c) noexcept {
  if (set == nullptr) {
    return false;
  }
  for (const char* p = set; *p != '\0'; ++p) {
    if (*p == c) {
      return true;
    }
  }
  return false;
}

bool IsAsciiDigit(char c) noexcept { return c >= '0' && c <= '9'; }

bool IsAsciiLower(char c) noexcept { return c >= 'a' && c <= 'z'; }

bool IsAsciiUpper(char c) noexcept { return c >= 'A' && c <= 'Z'; }

// 十六进制字符 -> 数值，非法返回 -1。
int HexValue(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

std::uint64_t StringUtil::Fnv1a64(const void* data, std::size_t len) noexcept {
  std::uint64_t hash = kFnvOffsetBasis;
  if (data == nullptr || len == 0) {
    return hash;  // 空输入返回 offset basis（与 Python 侧一致）
  }
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < len; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint64_t StringUtil::Fnv1a64(const std::string& s) noexcept {
  return Fnv1a64(s.data(), s.size());
}

std::uint64_t StringUtil::Fnv1a64(const char* s) noexcept {
  if (s == nullptr) {
    return kFnvOffsetBasis;
  }
  return Fnv1a64(s, std::strlen(s));
}

std::uint32_t StringUtil::Fnv1a32(const std::string& s) noexcept {
  return static_cast<std::uint32_t>(Fnv1a64(s) & 0xFFFFFFFFULL);
}

std::vector<std::string> StringUtil::Split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (true) {
    const std::size_t pos = s.find(sep, begin);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(begin));
      break;
    }
    parts.push_back(s.substr(begin, pos - begin));
    begin = pos + 1;
  }
  return parts;
}

std::vector<std::string> StringUtil::SplitAny(const std::string& s, const char* delims) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin < s.size()) {
    // 跳过前导分隔符
    while (begin < s.size() && ContainsChar(delims, s[begin])) {
      ++begin;
    }
    if (begin >= s.size()) {
      break;
    }
    std::size_t end = begin;
    while (end < s.size() && !ContainsChar(delims, s[end])) {
      ++end;
    }
    parts.push_back(s.substr(begin, end - begin));
    begin = end;
  }
  return parts;
}

std::string StringUtil::Trim(const std::string& s, const char* trim_chars) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  if (trim_chars == nullptr || *trim_chars == '\0') {
    return s;
  }
  while (begin < end && ContainsChar(trim_chars, s[begin])) {
    ++begin;
  }
  while (end > begin && ContainsChar(trim_chars, s[end - 1])) {
    --end;
  }
  return s.substr(begin, end - begin);
}

std::string StringUtil::ToLower(const std::string& s) {
  std::string result = s;
  for (char& c : result) {
    if (IsAsciiUpper(c)) {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return result;
}

std::string StringUtil::ToUpper(const std::string& s) {
  std::string result = s;
  for (char& c : result) {
    if (IsAsciiLower(c)) {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return result;
}

std::string StringUtil::ToSnakeCase(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  const std::size_t n = s.size();
  for (std::size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (IsAsciiUpper(c)) {
      const bool prev_is_lower_or_digit =
          i > 0 && (IsAsciiLower(s[i - 1]) || IsAsciiDigit(s[i - 1]));
      // "HTTPServer" 这种连续大写后跟小写的情况，在下一个小写之前断词 -> "http_server"
      const bool acronym_boundary =
          i > 0 && i + 1 < n && IsAsciiUpper(s[i - 1]) && IsAsciiLower(s[i + 1]);
      if (prev_is_lower_or_digit || acronym_boundary) {
        out.push_back('_');
      }
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c == '-' || c == ' ') {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool StringUtil::StartsWith(const std::string& s, const std::string& prefix) noexcept {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool StringUtil::EndsWith(const std::string& s, const std::string& suffix) noexcept {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string StringUtil::Join(const std::vector<std::string>& parts, const std::string& sep) {
  std::string out;
  std::size_t total = 0;
  for (const std::string& part : parts) {
    total += part.size() + sep.size();
  }
  out.reserve(total);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out += sep;
    }
    out += parts[i];
  }
  return out;
}

std::string StringUtil::ReplaceAll(const std::string& s, const std::string& needle,
                                   const std::string& replacement) {
  if (needle.empty()) {
    return s;
  }
  std::string out;
  out.reserve(s.size());
  std::size_t begin = 0;
  while (true) {
    const std::size_t pos = s.find(needle, begin);
    if (pos == std::string::npos) {
      out.append(s, begin, std::string::npos);
      break;
    }
    out.append(s, begin, pos - begin);
    out += replacement;
    begin = pos + needle.size();
  }
  return out;
}

std::string StringUtil::Format(const char* fmt, ...) {
  if (fmt == nullptr) {
    return std::string();
  }
  va_list args;
  va_start(args, fmt);
  va_list args_copy;
  va_copy(args_copy, args);

  // 第一段：用栈上的小缓冲探长度（绝大多数日志行在这里就结束了，零堆分配）。
  char stack_buf[256];
  const int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
  va_end(args);

  std::string result;
  if (needed < 0) {
    va_end(args_copy);
    return result;  // 格式化失败（编码错误等），返回空串而不是抛异常
  }
  const std::size_t length =
      std::min(static_cast<std::size_t>(needed), kMaxFormatLength);
  if (length < sizeof(stack_buf)) {
    result.assign(stack_buf, length);
  } else {
    // 第二段：按需分配，最多 4KB。
    std::vector<char> heap(length + 1, '\0');
    std::vsnprintf(heap.data(), heap.size(), fmt, args_copy);
    result.assign(heap.data(), length);
  }
  va_end(args_copy);
  return result;
}

std::string StringUtil::HexEncode(const void* data, std::size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  if (data == nullptr || len == 0) {
    return std::string();
  }
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  std::string out;
  out.resize(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out[i * 2] = kDigits[bytes[i] >> 4];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

bool StringUtil::HexDecode(const std::string& hex, std::string* out) {
  if (out == nullptr) {
    return false;
  }
  if (hex.size() % 2 != 0) {
    return false;  // 长度必须为偶数
  }
  std::string decoded;
  decoded.resize(hex.size() / 2);
  for (std::size_t i = 0; i < decoded.size(); ++i) {
    const int high = HexValue(hex[i * 2]);
    const int low = HexValue(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;  // 非法字符：out 不被修改
    }
    decoded[i] = static_cast<char>((high << 4) | low);
  }
  *out = std::move(decoded);
  return true;
}

}  // namespace qzrpc
