// 轻舟 RPC (Qingzhou RPC)
// src/protocol/codec.cpp
//
// 职责：字节流 <-> Frame，粘包/拆包的唯一实现处（doc 03 §8 的隐式状态机）。
//
// 三条不可动摇的规则：
//   1) 半包时**什么都不消费**（连 20 字节头都不消费）。这样连接对象上不需要保存
//      任何 per-connection 解码状态，"状态"完全由读缓冲里现有字节数表达。
//   2) 任何结构性错误一律 kError + 断开，绝不尝试重同步（二进制协议无法安全重同步，
//      猜测会被用来构造攻击面）。
//   3) **绝不**按 body_len 预分配内存：先确认整帧已到达，再按实际需要分配。
//
// metadata 解析用 Reader 子视图（零拷贝）：Reader 直接指向读缓冲内部的区域，
// 解析期间不产生任何中间字符串。

#include "protocol/codec.h"

#include <cstring>
#include <string>

#include "common/endian.h"
#include "serialize/reader.h"

namespace qzrpc {
namespace protocol {

namespace {

std::uint32_t EffectiveLimit(std::uint32_t max_frame_size) {
  if (max_frame_size == 0) return kDefaultMaxFrameSize;
  // 配置写成超过硬上限的值是配置错误：按硬上限收窄，绝不放开。
  return max_frame_size > kAbsoluteMaxFrameSize ? kAbsoluteMaxFrameSize : max_frame_size;
}

void SetError(std::string* err, const std::string& text) {
  if (err != nullptr) *err = text;
}

}  // namespace

Codec::Result Codec::Decode(Buffer* input, Frame* frame, std::uint32_t max_frame_size,
                            std::string* err) {
  if (input == nullptr || frame == nullptr) {
    SetError(err, "codec: null input or frame");
    return Result::kError;
  }
  const std::uint32_t limit = EffectiveLimit(max_frame_size);

  // 状态机第一步：头不全。
  const std::size_t available = input->ReadableBytes();
  if (available < kFrameHeaderSize) return Result::kNeedMore;

  const char* base = input->Peek();
  const std::uint8_t* head = reinterpret_cast<const std::uint8_t*>(base);

  FrameHeader header;
  if (!FrameHeader::DecodeFrom(head, &header, err)) return Result::kError;
  if (!ValidateHeader(header, limit, err)) return Result::kError;

  // 状态机第二步：体不全（此时仍然一个字节都不消费）。
  const std::size_t total = kFrameHeaderSize + static_cast<std::size_t>(header.body_len);
  if (available < total) return Result::kNeedMore;

  // 到这里整帧已在缓冲里：body_len 至少为 4，所以读 metadata_len 不会越界。
  const std::uint32_t metadata_len = LoadBig32(base + kFrameHeaderSize);
  if (metadata_len > header.body_len - static_cast<std::uint32_t>(kMetadataLengthSize)) {
    SetError(err, "codec: metadata_len exceeds body_len - 4");
    return Result::kError;
  }
  const std::size_t payload_len =
      static_cast<std::size_t>(header.body_len) - kMetadataLengthSize - metadata_len;

  // 整帧解析成功后才写 frame：解析失败时 frame 保持调用方原来的内容（不会被写坏）。
  frame->Clear();
  frame->header = header;

  const char* metadata_base = base + kFrameHeaderSize + kMetadataLengthSize;
  if (metadata_len > 0) {
    serde::Reader metadata_reader(metadata_base, metadata_len);
    if (!frame->metadata.Decode(&metadata_reader)) {
      SetError(err, std::string("codec: metadata decode failed: ") + metadata_reader.error());
      return Result::kError;
    }
    if (!metadata_reader.AtEnd()) {
      SetError(err, "codec: trailing bytes inside metadata");
      return Result::kError;
    }
  }
  if (payload_len > 0) {
    frame->payload.assign(metadata_base + metadata_len, payload_len);
  }

  // 最后才消费：任何一步失败都不会破坏输入缓冲。
  input->Retrieve(total);
  return Result::kOk;
}

bool Codec::Encode(Frame* frame, Buffer* output, std::uint32_t max_frame_size) {
  if (frame == nullptr || output == nullptr) return false;
  const std::uint32_t limit = EffectiveLimit(max_frame_size);

  // 先把 metadata 编出来（而不是"先写头再回填 body_len"）：
  // body_len 依赖 metadata 的实际长度，回填写法一旦忘记就会发出错误的帧长度。
  serde::Writer metadata_writer;
  if (!frame->metadata.Encode(&metadata_writer)) return false;
  const std::size_t metadata_len = metadata_writer.size();

  const std::size_t payload_len = frame->payload.size();
  const std::uint64_t body_len = static_cast<std::uint64_t>(kMetadataLengthSize) + metadata_len +
                                 static_cast<std::uint64_t>(payload_len);
  if (body_len > limit) return false;
  if (metadata_len > 0xFFFFFFFFULL || payload_len > 0xFFFFFFFFULL) return false;

  frame->header.body_len = static_cast<std::uint32_t>(body_len);
  if (!ValidateHeader(frame->header, limit, nullptr)) return false;

  // 所有校验都通过后才开始追加，保证失败时 output 不被写脏（不会留下半个帧）。
  std::uint8_t header_bytes[kFrameHeaderSize];
  frame->header.EncodeTo(header_bytes);
  output->Append(header_bytes, sizeof(header_bytes));

  std::uint8_t metadata_len_bytes[kMetadataLengthSize];
  StoreBig32(metadata_len_bytes, static_cast<std::uint32_t>(metadata_len));
  output->Append(metadata_len_bytes, sizeof(metadata_len_bytes));

  if (metadata_len > 0) output->Append(metadata_writer.data().data(), metadata_len);
  if (payload_len > 0) output->Append(frame->payload.data(), payload_len);
  return true;
}

bool Codec::ValidateHeader(const FrameHeader& header, std::uint32_t max_frame_size,
                           std::string* err) {
  const std::uint32_t limit = EffectiveLimit(max_frame_size);
  if (header.magic != kMagic) {
    SetError(err, "codec: bad magic");
    return false;
  }
  if (header.version != kVersion) {
    SetError(err, "codec: unsupported version " + std::to_string(header.version));
    return false;
  }
  if (!IsValidFlags(header.flags)) {
    SetError(err, "codec: invalid flags (request/response must be exactly one, no compression)");
    return false;
  }
  if (header.body_len < kMetadataLengthSize) {
    SetError(err, "codec: body_len < 4");
    return false;
  }
  if (header.body_len > limit) {
    SetError(err, "codec: body_len " + std::to_string(header.body_len) + " exceeds limit " +
                      std::to_string(limit));
    return false;
  }
  return true;
}

std::size_t Codec::PeekFrameSize(const Buffer& input, std::uint32_t max_frame_size,
                                 bool* header_invalid) {
  if (header_invalid != nullptr) *header_invalid = false;
  if (input.ReadableBytes() < kFrameHeaderSize) return 0;

  const std::uint8_t* head = reinterpret_cast<const std::uint8_t*>(input.Peek());
  FrameHeader header;
  if (!FrameHeader::DecodeFrom(head, &header, nullptr)) {
    if (header_invalid != nullptr) *header_invalid = true;
    return 0;
  }
  if (!ValidateHeader(header, max_frame_size, nullptr)) {
    if (header_invalid != nullptr) *header_invalid = true;
    return 0;
  }
  return kFrameHeaderSize + static_cast<std::size_t>(header.body_len);
}

}  // namespace protocol
}  // namespace qzrpc
