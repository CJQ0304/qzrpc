// 轻舟 RPC (Qingzhou RPC)
// src/serialize/writer.h
//
// 职责：protobuf wire format 兼容的**编码器**（只负责写出，不负责帧与协议）。
//
// 与 protobuf 的兼容性约定（这是能对外宣称"任意 protobuf 解析器可读"的依据）：
//   * tag 编码：tag = (field_number << 3) | wire_type，varint 输出；
//   * varint：每字节低 7 位有效，最高位为续位；小端序的 7 位分组（LSB first）；
//   * int32/int64/uint32/uint64/bool/enum 用 varint，**负数 int32 按 64 位符号扩展输出 10 字节**；
//   * sint32/sint64 用 zigzag 再 varint；
//   * fixed32/sfixed32/float 用 **小端** 4 字节（protobuf 规定 fixed 族是小端，与线协议头的大端不同！）；
//   * fixed64/sfixed64/double 用 **小端** 8 字节；
//   * string/bytes/嵌套 message 用 wire_type=2：varint 长度 + 原始字节；
//   * repeated 基本类型默认 packed（wire_type=2 的 length-delimited 块），与 proto3 默认一致。
//
// 线程安全：Writer 是**非线程安全**的纯数据对象，只能在单线程内使用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qzrpc {
namespace serde {

// 字节流编码器。默认累积到内部 std::string；Counting 模式只计数不存储（用于 ByteSize）。
class Writer {
 public:
  // 普通写入模式。
  Writer();

  // 构造一个"只计数"的 Writer：所有写入被丢弃，size() 反映真实编码长度。
  // 用途：Message::ByteSize() 与预分配。
  static Writer Counting();

  // 是否为计数模式。
  bool counting() const noexcept { return counting_; }

  // ---- 基础写入 ----

  // 写 varint32。v < 0x80 时只占 1 字节。
  void WriteVarint32(std::uint32_t v);

  // 写 varint64。
  void WriteVarint64(std::uint64_t v);

  // 写 tag：field_number 必须 >= 1，wire_type 取 0/1/2/5。
  void WriteTag(std::uint32_t field_number, std::uint8_t wire_type);

  // 写小端 4 字节（protobuf fixed32）。
  void WriteFixed32(std::uint32_t v);

  // 写小端 8 字节（protobuf fixed64）。
  void WriteFixed64(std::uint64_t v);

  // 写 IEEE754 单精度（小端）。用 memcpy 取位模式，保证 -0.0/NaN 位精确。
  void WriteFloat(float v);

  // 写 IEEE754 双精度（小端）。
  void WriteDouble(double v);

  // 写 int32（protobuf 语义：负数符号扩展到 64 位后 varint）。
  void WriteInt32(std::int32_t v);

  // 写 int64（varint）。
  void WriteInt64(std::int64_t v);

  // 写 int32 的合法负数优化版：负数时先写符号位扩展（与 WriteInt32 等价）。
  void WriteEnum(std::int32_t v);

  // 写 sint32（zigzag 编码）。
  void WriteSInt32(std::int32_t v);

  // 写 sint64（zigzag 编码）。
  void WriteSInt64(std::int64_t v);

  // 写 bool（0 或 1 的单字节 varint）。
  void WriteBool(bool v);

  // 写 length-delimited 块：先 varint 长度，再原始字节。data 可为 nullptr（len 必须为 0）。
  void WriteLengthDelimited(const void* data, std::size_t len);

  // 写 string（length-delimited）。
  void WriteString(const std::string& s);

  // 写 bytes（length-delimited）。
  void WriteBytes(const void* data, std::size_t len);

  // 写原始字节，**不带长度前缀**（用于包 packed 块内部元素、或帧头拼接）。
  void WriteRaw(const void* data, std::size_t len);

  // ---- 结果读取 ----

  // 已写入（或已计数）的字节数。
  std::size_t size() const noexcept { return counting_ ? counted_ : buffer_.size(); }

  // 当前缓冲内容。计数模式下返回空串。
  const std::string& data() const noexcept { return buffer_; }

  // 移出缓冲内容并清空（避免一次大拷贝）。
  std::string ReleaseData();

  // 清空缓冲与计数，回到初始状态（Counting 标记保留）。
  void Reset();

 private:
  // 真正落字节的唯一入口，统一处理计数模式。
  void Put(const void* data, std::size_t len);

  std::string buffer_;
  std::size_t counted_{0};
  bool counting_{false};
};

}  // namespace serde
}  // namespace qzrpc
