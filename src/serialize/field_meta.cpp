// 轻舟 RPC (Qingzhou RPC)
// src/serialize/field_meta.cpp
//
// 职责：字段元数据的三个纯函数。全部是编译期可算的 switch，不用静态数组
//       （避免"枚举加了新值但数组没更新"导致的越界读）。

#include "serialize/field_meta.h"

namespace qzrpc {
namespace serde {

const char* FieldTypeName(FieldType type) noexcept {
  switch (type) {
    case FieldType::kInt32:
      return "int32";
    case FieldType::kInt64:
      return "int64";
    case FieldType::kUInt32:
      return "uint32";
    case FieldType::kUInt64:
      return "uint64";
    case FieldType::kSInt32:
      return "sint32";
    case FieldType::kSInt64:
      return "sint64";
    case FieldType::kBool:
      return "bool";
    case FieldType::kFloat:
      return "float";
    case FieldType::kDouble:
      return "double";
    case FieldType::kString:
      return "string";
    case FieldType::kBytes:
      return "bytes";
    case FieldType::kEnum:
      return "enum";
    case FieldType::kMessage:
      return "message";
  }
  // 走到这里说明调用方把非法值塞进了 FieldType（例如从文件里读到了脏数据）。
  return "unknown";
}

WireType WireTypeOf(FieldType type) noexcept {
  switch (type) {
    // fixed 族：wire_type 1 / 5，编码为**小端**（protobuf 规定，与帧头大端无关）。
    case FieldType::kFloat:
      return WireType::kFixed32;
    case FieldType::kDouble:
      return WireType::kFixed64;
    // 变长块：wire_type 2。
    case FieldType::kString:
    case FieldType::kBytes:
    case FieldType::kMessage:
      return WireType::kLengthDelimited;
    // 其余全部是 varint：wire_type 0（int32/int64/uint32/uint64/sint32/sint64/bool/enum）。
    case FieldType::kInt32:
    case FieldType::kInt64:
    case FieldType::kUInt32:
    case FieldType::kUInt64:
    case FieldType::kSInt32:
    case FieldType::kSInt64:
    case FieldType::kBool:
    case FieldType::kEnum:
      return WireType::kVarint;
  }
  return WireType::kVarint;
}

bool IsPackable(FieldType type) noexcept {
  // protobuf 的 packed 只适用于"定长或 varint 的标量"，不适用于 string/bytes/message。
  switch (type) {
    case FieldType::kInt32:
    case FieldType::kInt64:
    case FieldType::kUInt32:
    case FieldType::kUInt64:
    case FieldType::kSInt32:
    case FieldType::kSInt64:
    case FieldType::kBool:
    case FieldType::kEnum:
    case FieldType::kFloat:
    case FieldType::kDouble:
      return true;
    case FieldType::kString:
    case FieldType::kBytes:
    case FieldType::kMessage:
      return false;
  }
  return false;
}

bool IsNumeric(FieldType type) noexcept {
  // 数值 = varint 族 + fixed 族（都不含长度前缀）。
  switch (type) {
    case FieldType::kInt32:
    case FieldType::kInt64:
    case FieldType::kUInt32:
    case FieldType::kUInt64:
    case FieldType::kSInt32:
    case FieldType::kSInt64:
    case FieldType::kFloat:
    case FieldType::kDouble:
    case FieldType::kEnum:
      return true;
    case FieldType::kBool:
    case FieldType::kString:
    case FieldType::kBytes:
    case FieldType::kMessage:
      return false;
  }
  return false;
}

}  // namespace serde
}  // namespace qzrpc
