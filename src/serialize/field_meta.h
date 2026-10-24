// 轻舟 RPC (Qingzhou RPC)
// src/serialize/field_meta.h
//
// 职责：字段的静态元数据描述。由 codegen 生成为 `static const FieldMeta kFields[]`，
//       供运行时做"轻反射"：调试打印、字段合法性校验、schema 一致性检查、JSON 化。
//
// 刻意不做的事：
//   * 不做字段级的 get/set 反射（需要 offset 或 visitor，会让生成代码膨胀一倍）；
//     v1 只需要"字段清单 + 类型 + 编号"，用于工具与日志，不用于泛型赋值。
//   * 不支持 group（wire_type 3/4，protobuf 已废弃）。
//
// 线程安全：全部是编译期常量与无状态函数，天然线程安全。
#pragma once

#include <cstddef>
#include <cstdint>

namespace qzrpc {
namespace serde {

// protobuf 线类型。数值即 wire format 里的 3 位编码。
enum class WireType : std::uint8_t {
  kVarint = 0,
  kFixed64 = 1,
  kLengthDelimited = 2,
  kStartGroup = 3,  // 已废弃，本框架不支持
  kEndGroup = 4,    // 已废弃，本框架不支持
  kFixed32 = 5,
};

// IDL 里可以声明的字段类型。
enum class FieldType : std::uint8_t {
  kInt32 = 0,
  kInt64 = 1,
  kUInt32 = 2,
  kUInt64 = 3,
  kSInt32 = 4,
  kSInt64 = 5,
  kBool = 6,
  kFloat = 7,
  kDouble = 8,
  kString = 9,
  kBytes = 10,
  kEnum = 11,
  kMessage = 12,
};

// 单个字段的元数据。由 codegen 生成的常量表使用。
// 注意：这是 POD，允许静态初始化（constexpr 友好），不得增加需要动态初始化的成员。
struct FieldMeta {
  // 字段编号（tag），1..536870911（2^29-1，protobuf 上限）。
  std::uint32_t number;

  // 字段名（IDL 中的原样名字，如 "user_name"）。用于日志与 JSON 键。
  const char* name;

  // IDL 声明类型。
  FieldType type;

  // kMessage / kEnum 时指向类型全名（如 "demo.EchoBody"），其它类型为 nullptr。
  const char* type_name;

  // 是否 repeated。
  bool repeated;

  // 是否 optional（proto3 语义下等价于"有 has_xxx()"）。required 在本框架不存在。
  bool optional;

  // 是否显式指定了默认值（IDL 里写了 `= 默认值`）。
  bool has_default;

  // 默认值的文本形式（如 "0"、"false"、"\"hello\""）。has_default 为 false 时为 nullptr。
  const char* default_value;
};

// 字段类型的可读名（"int32"/"string"/...）。永不返回 nullptr。
const char* FieldTypeName(FieldType type) noexcept;

// 字段类型对应的线类型。
WireType WireTypeOf(FieldType type) noexcept;

// 该类型是否属于"可 packed"的数值类型（repeated 时默认 packed）。
bool IsPackable(FieldType type) noexcept;

// 该类型是否为数值（varint / fixed 族）。
bool IsNumeric(FieldType type) noexcept;

}  // namespace serde
}  // namespace qzrpc
