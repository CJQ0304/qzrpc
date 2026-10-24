// 轻舟 RPC (Qingzhou RPC)
// src/serialize/reader.cpp
//
// 职责：protobuf wire format 兼容的解码器。零拷贝视图，只记录 pos_。
//
// ★ 失败会"传染"：任何 Read* 失败都调用 Fail()，failed_ 置位后所有后续 Read*
//   立刻返回 false。这样生成代码里的 Decode 不需要每个 case 都写错误传播，
//   最后判一次 reader->failed() / AtEnd() 即可。
//
// 安全边界（全部为硬检查，不存在越界读）：
//   * varint64 最多 10 字节，第 10 字节只能是 0x00 或 0x01；
//   * varint32 最多 5 字节，第 5 字节的高 4 位必须为 0；
//   * 任何长度前缀都先与 bytes_left() 比较；
//   * 非法 wire_type（3/4/6/7）一律拒绝（group 已废弃，本框架不支持）。

#include "serialize/reader.h"

#include <cstring>

namespace qzrpc {
namespace serde {

namespace {

constexpr std::uint8_t kWireVarint = 0;
constexpr std::uint8_t kWireFixed64 = 1;
constexpr std::uint8_t kWireLengthDelimited = 2;
constexpr std::uint8_t kWireFixed32 = 5;

}  // namespace

Reader::Reader(const void* data, std::size_t len)
    : begin_(static_cast<const char*>(data)), len_(data == nullptr ? 0 : len) {}

bool Reader::Fail(const char* reason) noexcept {
  if (!failed_) {
    failed_ = true;
    error_ = reason != nullptr ? reason : "unknown error";
  }
  return false;
}

bool Reader::ReadVarint64(std::uint64_t* out) {
  if (out == nullptr) return Fail("null output for varint64");
  if (failed_) return false;
  std::uint64_t value = 0;
  for (int i = 0; i < 10; ++i) {
    if (pos_ >= len_) return Fail("truncated varint64");
    const std::uint8_t byte = static_cast<std::uint8_t>(begin_[pos_++]);
    if (i == 9) {
      // 第 10 字节只剩 1 位有效（64 - 9*7 = 1），只能是 0 或 1。
      if (byte > 0x01U) return Fail("varint64 overflow");
      value |= static_cast<std::uint64_t>(byte) << 63;
      *out = value;
      return true;
    }
    value |= static_cast<std::uint64_t>(byte & 0x7FU) << (7 * i);
    if ((byte & 0x80U) == 0) {
      *out = value;
      return true;
    }
  }
  return Fail("varint64 too long");
}

bool Reader::ReadVarint32(std::uint32_t* out) {
  if (out == nullptr) return Fail("null output for varint32");
  if (failed_) return false;
  std::uint32_t value = 0;
  for (int i = 0; i < 5; ++i) {
    if (pos_ >= len_) return Fail("truncated varint32");
    const std::uint8_t byte = static_cast<std::uint8_t>(begin_[pos_++]);
    if (i == 4) {
      // 第 5 字节只剩 4 位有效（32 - 4*7 = 4）。
      if (byte > 0x0FU) return Fail("varint32 overflow");
      value |= static_cast<std::uint32_t>(byte) << 28;
      *out = value;
      return true;
    }
    value |= static_cast<std::uint32_t>(byte & 0x7FU) << (7 * i);
    if ((byte & 0x80U) == 0) {
      *out = value;
      return true;
    }
  }
  return Fail("varint32 too long");
}

bool Reader::ReadTag(std::uint32_t* field_number, std::uint8_t* wire_type) {
  if (field_number == nullptr || wire_type == nullptr) return Fail("null output for tag");
  if (failed_) return false;
  if (AtEnd()) return false;  // 正常结束：不置 failed_，调用方用 AtEnd() 区分
  std::uint32_t tag = 0;
  if (!ReadVarint32(&tag)) return false;
  const std::uint32_t number = tag >> 3;
  const std::uint8_t type = static_cast<std::uint8_t>(tag & 0x7U);
  if (number == 0) return Fail("field number 0 is illegal");
  if (type != kWireVarint && type != kWireFixed64 && type != kWireLengthDelimited &&
      type != kWireFixed32) {
    return Fail("unsupported wire type (group is not supported)");
  }
  *field_number = number;
  *wire_type = type;
  return true;
}

bool Reader::ReadFixed32(std::uint32_t* out) {
  if (out == nullptr) return Fail("null output for fixed32");
  if (failed_) return false;
  if (len_ - pos_ < 4) return Fail("truncated fixed32");
  const unsigned char* p = reinterpret_cast<const unsigned char*>(begin_ + pos_);
  // 小端：低字节在前（protobuf 规定，与帧头大端无关）。
  *out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
  pos_ += 4;
  return true;
}

bool Reader::ReadFixed64(std::uint64_t* out) {
  if (out == nullptr) return Fail("null output for fixed64");
  if (failed_) return false;
  if (len_ - pos_ < 8) return Fail("truncated fixed64");
  const unsigned char* p = reinterpret_cast<const unsigned char*>(begin_ + pos_);
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  }
  *out = value;
  pos_ += 8;
  return true;
}

bool Reader::ReadFloat(float* out) {
  if (out == nullptr) return Fail("null output for float");
  std::uint32_t bits = 0;
  if (!ReadFixed32(&bits)) return false;
  std::memcpy(out, &bits, sizeof(*out));
  return true;
}

bool Reader::ReadDouble(double* out) {
  if (out == nullptr) return Fail("null output for double");
  std::uint64_t bits = 0;
  if (!ReadFixed64(&bits)) return false;
  std::memcpy(out, &bits, sizeof(*out));
  return true;
}

bool Reader::ReadLengthDelimited(const char** data, std::size_t* len) {
  if (data == nullptr || len == nullptr) return Fail("null output for length-delimited");
  if (failed_) return false;
  std::uint32_t size = 0;
  if (!ReadVarint32(&size)) return false;
  if (static_cast<std::size_t>(size) > bytes_left()) return Fail("length exceeds remaining bytes");
  *data = begin_ + pos_;
  *len = static_cast<std::size_t>(size);
  pos_ += size;
  return true;
}

bool Reader::ReadString(std::string* out) {
  if (out == nullptr) return Fail("null output for string");
  const char* data = nullptr;
  std::size_t len = 0;
  if (!ReadLengthDelimited(&data, &len)) return false;
  out->assign(data, len);
  return true;
}

bool Reader::ReadBytes(std::string* out) { return ReadString(out); }

bool Reader::ReadRaw(void* out, std::size_t len) {
  if (failed_) return false;
  if (len == 0) return true;
  if (out == nullptr) return Fail("null output for raw bytes");
  if (len > bytes_left()) return Fail("truncated raw bytes");
  std::memcpy(out, begin_ + pos_, len);
  pos_ += len;
  return true;
}

bool Reader::ReadInt32(std::int32_t* out) {
  if (out == nullptr) return Fail("null output for int32");
  if (failed_) return false;
  // protobuf 的 int32 负数在线上是 10 字节 varint，这里按 64 位读再截断。
  std::uint64_t wide = 0;
  if (!ReadVarint64(&wide)) return false;
  *out = static_cast<std::int32_t>(static_cast<std::uint32_t>(wide));
  return true;
}

bool Reader::ReadInt64(std::int64_t* out) {
  if (out == nullptr) return Fail("null output for int64");
  if (failed_) return false;
  std::uint64_t wide = 0;
  if (!ReadVarint64(&wide)) return false;
  *out = static_cast<std::int64_t>(wide);
  return true;
}

bool Reader::ReadSInt32(std::int32_t* out) {
  if (out == nullptr) return Fail("null output for sint32");
  if (failed_) return false;
  std::uint32_t zz = 0;
  if (!ReadVarint32(&zz)) return false;
  // zigzag 解码：(zz >> 1) ^ -(zz & 1)
  *out = static_cast<std::int32_t>((zz >> 1) ^ (0U - (zz & 1U)));
  return true;
}

bool Reader::ReadSInt64(std::int64_t* out) {
  if (out == nullptr) return Fail("null output for sint64");
  if (failed_) return false;
  std::uint64_t zz = 0;
  if (!ReadVarint64(&zz)) return false;
  *out = static_cast<std::int64_t>((zz >> 1) ^ (0ULL - (zz & 1ULL)));
  return true;
}

bool Reader::ReadBool(bool* out) {
  if (out == nullptr) return Fail("null output for bool");
  if (failed_) return false;
  std::uint32_t raw = 0;
  if (!ReadVarint32(&raw)) return false;
  *out = raw != 0;
  return true;
}

// ---------------------------------------------------------------------------
// 带 wire_type 校验的字段读取
// ---------------------------------------------------------------------------

bool Reader::ReadVarint32Field(std::uint8_t wire_type, std::uint32_t* out) {
  if (failed_) return false;
  if (wire_type != kWireVarint) return Fail("wire type mismatch: expected varint32");
  return ReadVarint32(out);
}

bool Reader::ReadVarint64Field(std::uint8_t wire_type, std::uint64_t* out) {
  if (failed_) return false;
  if (wire_type != kWireVarint) return Fail("wire type mismatch: expected varint64");
  return ReadVarint64(out);
}

bool Reader::ReadInt32Field(std::uint8_t wire_type, std::int32_t* out) {
  if (failed_) return false;
  if (wire_type != kWireVarint) return Fail("wire type mismatch: expected int32");
  return ReadInt32(out);
}

bool Reader::ReadInt64Field(std::uint8_t wire_type, std::int64_t* out) {
  if (failed_) return false;
  if (wire_type != kWireVarint) return Fail("wire type mismatch: expected int64");
  return ReadInt64(out);
}

bool Reader::ReadSInt32Field(std::uint8_t wire_type, std::int32_t* out) {
  if (failed_) return false;
  if (wire_type != kWireVarint) return Fail("wire type mismatch: expected sint32");
  return ReadSInt32(out);
}

bool Reader::ReadSInt64Field(std::uint8_t wire_type, std::int64_t* out) {
  if (failed_) return false;
  if (wire_type != kWireVarint) return Fail("wire type mismatch: expected sint64");
  return ReadSInt64(out);
}

bool Reader::ReadBoolField(std::uint8_t wire_type, bool* out) {
  if (failed_) return false;
  if (wire_type != kWireVarint) return Fail("wire type mismatch: expected bool");
  return ReadBool(out);
}

bool Reader::ReadFloatField(std::uint8_t wire_type, float* out) {
  if (failed_) return false;
  if (wire_type != kWireFixed32) return Fail("wire type mismatch: expected float");
  return ReadFloat(out);
}

bool Reader::ReadDoubleField(std::uint8_t wire_type, double* out) {
  if (failed_) return false;
  if (wire_type != kWireFixed64) return Fail("wire type mismatch: expected double");
  return ReadDouble(out);
}

bool Reader::ReadLengthDelimitedField(std::uint8_t wire_type, const char** data,
                                      std::size_t* len) {
  if (failed_) return false;
  if (wire_type != kWireLengthDelimited) {
    return Fail("wire type mismatch: expected length-delimited");
  }
  return ReadLengthDelimited(data, len);
}

bool Reader::ReadStringField(std::uint8_t wire_type, std::string* out) {
  if (failed_) return false;
  if (wire_type != kWireLengthDelimited) return Fail("wire type mismatch: expected string");
  return ReadString(out);
}

// ---------------------------------------------------------------------------
// 未知字段
// ---------------------------------------------------------------------------

bool Reader::SkipField(std::uint8_t wire_type) {
  if (failed_) return false;
  switch (wire_type) {
    case kWireVarint: {
      std::uint64_t discarded = 0;
      return ReadVarint64(&discarded);
    }
    case kWireFixed64:
      if (len_ - pos_ < 8) return Fail("truncated fixed64 while skipping");
      pos_ += 8;
      return true;
    case kWireLengthDelimited: {
      const char* data = nullptr;
      std::size_t len = 0;
      return ReadLengthDelimited(&data, &len);
    }
    case kWireFixed32:
      if (len_ - pos_ < 4) return Fail("truncated fixed32 while skipping");
      pos_ += 4;
      return true;
    default:
      return Fail("illegal wire type while skipping");
  }
}

bool Reader::SkipFieldCapture(std::uint8_t wire_type, std::string* raw) {
  if (failed_) return false;
  if (raw == nullptr) return Fail("null output for captured raw field");
  const std::size_t start = pos_;
  if (!SkipField(wire_type)) return false;
  raw->assign(begin_ + start, pos_ - start);
  return true;
}

const void* Reader::cursor() const noexcept {
  if (begin_ == nullptr) return nullptr;
  return begin_ + pos_;
}

}  // namespace serde
}  // namespace qzrpc
