// 轻舟 RPC (Qingzhou RPC)
// src/serialize/writer.cpp
//
// 职责：protobuf wire format 兼容的编码器。
//
// 四个兼容性坑（doc 00 §4.1），本文件的每条注释都对应一处：
//   1) fixed32/fixed64 一律**小端**（protobuf 规定），与帧头的大端完全无关；
//   2) 负 int32 必须先符号扩展到 64 位再 varint —— 否则输出 5 字节，protobuf 解出来是正数；
//   3) packed 块的 tag wire_type 是 2（由调用方写，本类只提供 WriteRaw）；
//   4) string 长度上限 INT32_MAX，超长时**不写该字段**（见下方说明）。

#include "serialize/writer.h"

#include <cstring>
#include <limits>
#include <utility>

namespace qzrpc {
namespace serde {

Writer::Writer() = default;

Writer Writer::Counting() {
  Writer writer;
  writer.counting_ = true;
  return writer;
}

void Writer::Put(const void* data, std::size_t len) {
  if (data == nullptr || len == 0) return;
  if (counting_) {
    counted_ += len;
    return;
  }
  buffer_.append(static_cast<const char*>(data), len);
}

void Writer::WriteVarint64(std::uint64_t v) {
  // 最多 10 字节：7 bit × 9 + 最后 1 bit。
  unsigned char buf[10];
  std::size_t n = 0;
  while (v >= 0x80U) {
    // 低 7 位 + 续位；显式窄化避免 /W4 的 C4244。
    buf[n++] = static_cast<unsigned char>((v & 0x7FU) | 0x80U);
    v >>= 7;
  }
  buf[n++] = static_cast<unsigned char>(v);
  Put(buf, n);
}

void Writer::WriteVarint32(std::uint32_t v) { WriteVarint64(v); }

void Writer::WriteTag(std::uint32_t field_number, std::uint8_t wire_type) {
  // field_number 必须 >= 1；0 是非法字段号（ReadTag 会拒绝），这里直接不写，
  // 避免生成代码的 bug 变成线上可解析但语义错误的帧。
  if (field_number == 0) return;
  const std::uint32_t tag = (field_number << 3) | (static_cast<std::uint32_t>(wire_type) & 0x7U);
  WriteVarint32(tag);
}

void Writer::WriteFixed32(std::uint32_t v) {
  // 小端：低字节在前。位运算而非 memcpy，保证任何主机字节序下行为一致。
  unsigned char buf[4];
  buf[0] = static_cast<unsigned char>(v & 0xFFU);
  buf[1] = static_cast<unsigned char>((v >> 8) & 0xFFU);
  buf[2] = static_cast<unsigned char>((v >> 16) & 0xFFU);
  buf[3] = static_cast<unsigned char>((v >> 24) & 0xFFU);
  Put(buf, sizeof(buf));
}

void Writer::WriteFixed64(std::uint64_t v) {
  unsigned char buf[8];
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFU);
  }
  Put(buf, sizeof(buf));
}

void Writer::WriteFloat(float v) {
  // 用 memcpy 取位模式：保证 -0.0 / NaN 的符号与尾数位精确保留。
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v), "float must be 32-bit");
  std::memcpy(&bits, &v, sizeof(bits));
  WriteFixed32(bits);
}

void Writer::WriteDouble(double v) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit");
  std::memcpy(&bits, &v, sizeof(bits));
  WriteFixed64(bits);
}

void Writer::WriteInt32(std::int32_t v) {
  // 坑 2：负数先符号扩展到 64 位，输出 10 字节 varint。
  // 直接写 static_cast<std::uint32_t>(v) 会得到 5 字节，protobuf 读成 4294967295 这类正数。
  WriteVarint64(static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
}

void Writer::WriteInt64(std::int64_t v) {
  WriteVarint64(static_cast<std::uint64_t>(v));  // 负数是 10 字节，符合 protobuf
}

void Writer::WriteEnum(std::int32_t v) { WriteInt32(v); }

void Writer::WriteSInt32(std::int32_t v) {
  // zigzag: (n << 1) ^ (n >> 31)。n >> 31 对负数得到 -1（全 1），实现算术右移。
  const std::uint32_t zz =
      (static_cast<std::uint32_t>(v) << 1) ^ static_cast<std::uint32_t>(v >> 31);
  WriteVarint32(zz);
}

void Writer::WriteSInt64(std::int64_t v) {
  const std::uint64_t zz =
      (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
  WriteVarint64(zz);
}

void Writer::WriteBool(bool v) {
  const unsigned char byte = static_cast<unsigned char>(v ? 1 : 0);
  Put(&byte, 1);
}

void Writer::WriteLengthDelimited(const void* data, std::size_t len) {
  if (len > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return;
  WriteVarint32(static_cast<std::uint32_t>(len));
  Put(data, len);
}

void Writer::WriteString(const std::string& s) {
  // 坑 4：protobuf 的长度前缀是 int32，超过 INT32_MAX 的字符串无法表达。
  // Writer 没有错误状态（头文件已冻结），因此这里选择"整字段不写"：
  // 生成代码的 Encode 无法感知失败，但 2GB 级的字段在本框架里根本发不出去
  // （帧上限 16MB），静默丢弃一个必然非法的字段比写出一个越界长度更安全。
  // 该缺口已列入交付报告的「契约缺口」。
  if (s.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) return;
  WriteLengthDelimited(s.data(), s.size());
}

void Writer::WriteBytes(const void* data, std::size_t len) {
  if (len > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) return;
  WriteLengthDelimited(data, len);
}

void Writer::WriteRaw(const void* data, std::size_t len) { Put(data, len); }

std::string Writer::ReleaseData() {
  std::string out = std::move(buffer_);
  buffer_.clear();  // move 之后 buffer_ 处于合法但未定义内容的状态，显式清空
  counted_ = 0;
  return out;
}

void Writer::Reset() {
  buffer_.clear();
  counted_ = 0;
}

}  // namespace serde
}  // namespace qzrpc
