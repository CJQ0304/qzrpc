// 轻舟 RPC (Qingzhou RPC)
// src/serialize/reader.h
//
// 职责：protobuf wire format 兼容的**解码器**。零拷贝：只持有 (const char*, len) 视图，
//       绝不拷贝 payload；字符串字段由调用方决定是否 memcpy 出去。
//
// 错误处理：所有 Read* 返回 bool。**读失败后 Reader 进入 failed 状态**，
//           后续调用一律返回 false，最终 `!reader->failed()` 就是整体成败判据。
//           这是一个刻意的设计：generated code 里不需要每个 case 都写长串的错误传播。
//
// 安全性：所有长度都做上界检查（len 与剩余字节比较），不存在越界读；
//         恶意构造的超长 varint（>10 字节）与非法 wire_type 都会被拒绝。
//
// 线程安全：Reader 是**非线程安全**的纯数据对象，只能在单线程内使用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qzrpc {
namespace serde {

// 字节流解码器。
class Reader {
 public:
  // 构造。参数：data/len - 待解析的字节视图，**不拷贝**，调用方必须保证其生命周期覆盖 Reader。
  Reader(const void* data, std::size_t len);

  // ---- 基础读取 ----

  // 读 varint32。超过 5 字节或溢出时失败。
  bool ReadVarint32(std::uint32_t* out);

  // 读 varint64。超过 10 字节或溢出时失败。
  bool ReadVarint64(std::uint64_t* out);

  // 读一个 tag。
  // 参数：field_number - 输出字段号（>0）；wire_type - 输出线类型（0/1/2/5）。
  // 返回：true 成功；false 表示**已到末尾且没有更多完整字段**，或数据损坏。
  //       用 AtEnd() 区分"正常结束"与"损坏"。
  bool ReadTag(std::uint32_t* field_number, std::uint8_t* wire_type);

  // 读小端 fixed32 / fixed64。
  bool ReadFixed32(std::uint32_t* out);
  bool ReadFixed64(std::uint64_t* out);

  // 读 IEEE754 单/双精度。
  bool ReadFloat(float* out);
  bool ReadDouble(double* out);

  // 读 length-delimited 块的视图（零拷贝）。
  // 参数：data - 输出指向块首；len - 输出块长度。
  // 返回：true 成功。视图在 Reader 的底层缓冲生命周期内有效。
  bool ReadLengthDelimited(const char** data, std::size_t* len);

  // 读 string（拷贝）。
  bool ReadString(std::string* out);

  // 读 bytes（拷贝）。
  bool ReadBytes(std::string* out);

  // 读定长原始字节到 out。
  bool ReadRaw(void* out, std::size_t len);

  // ---- 无 tag 的基本类型读取（packed 元素与 frame 内部使用） ----

  // 读 int32（varint 结果截断到 32 位，兼容 protobuf 的 10 字节负数形式）。
  bool ReadInt32(std::int32_t* out);
  // 读 int64。
  bool ReadInt64(std::int64_t* out);
  // 读 uint32。
  bool ReadUInt32(std::uint32_t* out) { return ReadVarint32(out); }
  // 读 uint64。
  bool ReadUInt64(std::uint64_t* out) { return ReadVarint64(out); }
  // 读 zigzag int32。
  bool ReadSInt32(std::int32_t* out);
  // 读 zigzag int64。
  bool ReadSInt64(std::int64_t* out);
  // 读 bool（非 0 即真）。
  bool ReadBool(bool* out);

  // ---- 带 wire_type 校验的字段读取（generated code 直接使用，减少样板） ----

  // 要求 wire_type == 0，读 uint32。
  bool ReadVarint32Field(std::uint8_t wire_type, std::uint32_t* out);
  // 要求 wire_type == 0，读 uint64。
  bool ReadVarint64Field(std::uint8_t wire_type, std::uint64_t* out);
  // 要求 wire_type == 0，读 int32（varint 后截断到 32 位，兼容负数 10 字节形式）。
  bool ReadInt32Field(std::uint8_t wire_type, std::int32_t* out);
  // 要求 wire_type == 0，读 int64。
  bool ReadInt64Field(std::uint8_t wire_type, std::int64_t* out);
  // 要求 wire_type == 0，读 zigzag int32。
  bool ReadSInt32Field(std::uint8_t wire_type, std::int32_t* out);
  // 要求 wire_type == 0，读 zigzag int64。
  bool ReadSInt64Field(std::uint8_t wire_type, std::int64_t* out);
  // 要求 wire_type == 0，读 bool（非 0 即真）。
  bool ReadBoolField(std::uint8_t wire_type, bool* out);
  // 要求 wire_type == 5，读 float。
  bool ReadFloatField(std::uint8_t wire_type, float* out);
  // 要求 wire_type == 1，读 double。
  bool ReadDoubleField(std::uint8_t wire_type, double* out);
  // 要求 wire_type == 2，读 length-delimited 视图（零拷贝，用于 string/bytes/嵌套 message）。
  bool ReadLengthDelimitedField(std::uint8_t wire_type, const char** data, std::size_t* len);
  // 要求 wire_type == 2，读 string（拷贝）。
  bool ReadStringField(std::uint8_t wire_type, std::string* out);

  // ---- 未知字段处理 ----

  // 跳过当前字段（tag 已被 ReadTag 消费）。
  // 返回：false 表示数据损坏（非法 wire_type 或长度越界）。
  bool SkipField(std::uint8_t wire_type);

  // 跳过当前字段并保留其**原始编码字节**（不含 tag），用于放进 Message::unknown_fields，
  // 保证"读进来再写出去"不丢字段（protobuf 3 的 unknown field 语义）。
  bool SkipFieldCapture(std::uint8_t wire_type, std::string* raw);

  // ---- 状态 ----

  // 是否已到达末尾（没有剩余字节）。
  bool AtEnd() const noexcept { return pos_ >= len_; }

  // 剩余可读字节数。
  std::size_t bytes_left() const noexcept { return len_ - pos_; }

  // 当前读位置（相对起始）。
  std::size_t position() const noexcept { return pos_; }

  // 当前游标指针。
  const void* cursor() const noexcept;

  // 是否已经发生过读取失败。任何 Read* 失败都会置位，且此后所有 Read* 直接返回 false。
  bool failed() const noexcept { return failed_; }

  // 失败原因（供日志/测试断言）。永不返回 nullptr。
  const char* error() const noexcept { return error_; }

  // 跳过剩余全部字节（用于"忽略尾部填充"）。
  void SkipRemaining() noexcept { pos_ = len_; }

 private:
  // 记录失败并返回 false。
  bool Fail(const char* reason) noexcept;

  const char* begin_{nullptr};
  std::size_t len_{0};
  std::size_t pos_{0};
  bool failed_{false};
  const char* error_{""};
};

}  // namespace serde
}  // namespace qzrpc
