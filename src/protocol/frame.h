// 轻舟 RPC (Qingzhou RPC)
// src/protocol/frame.h
//
// 职责：帧的内存表示 —— 定长头 + metadata + payload。
//
// 布局（详见 doc 03）：
//   +--------------------------------------------------+
//   | FrameHeader (20B, 全大端)                          |
//   +--------------------------------------------------+
//   | metadata_len (4B, 大端)                            |
//   | metadata    (metadata_len B, 内部为 protobuf TLV)  |
//   | payload     (body_len - 4 - metadata_len B)        |
//   +--------------------------------------------------+
//   body_len = 4 + metadata_len + payload_len
//
// 为什么 metadata 单独成一个区而不是塞进 payload：
//   框架需要在不解析业务消息的前提下读取 trace_id / timeout / 幂等等信息。
//   如果塞进 payload，服务端必须先反序列化业务消息才能拿 trace_id，
//   而 trace_id 恰恰要在"反序列化失败"时也能用于排查。分区是唯一正确的选择。
//
// 线程安全：Frame 是非线程安全的值对象，只能在单线程内使用（通常在某个 EventLoop 线程）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "protocol/protocol_const.h"
#include "serialize/reader.h"
#include "serialize/writer.h"

namespace qzrpc {
namespace protocol {

// 定长帧头。字段顺序**即**线上顺序。
struct FrameHeader {
  // 魔数，固定 kMagic。
  std::uint32_t magic = kMagic;
  // 协议版本，固定 kVersion。
  std::uint8_t version = kVersion;
  // 标志位，见 FrameFlags。
  std::uint8_t flags = kFlagRequest;
  // 结果码，取值见 qzrpc::StatusCode（请求帧里必须为 0）。
  std::uint16_t status = 0;
  // 请求序号，由客户端生成，用于同一条连接上匹配响应。0 保留（表示"不关心"）。
  std::uint32_t seq = 0;
  // 方法 id，见 MethodIdOfName()。
  std::uint32_t method_id = 0;
  // body 总长度 = 4 + metadata_len + payload_len。
  std::uint32_t body_len = 0;

  // 序列化到 20 字节缓冲。out 至少 kFrameHeaderSize 字节。
  void EncodeTo(std::uint8_t* out) const;

  // 从 20 字节缓冲解析并做完整校验（魔数/版本/flags/长度）。
  // 返回：true 成功；false 失败并把原因写入 err（可为 nullptr）。
  static bool DecodeFrom(const std::uint8_t* in, FrameHeader* out, std::string* err);

  bool is_request() const noexcept { return (flags & kFlagRequest) != 0; }
  bool is_response() const noexcept { return (flags & kFlagResponse) != 0; }
  bool one_way() const noexcept { return (flags & kFlagOneWay) != 0; }
  bool keep_alive() const noexcept { return (flags & kFlagKeepAlive) != 0; }
  bool draining() const noexcept { return (flags & kFlagDraining) != 0; }
};

// 帧的 metadata 区。内部编码是一串 protobuf 风格的 TLV（tag = key<<3 | wire_type）。
class Metadata {
 public:
  Metadata() = default;

  // ---- 标准字段 ----

  // trace_id。空串表示未设置。
  void SetTraceId(std::string trace_id) { trace_id_ = std::move(trace_id); }
  const std::string& trace_id() const noexcept { return trace_id_; }
  bool has_trace_id() const noexcept { return !trace_id_.empty(); }

  // 服务端业务超时（毫秒）。0 表示未设置，服务端使用默认值。
  void SetTimeoutMs(std::uint32_t timeout_ms) { has_timeout_ = true; timeout_ms_ = timeout_ms; }
  std::uint32_t timeout_ms() const noexcept { return timeout_ms_; }
  bool has_timeout() const noexcept { return has_timeout_; }

  // 幂等标记。只有标记为幂等的调用，客户端重试才被允许。
  void SetIdempotent(bool on) { has_idempotent_ = true; idempotent_ = on; }
  bool idempotent() const noexcept { return idempotent_; }
  bool has_idempotent() const noexcept { return has_idempotent_; }

  // 压缩算法。v1 只接受 0（无压缩）。
  void SetCompression(std::uint32_t algo) { compression_ = algo; }
  std::uint32_t compression() const noexcept { return compression_; }

  // 调用方服务名。
  void SetCaller(std::string caller) { caller_ = std::move(caller); }
  const std::string& caller() const noexcept { return caller_; }
  bool has_caller() const noexcept { return !caller_.empty(); }

  // 当前是第几次重试（首次为 0）。
  void SetRetryCount(std::uint32_t n) { retry_count_ = n; }
  std::uint32_t retry_count() const noexcept { return retry_count_; }

  // 绝对 deadline（Unix 毫秒）。0 表示未设置。
  void SetDeadlineUnixMs(std::uint64_t ms) { deadline_unix_ms_ = ms; }
  std::uint64_t deadline_unix_ms() const noexcept { return deadline_unix_ms_; }

  // ---- 用户扩展 ----

  // 设置扩展键（key 必须 >= kMetaUserExtBase，否则返回 false）。
  // 值按 wire_type=2（length-delimited 字符串）编码。
  bool SetExtra(std::uint32_t key, std::string value);

  // 读取扩展键。不存在时返回空串。
  std::string GetExtra(std::uint32_t key) const;

  // 全部扩展键值（只读）。
  const std::map<std::uint32_t, std::string>& extras() const noexcept { return extras_; }

  // ---- 编解码 ----

  // 编码后的字节数（不含 4 字节长度前缀）。
  std::size_t EncodedSize() const;

  // 编码到 writer（**不写** 4 字节长度前缀，前缀由 Codec 负责）。
  bool Encode(serde::Writer* writer) const;

  // 从 reader 解析（范围恰好是 metadata 区，**不含**长度前缀）。
  bool Decode(serde::Reader* reader);

  // 是否为空（无任何字段）。
  bool empty() const noexcept;

  // 清空。
  void Clear();

 private:
  std::string trace_id_;
  std::string caller_;
  std::uint32_t timeout_ms_{0};
  std::uint32_t compression_{0};
  std::uint32_t retry_count_{0};
  std::uint64_t deadline_unix_ms_{0};
  bool idempotent_{false};
  bool has_timeout_{false};
  bool has_idempotent_{false};
  std::map<std::uint32_t, std::string> extras_;
};

// 一帧的完整内存表示。
class Frame {
 public:
  Frame() = default;

  // 重置为"一个空请求帧"。
  // 参数：seq - 请求序号；method_id - 方法 id；is_request - true 请求 / false 响应。
  void Reset(std::uint32_t seq, std::uint32_t method_id, bool is_request);

  // 重置为指定结果码的响应帧（服务端回包用）。
  void ResetResponse(std::uint32_t seq, std::uint32_t method_id, std::uint16_t status);

  // 整帧的线上字节数（20 + body_len）。
  std::size_t EncodedSize() const;

  // 清空为初始状态。
  void Clear();

  // 定长头。
  FrameHeader header;
  // metadata 区。
  Metadata metadata;
  // 业务消息体（protobuf 编码后的字节）。
  std::string payload;
};

}  // namespace protocol
}  // namespace qzrpc
