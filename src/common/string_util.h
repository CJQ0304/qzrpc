// 轻舟 RPC (Qingzhou RPC)
// src/common/string_util.h
//
// 职责：字符串处理与哈希工具。其中 Fnv1a64 是 method_id 生成规则的**唯一实现**，
//       codegen（Python）必须产出与之一致的结果（见 doc 03 §7 与 doc 04）。
//
// 线程安全：全部为无状态静态函数，线程安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/noncopyable.h"

namespace qzrpc {

class StringUtil : private StaticOnly {
 public:
  // FNV-1a 64 位哈希。这是 method_id 算法的基础，**改动会导致所有已生成的代码失效**。
  // 参数：data/len - 字节序列，允许 len == 0（返回 offset basis）。
  // 返回：64 位哈希值。
  static std::uint64_t Fnv1a64(const void* data, std::size_t len) noexcept;

  // FNV-1a 64 位哈希（std::string 重载）。
  static std::uint64_t Fnv1a64(const std::string& s) noexcept;

  // FNV-1a 64 位哈希（C 字符串重载，nullptr 视为空串）。
  static std::uint64_t Fnv1a64(const char* s) noexcept;

  // 取 FNV-1a 64 的低 32 位，用于非加密场景（如一致性哈希的虚拟节点索引）。
  static std::uint32_t Fnv1a32(const std::string& s) noexcept;

  // 按单字符分隔切分，保留空字段（"a,,b" → {"a","","b"}）。
  static std::vector<std::string> Split(const std::string& s, char sep);

  // 按任意分隔符集合切分（例如 SplitAny(s, " \t\r\n") 用于按空白分词），丢弃空字段。
  static std::vector<std::string> SplitAny(const std::string& s, const char* delims);

  // 去除首尾在 trim_chars 中的字符。
  // 参数：trim_chars - 默认去空白（空格/制表/回车/换行）。
  static std::string Trim(const std::string& s, const char* trim_chars = " \t\r\n");

  // 全小写 / 全大写（只作用于 ASCII，非 ASCII 字节原样保留）。
  static std::string ToLower(const std::string& s);
  static std::string ToUpper(const std::string& s);

  // 把 camelCase / CamelCase 转成 snake_case（codegen 生成文件名时使用）。
  static std::string ToSnakeCase(const std::string& s);

  // 前缀/后缀判断（大小写敏感）。
  static bool StartsWith(const std::string& s, const std::string& prefix) noexcept;
  static bool EndsWith(const std::string& s, const std::string& suffix) noexcept;

  // 用 sep 连接。
  static std::string Join(const std::vector<std::string>& parts, const std::string& sep);

  // 全局替换（needle 为空时原样返回）。
  static std::string ReplaceAll(const std::string& s, const std::string& needle,
                                const std::string& replacement);

  // printf 风格的格式化，最多支持 4KB 输出（超长截断）。
  static std::string Format(const char* fmt, ...);

  // 十六进制编码（小写，无分隔符）。
  static std::string HexEncode(const void* data, std::size_t len);

  // 十六进制解码（忽略大小写，长度必须为偶数且字符合法）。
  // 返回：true 表示解码成功并写入 out；false 表示输入非法（out 不被修改）。
  static bool HexDecode(const std::string& hex, std::string* out);
};

}  // namespace qzrpc
