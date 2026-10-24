// 轻舟 RPC (Qingzhou RPC)
// src/protocol/frame.cpp
//
// 职责：帧的内存表示与其编解码（不含粘包逻辑，那在 codec.cpp）。
//
// 大端约定：帧头全部定长字段一律大端，统一走 common/endian.h 的 StoreBig32/LoadBig32。
// 本文件**不允许**出现手写的 <<24 / >>8 之类字节序代码（除了 metadata 的 TLV 走
// serde::Writer/Reader，那本来就是 protobuf 的小端/varint 域）。
//
// 校验策略（DecodeFrom）：
//   * magic / version / flags / body_len 结构性错误 → 返回 false（调用方必须断开）；
//   * 请求帧 status != 0 **不在这一层拒绝**：doc 03 §14 第 6 项要求"服务端按
//     INVALID_ARGUMENT 处理"，也就是说要能读出这一帧并回一个错误响应，
//     而不是直接断连。该判定属于服务端编排层（下一波 server/ 实现）。

#include "protocol/frame.h"

#include <string>
#include <utility>

#include "common/endian.h"

namespace qzrpc {
namespace protocol {

namespace {

void SetError(std::string* err, const char* text) {
  if (err != nullptr) *err = text;
}

void SetError(std::string* err, const std::string& text) {
  if (err != nullptr) *err = text;
}

// 用户扩展键的接收侧上限（防滥用）。
//
// 为什么需要上限：extras_ 是 std::map<uint32_t, string>，如果解码时不加约束，
// 一个构造出来的恶意帧可以塞进无上限的键值对，把"解码一个帧"变成"分配任意内存"。
// 这是纯接收侧策略，不属于线协议，所以刻意不放进 protocol_const.h。
//
// 超限时的处理是**静默丢弃**而不是返回 false：
// 扩展键是可选展示字段，丢弃的代价远小于"因为一个可选字段让整条连接断掉"；
// 而如果返回 false，攻击者反而能用最少的字节让我们拒绝合法连接。
constexpr std::size_t kMaxUserExtras = 32;
constexpr std::size_t kMaxUserExtValueLen = 1024;

}  // namespace

// ---------------------------------------------------------------------------
// FrameHeader
// ---------------------------------------------------------------------------

void FrameHeader::EncodeTo(std::uint8_t* out) const {
  if (out == nullptr) return;
  StoreBig32(out + 0, magic);
  out[4] = version;
  out[5] = flags;
  StoreBig16(out + 6, status);
  StoreBig32(out + 8, seq);
  StoreBig32(out + 12, method_id);
  StoreBig32(out + 16, body_len);
}

bool FrameHeader::DecodeFrom(const std::uint8_t* in, FrameHeader* out, std::string* err) {
  if (in == nullptr || out == nullptr) {
    SetError(err, "frame header: null buffer");
    return false;
  }
  FrameHeader header;
  header.magic = LoadBig32(in + 0);
  header.version = in[4];
  header.flags = in[5];
  header.status = LoadBig16(in + 6);
  header.seq = LoadBig32(in + 8);
  header.method_id = LoadBig32(in + 12);
  header.body_len = LoadBig32(in + 16);

  if (header.magic != kMagic) {
    SetError(err, "frame header: bad magic");
    return false;
  }
  if (header.version != kVersion) {
    SetError(err, "frame header: unsupported version " + std::to_string(header.version));
    return false;
  }
  if (!IsValidFlags(header.flags)) {
    SetError(err, "frame header: invalid flags");
    return false;
  }
  if (header.body_len < kMetadataLengthSize) {
    SetError(err, "frame header: body_len < 4");
    return false;
  }
  if (header.body_len > kAbsoluteMaxFrameSize) {
    SetError(err, "frame header: body_len exceeds absolute limit");
    return false;
  }
  *out = header;
  return true;
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

bool Metadata::SetExtra(std::uint32_t key, std::string value) {
  // key < 100 是框架保留位，业务不得覆盖（避免"业务写坏了 timeout_ms"这种事故）。
  if (key < static_cast<std::uint32_t>(kMetaUserExtBase)) return false;
  extras_[key] = std::move(value);
  return true;
}

std::string Metadata::GetExtra(std::uint32_t key) const {
  const auto it = extras_.find(key);
  return it == extras_.end() ? std::string() : it->second;
}

std::size_t Metadata::EncodedSize() const {
  // 用计数模式的 Writer 跑一遍 Encode：不分配内存，且与真实编码共用同一段代码，
  // 不可能出现"算出来的长度和写出去的字节数不一致"。
  serde::Writer writer = serde::Writer::Counting();
  if (!Encode(&writer)) return 0;
  return writer.size();
}

bool Metadata::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;
  // 按 key 升序输出：同一份 metadata 永远得到同一串字节（便于 diff 与哈希）。
  if (!trace_id_.empty()) {
    writer->WriteTag(kMetaTraceId, 2);
    writer->WriteString(trace_id_);
  }
  if (has_timeout_) {
    writer->WriteTag(kMetaTimeoutMs, 0);
    writer->WriteVarint32(timeout_ms_);
  }
  if (has_idempotent_) {
    writer->WriteTag(kMetaIdempotent, 0);
    writer->WriteVarint32(idempotent_ ? 1U : 0U);
  }
  // compression 只有非 0 才写：0 是默认值，省略即等价（proto3 语义）。
  if (compression_ != 0) {
    writer->WriteTag(kMetaCompression, 0);
    writer->WriteVarint32(compression_);
  }
  if (!caller_.empty()) {
    writer->WriteTag(kMetaCaller, 2);
    writer->WriteString(caller_);
  }
  if (retry_count_ != 0) {
    writer->WriteTag(kMetaRetryCount, 0);
    writer->WriteVarint32(retry_count_);
  }
  if (deadline_unix_ms_ != 0) {
    writer->WriteTag(kMetaDeadlineUnixMs, 0);
    writer->WriteVarint64(deadline_unix_ms_);
  }
  for (const auto& item : extras_) {
    writer->WriteTag(item.first, 2);
    writer->WriteString(item.second);
  }
  return true;
}

bool Metadata::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t key = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&key, &wire_type)) return false;

    // 说明：已知 key 的 wire_type 不匹配时，按"未知字段"跳过而不是判定协议错误。
    // 理由：未来版本可能给同一个 key 换更紧凑的编码，跳过比断连更利于演进；
    // 而真正越界/非法的 wire_type 会在 SkipField 里失败（那一类必须断连）。
    switch (key) {
      case kMetaTraceId:
        if (wire_type == 2) {
          if (!reader->ReadString(&trace_id_)) return false;
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
      case kMetaTimeoutMs:
        if (wire_type == 0) {
          if (!reader->ReadVarint32(&timeout_ms_)) return false;
          has_timeout_ = true;
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
      case kMetaIdempotent: {
        if (wire_type == 0) {
          std::uint32_t value = 0;
          if (!reader->ReadVarint32(&value)) return false;
          idempotent_ = value != 0;
          has_idempotent_ = true;
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
      }
      case kMetaCompression:
        if (wire_type == 0) {
          if (!reader->ReadVarint32(&compression_)) return false;
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
      case kMetaCaller:
        if (wire_type == 2) {
          if (!reader->ReadString(&caller_)) return false;
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
      case kMetaRetryCount:
        if (wire_type == 0) {
          if (!reader->ReadVarint32(&retry_count_)) return false;
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
      case kMetaDeadlineUnixMs:
        if (wire_type == 0) {
          if (!reader->ReadVarint64(&deadline_unix_ms_)) return false;
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
      default:
        // 用户扩展键（>= kMetaUserExtBase）必须收进 extras_，否则
        // SetExtra/GetExtra 就成了"只写不读"——发出去的键值永远收不回来，
        // 而单机 echo demo 不会暴露这一点（它不读扩展键）。
        //
        // 未来新增的框架保留键（< kMetaUserExtBase）仍然按未知字段跳过，
        // 这是 doc 03 §6.1 承诺的向后兼容性：老版本遇到新版本的字段不报错。
        if (key >= static_cast<std::uint32_t>(kMetaUserExtBase) && wire_type == 2) {
          std::string value;
          if (!reader->ReadString(&value)) return false;
          if (extras_.size() < kMaxUserExtras && value.size() <= kMaxUserExtValueLen) {
            extras_[key] = std::move(value);
          }
          // 超限：静默丢弃（见文件内 kMaxUserExtras 的说明），但字节已经读掉了，
          // 所以流的解析位置仍然正确，后续字段不受影响。
        } else if (!reader->SkipField(wire_type)) {
          return false;
        }
        break;
    }
    if (reader->failed()) return false;
  }
  return !reader->failed();
}

bool Metadata::empty() const noexcept {
  return trace_id_.empty() && caller_.empty() && !has_timeout_ && !has_idempotent_ &&
         compression_ == 0 && retry_count_ == 0 && deadline_unix_ms_ == 0 && extras_.empty();
}

void Metadata::Clear() {
  trace_id_.clear();
  caller_.clear();
  timeout_ms_ = 0;
  compression_ = 0;
  retry_count_ = 0;
  deadline_unix_ms_ = 0;
  idempotent_ = false;
  has_timeout_ = false;
  has_idempotent_ = false;
  extras_.clear();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void Frame::Reset(std::uint32_t seq, std::uint32_t method_id, bool is_request) {
  Clear();
  header.magic = kMagic;
  header.version = kVersion;
  header.flags = is_request ? static_cast<std::uint8_t>(kFlagRequest)
                            : static_cast<std::uint8_t>(kFlagResponse);
  header.status = 0;
  header.seq = seq;
  header.method_id = method_id;
  header.body_len = static_cast<std::uint32_t>(kMetadataLengthSize);  // 空 metadata + 空 payload
}

void Frame::ResetResponse(std::uint32_t seq, std::uint32_t method_id, std::uint16_t status) {
  Reset(seq, method_id, false);
  header.status = status;
}

std::size_t Frame::EncodedSize() const {
  // 以 metadata/payload 的实际内容为准，而不是信任 header.body_len（可能是陈旧的）。
  return kFrameHeaderSize + kMetadataLengthSize + metadata.EncodedSize() + payload.size();
}

void Frame::Clear() {
  header = FrameHeader{};
  metadata.Clear();
  payload.clear();
}

}  // namespace protocol
}  // namespace qzrpc
