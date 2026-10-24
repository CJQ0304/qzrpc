// 轻舟 RPC (Qingzhou RPC)
// tests/test_codec.cpp
//
// doc 03 §14 自检清单里属于"编解码层"的七项（第 8/9 项是连接/并发语义，归 test_e2e_echo）。
// 第 1 项（37 字节 golden）是整个项目第一个"真的能跑"的里程碑，这里逐字节固化。
//
// 为什么这些断言必须进 ctest 而不是留在某个人的 .scratch 里：
// 帧头是**大端**、payload 里的 fixed 族是**小端**、长度前缀是 varint ——
// 三种字节序/编码规则同时存在于一条报文里，任何一处搞反都不会让编译或类型检查报错，
// 只会在与真实对端互通时表现为"字段值全错"。

#include <cstdint>
#include <string>

#include "common/buffer.h"
#include "common/endian.h"
#include "common/status.h"
#include "common/string_util.h"
#include "protocol/codec.h"
#include "protocol/frame.h"
#include "protocol/protocol_const.h"
#include "serialize/reader.h"
#include "test_main.h"

namespace {

using qzrpc::protocol::Codec;
using qzrpc::protocol::Frame;
using qzrpc::protocol::FrameHeader;

// doc 03 §10 的 37 字节 golden 请求帧：
//   seq=1, flags=Request|KeepAlive, trace_id="ab12", timeout_ms=1000, EchoRequest{message="hi"}
constexpr char kGoldenRequestHex[] =
    "515A5250"    // magic 'QZRP'
    "01"          // version
    "05"          // flags = Request | KeepAlive
    "0000"        // status = OK
    "00000001"    // seq = 1
    "21678893"    // method_id = FNV-1a64("demo.EchoService.Echo") & 0xFFFFFFFF
    "00000011"    // body_len = 17
    "00000009"    // metadata_len = 9
    "0A0461623132"  // key=1 (trace_id), wt=2, len=4, "ab12"
    "10E807"        // key=2 (timeout_ms), wt=0, varint 1000
    "0A026869";     // payload: tag(field=1,wt=2) len=2 "hi"

// doc 03 §10 的 28 字节 golden 响应帧：seq=1, flags=Response, status=OK, 同一 payload。
constexpr char kGoldenResponseHex[] =
    "515A5250" "01" "02" "0000" "00000001" "21678893" "00000008" "00000000" "0A026869";

constexpr std::size_t kGoldenRequestSize = 37;
constexpr std::size_t kGoldenResponseSize = 28;

std::string Bytes(const char* hex) {
  std::string out;
  const bool ok = qzrpc::StringUtil::HexDecode(std::string(hex), &out);
  QZ_EXPECT(ok);
  return out;
}

std::string ToHex(const std::string& raw) {
  return qzrpc::StringUtil::HexEncode(raw.data(), raw.size());
}

std::string GoldenRequest() { return Bytes(kGoldenRequestHex); }
std::string GoldenResponse() { return Bytes(kGoldenResponseHex); }

// 就地改写若干字节，用于构造"只有一处非法"的恶意帧 —— 比手写整帧更不容易写错。
void Patch(std::string* raw, std::size_t offset, const char* hex) {
  const std::string patch = Bytes(hex);
  QZ_EXPECT(offset + patch.size() <= raw->size());
  for (std::size_t i = 0; i < patch.size(); ++i) {
    (*raw)[offset + i] = patch[i];
  }
}

// 把整段字节塞进 Buffer 后解一帧。
Codec::Result DecodeFromBytes(const std::string& raw, Frame* frame, std::string* err = nullptr,
                              std::uint32_t max_frame_size = 0) {
  qzrpc::Buffer input;
  input.Append(raw);
  return Codec::Decode(&input, frame, max_frame_size, err);
}

// ---------------------------------------------------------------------------
// §14 第 1 项：37 字节 golden 能解出全部字段
// ---------------------------------------------------------------------------

void TestGoldenRequestDecode() {
  const std::string raw = GoldenRequest();
  QZ_CHECK_EQ(raw.size(), kGoldenRequestSize);

  qzrpc::Buffer input;
  input.Append(raw);

  Frame frame;
  std::string err;
  QZ_CHECK(Codec::Decode(&input, &frame, 0, &err) == Codec::Result::kOk);
  QZ_CHECK(err.empty());
  QZ_CHECK_EQ(input.ReadableBytes(), std::size_t{0});  // 整帧被消费

  QZ_CHECK_EQ(frame.header.magic, qzrpc::protocol::kMagic);
  QZ_CHECK_EQ(frame.header.version, qzrpc::protocol::kVersion);
  QZ_CHECK_EQ(frame.header.flags, static_cast<std::uint8_t>(0x05));
  QZ_CHECK(frame.header.is_request());
  QZ_CHECK(frame.header.keep_alive());
  QZ_CHECK(!frame.header.is_response());
  QZ_CHECK_EQ(frame.header.status, static_cast<std::uint16_t>(0));
  QZ_CHECK_EQ(frame.header.seq, 1U);
  QZ_CHECK_EQ(frame.header.method_id, 0x21678893U);
  QZ_CHECK_EQ(frame.header.body_len, 17U);

  QZ_CHECK(frame.metadata.has_trace_id());
  QZ_CHECK_EQ(frame.metadata.trace_id(), std::string("ab12"));
  QZ_CHECK(frame.metadata.has_timeout());
  QZ_CHECK_EQ(frame.metadata.timeout_ms(), 1000U);
  QZ_CHECK(!frame.metadata.has_idempotent());  // 帧里没带这一项

  // payload 是一条 protobuf 消息：EchoRequest{message="hi"}
  QZ_CHECK_EQ(ToHex(frame.payload), std::string("0a026869"));
  qzrpc::serde::Reader payload_reader(frame.payload.data(), frame.payload.size());
  std::uint32_t field = 0;
  std::uint8_t wire = 0;
  QZ_CHECK(payload_reader.ReadTag(&field, &wire));
  QZ_CHECK_EQ(field, 1U);
  QZ_CHECK_EQ(wire, 2);
  std::string message;
  QZ_CHECK(payload_reader.ReadString(&message));
  QZ_CHECK_EQ(message, std::string("hi"));
  QZ_CHECK(payload_reader.AtEnd());
}

// 反向：同样的语义字段编码回去必须**逐字节**相同。
void TestGoldenRequestReencode() {
  Frame frame;
  frame.Reset(1, 0x21678893U, true);
  frame.header.flags = static_cast<std::uint8_t>(qzrpc::protocol::kFlagRequest |
                                                 qzrpc::protocol::kFlagKeepAlive);
  frame.metadata.SetTraceId("ab12");
  frame.metadata.SetTimeoutMs(1000);
  frame.payload = Bytes("0A026869");

  qzrpc::Buffer output;
  QZ_CHECK(Codec::Encode(&frame, &output));
  QZ_CHECK_EQ(output.ReadableBytes(), kGoldenRequestSize);
  QZ_CHECK_EQ(ToHex(std::string(output.Peek(), output.ReadableBytes())),
              ToHex(GoldenRequest()));
  // Encode 会回写 body_len
  QZ_CHECK_EQ(frame.header.body_len, 17U);
}

void TestGoldenResponse() {
  // 解码
  Frame decoded;
  QZ_CHECK(DecodeFromBytes(GoldenResponse(), &decoded) == Codec::Result::kOk);
  QZ_CHECK(decoded.header.is_response());
  QZ_CHECK(!decoded.header.is_request());
  QZ_CHECK_EQ(decoded.header.flags, static_cast<std::uint8_t>(0x02));
  QZ_CHECK_EQ(decoded.header.status, static_cast<std::uint16_t>(qzrpc::StatusCode::kOk));
  QZ_CHECK_EQ(decoded.header.seq, 1U);
  QZ_CHECK_EQ(decoded.header.body_len, 8U);
  QZ_CHECK(decoded.metadata.empty());

  // 编码
  Frame frame;
  frame.ResetResponse(1, 0x21678893U, static_cast<std::uint16_t>(qzrpc::StatusCode::kOk));
  frame.payload = Bytes("0A026869");
  qzrpc::Buffer output;
  QZ_CHECK(Codec::Encode(&frame, &output));
  QZ_CHECK_EQ(output.ReadableBytes(), kGoldenResponseSize);
  QZ_CHECK_EQ(ToHex(std::string(output.Peek(), output.ReadableBytes())),
              ToHex(GoldenResponse()));
}

// ---------------------------------------------------------------------------
// §14 第 2 项：任意位置切断，第一次必须 kNeedMore 且**一个字节都不消费**
// ---------------------------------------------------------------------------

void TestSplitAtEveryOffset() {
  const std::string raw = GoldenRequest();

  for (std::size_t split = 0; split <= raw.size(); ++split) {
    qzrpc::Buffer input;
    input.Append(raw.data(), split);

    Frame frame;
    std::string err;
    const Codec::Result first = Codec::Decode(&input, &frame, 0, &err);
    if (split == raw.size()) {
      // 整帧一次喂入：直接成功
      QZ_CHECK(first == Codec::Result::kOk);
      continue;
    }
    QZ_CHECK(first == Codec::Result::kNeedMore);
    QZ_CHECK_EQ(input.ReadableBytes(), split);  // 半包时未消费任何字节
    QZ_CHECK(err.empty());                      // 半包不是错误，不该写 err
    QZ_CHECK_EQ(frame.header.seq, 0U);          // 失败时不写 frame

    // 补齐剩余字节再解一次
    input.Append(raw.data() + split, raw.size() - split);
    QZ_CHECK(Codec::Decode(&input, &frame, 0, &err) == Codec::Result::kOk);
    QZ_CHECK_EQ(frame.header.seq, 1U);
    QZ_CHECK_EQ(input.ReadableBytes(), std::size_t{0});
  }
}

// ---------------------------------------------------------------------------
// §14 第 3 项：粘包 —— 两帧粘在一起一次喂入，连续两次 kOk
// ---------------------------------------------------------------------------

void TestTwoFramesStuckTogether() {
  const std::string raw = GoldenRequest() + GoldenResponse();
  qzrpc::Buffer input;
  input.Append(raw);
  QZ_CHECK_EQ(input.ReadableBytes(), kGoldenRequestSize + kGoldenResponseSize);

  Frame first;
  QZ_CHECK(Codec::Decode(&input, &first) == Codec::Result::kOk);
  QZ_CHECK(first.header.is_request());
  QZ_CHECK_EQ(input.ReadableBytes(), kGoldenResponseSize);  // 只消费了第一帧

  Frame second;
  QZ_CHECK(Codec::Decode(&input, &second) == Codec::Result::kOk);
  QZ_CHECK(second.header.is_response());
  QZ_CHECK_EQ(second.header.seq, 1U);
  QZ_CHECK_EQ(input.ReadableBytes(), std::size_t{0});

  // 第三次必须回到"等更多数据"，而不是报错
  Frame third;
  QZ_CHECK(Codec::Decode(&input, &third) == Codec::Result::kNeedMore);
}

// ---------------------------------------------------------------------------
// §14 第 4 项：body_len = 0xFFFFFFFF 必须报错，且**绝不预分配**
//
// "没有发生大内存分配"的可观测代理：
//   * 读缓冲的 capacity 一点没变（没有按 body_len 预留）；
//   * 一个字节都没被消费；
//   * frame 的 payload 仍是空（没有 reserve/assign）。
// 真去测进程 RSS 在单测里既不稳定也不可移植。
// ---------------------------------------------------------------------------

void TestHugeBodyLenRejected() {
  std::string raw = GoldenRequest();
  Patch(&raw, 16, "FFFFFFFF");  // body_len

  qzrpc::Buffer input;
  input.Append(raw);
  const std::size_t capacity_before = input.capacity();
  const std::size_t readable_before = input.ReadableBytes();

  Frame frame;
  std::string err;
  QZ_CHECK(Codec::Decode(&input, &frame, 0, &err) == Codec::Result::kError);
  QZ_CHECK(!err.empty());

  QZ_CHECK_EQ(input.capacity(), capacity_before);  // 没有按 body_len 扩容
  QZ_CHECK_EQ(input.ReadableBytes(), readable_before);
  QZ_CHECK_EQ(frame.payload.size(), std::size_t{0});
  QZ_CHECK_EQ(frame.header.body_len, 0U);  // 失败时不写 frame
}

// ---------------------------------------------------------------------------
// §14 第 5 项：magic 改错必须 kError
// ---------------------------------------------------------------------------

void TestBadMagic() {
  std::string raw = GoldenRequest();
  Patch(&raw, 0, "515A5251");  // 'QZRQ'
  Frame frame;
  std::string err;
  QZ_CHECK(DecodeFromBytes(raw, &frame, &err) == Codec::Result::kError);
  QZ_CHECK(!err.empty());

  // 全部 4 个 magic 字节都要参与校验，不能只比对第一个
  for (std::size_t offset = 0; offset < 4; ++offset) {
    std::string mutated = GoldenRequest();
    mutated[offset] = static_cast<char>(mutated[offset] ^ 0xFF);
    QZ_CHECK(DecodeFromBytes(mutated, &frame) == Codec::Result::kError);
  }
}

// ---------------------------------------------------------------------------
// §14 第 6 项：请求帧 status != 0
//
// 契约（frame.cpp:12 的注释 + 霍承钧的实现）：**codec 层不拒绝**，原样保留 status，
// 由服务端层映射成 INVALID_ARGUMENT 响应。理由是"能读到帧就不该断连"（doc 03 §9）。
// 这里断言的是这条分工本身：Decode == kOk 且 header.status 保真。
// 若将来裁决改成 codec 直接 kError，这个断言要跟着改 —— 但那样服务端就没法回
// 带原始 seq 的错误响应了（因为它拿不到 seq）。
// ---------------------------------------------------------------------------

void TestRequestWithNonZeroStatusReachesServer() {
  std::string raw = GoldenRequest();
  Patch(&raw, 6, "0005");  // status = 5 (kOverload)，制造一个"请求却带错误码"的非法帧

  Frame frame;
  std::string err;
  QZ_CHECK(DecodeFromBytes(raw, &frame, &err) == Codec::Result::kOk);
  QZ_CHECK(frame.header.is_request());
  QZ_CHECK_EQ(frame.header.status, static_cast<std::uint16_t>(5));
  QZ_CHECK_EQ(frame.header.seq, 1U);  // seq 必须保真，服务端要靠它回错误响应
  QZ_CHECK(err.empty());

  // 对应的正确做法：服务端用同一个 seq 回 kInvalidArgument
  Frame response;
  response.ResetResponse(frame.header.seq, frame.header.method_id,
                         static_cast<std::uint16_t>(qzrpc::StatusCode::kInvalidArgument));
  QZ_CHECK_EQ(response.header.seq, 1U);
  QZ_CHECK_EQ(response.header.status, static_cast<std::uint16_t>(1));
  QZ_CHECK(response.header.is_response());
}

// ---------------------------------------------------------------------------
// 其余头部校验：version / flags / metadata_len
// ---------------------------------------------------------------------------

void TestHeaderValidation() {
  Frame frame;
  std::string err;

  {
    std::string raw = GoldenRequest();
    Patch(&raw, 4, "02");  // version = 2
    QZ_CHECK(DecodeFromBytes(raw, &frame, &err) == Codec::Result::kError);
  }
  {
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "00");  // 既不是请求也不是响应
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "03");  // 请求与响应同时置位
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "11");  // Request | Compressed：v1 不支持压缩
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    // 未定义位 0x40 / 0x80：v1 无"关键位/非关键位"协商机制，必须显式拒绝。
    // 放行 = "不认识这个位但假装它不存在"，v2 一旦用这两位改语义，v1 会静默误解析。
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "45");  // Request | KeepAlive(0x04) | 0x40
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "82");  // Response(0x02) + 未定义位(0x80)：单看 req/resp 是合法的，靠的是未定义位这一条
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "85");  // Request(0x01)+KeepAlive(0x04)+未定义位(0x80)
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    // 同一类帧走 PeekFrameSize：header_invalid 必须被置起，且返回 0 字节长度，
    // 否则上层会拿一个"长度可信、内容非法"的帧继续收包（等于把坏帧当粘包处理）。
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "45");
    qzrpc::Buffer input;
    input.Append(raw.data(), raw.size());
    bool header_invalid = false;
    const std::size_t size = Codec::PeekFrameSize(input, 0, &header_invalid);
    QZ_CHECK(header_invalid);
    QZ_CHECK_EQ(size, std::size_t{0});
  }
  {
    // metadata_len (14) > body_len - 4 (13)
    std::string raw = GoldenRequest();
    Patch(&raw, 20, "0000000E");
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    // body_len < 4：连 metadata 长度前缀都放不下
    std::string raw = GoldenRequest();
    Patch(&raw, 16, "00000003");
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kError);
  }
  {
    // 合法的扩展 flag：Request | OneWay 必须被接受（OneWay 是正常语义）
    std::string raw = GoldenRequest();
    Patch(&raw, 5, "09");
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kOk);
    QZ_CHECK(frame.header.one_way());
  }
  {
    // 响应帧带 Draining 位也要能解（停机期间服务端会这么回）
    std::string raw = GoldenResponse();
    Patch(&raw, 5, "22");
    QZ_CHECK(DecodeFromBytes(raw, &frame) == Codec::Result::kOk);
    QZ_CHECK(frame.header.draining());
    QZ_CHECK(frame.header.is_response());
  }
}

// ---------------------------------------------------------------------------
// max_frame_size 上限（doc 03 §9）
// ---------------------------------------------------------------------------

void TestMaxFrameSize() {
  Frame frame;
  // body_len = 17，限制 10 -> 拒绝
  QZ_CHECK(DecodeFromBytes(GoldenRequest(), &frame, nullptr, 10) == Codec::Result::kError);
  // 限制恰好等于 body_len -> 接受
  QZ_CHECK(DecodeFromBytes(GoldenRequest(), &frame, nullptr, 17) == Codec::Result::kOk);
  // 传 0 表示用默认值（16 MiB）-> 接受
  QZ_CHECK(DecodeFromBytes(GoldenRequest(), &frame, nullptr, 0) == Codec::Result::kOk);
  // 传一个超过硬上限的值：必须被夹到 kAbsoluteMaxFrameSize，而不是采信调用方
  QZ_CHECK(DecodeFromBytes(GoldenRequest(), &frame, nullptr, 0xFFFFFFFFU) == Codec::Result::kOk);

  // Encode 方向同样受上限约束
  Frame big;
  big.Reset(2, 0x21678893U, true);
  big.payload.assign(64, 'x');
  qzrpc::Buffer output;
  QZ_CHECK(!Codec::Encode(&big, &output, 20));  // body 68 > 20
  QZ_CHECK_EQ(output.ReadableBytes(), std::size_t{0});  // 失败时不留半个帧
  QZ_CHECK(Codec::Encode(&big, &output, 0));
}

// ---------------------------------------------------------------------------
// PeekFrameSize：先判断整帧是否到达，再决定是否分配
// ---------------------------------------------------------------------------

void TestPeekFrameSize() {
  {
    qzrpc::Buffer input;
    input.Append(GoldenRequest());
    bool invalid = false;
    QZ_CHECK_EQ(Codec::PeekFrameSize(input, 0, &invalid), kGoldenRequestSize);
    QZ_CHECK(!invalid);
    QZ_CHECK_EQ(input.ReadableBytes(), kGoldenRequestSize);  // 窥探不消费
  }
  {
    qzrpc::Buffer input;
    input.Append(GoldenRequest().data(), 19);  // 头都不全
    bool invalid = false;
    QZ_CHECK_EQ(Codec::PeekFrameSize(input, 0, &invalid), std::size_t{0});
    QZ_CHECK(!invalid);
  }
  {
    // 头全、体不全：**返回整帧应有的大小**，由调用方与 ReadableBytes() 比较来决定
    // 是否继续等（这是 PeekFrameSize 的契约："若头部已完整，返回该帧线上总字节数"）。
    qzrpc::Buffer input;
    input.Append(GoldenRequest().data(), 25);
    bool invalid = false;
    QZ_CHECK_EQ(Codec::PeekFrameSize(input, 0, &invalid), kGoldenRequestSize);
    QZ_CHECK(!invalid);
    QZ_CHECK(input.ReadableBytes() < Codec::PeekFrameSize(input, 0, &invalid));  // 还没到齐
  }
  {
    std::string raw = GoldenRequest();
    Patch(&raw, 0, "00000000");  // 坏 magic
    qzrpc::Buffer input;
    input.Append(raw);
    bool invalid = false;
    QZ_CHECK_EQ(Codec::PeekFrameSize(input, 0, &invalid), std::size_t{0});
    QZ_CHECK(invalid);
  }
}

// ---------------------------------------------------------------------------
// metadata 往返：标准键 + 扩展键 + 未知键跳过
// ---------------------------------------------------------------------------

void TestMetadataRoundTrip() {
  Frame frame;
  frame.Reset(7, 0x11223344U, true);
  frame.metadata.SetTraceId("0123456789abcdef0123456789abcdef");
  frame.metadata.SetTimeoutMs(5000);
  frame.metadata.SetIdempotent(true);
  frame.metadata.SetCompression(0);
  frame.metadata.SetCaller("demo.bench");
  frame.metadata.SetRetryCount(2);
  frame.metadata.SetDeadlineUnixMs(1714559696789ULL);
  QZ_CHECK(frame.metadata.SetExtra(100, "ext-a"));
  QZ_CHECK(frame.metadata.SetExtra(101, "ext-b"));
  QZ_CHECK(!frame.metadata.SetExtra(1, "标准键不算扩展"));  // key < kMetaUserExtBase
  frame.payload = Bytes("0A026869");

  qzrpc::Buffer output;
  QZ_CHECK(Codec::Encode(&frame, &output));

  Frame decoded;
  QZ_CHECK(Codec::Decode(&output, &decoded) == Codec::Result::kOk);
  QZ_CHECK_EQ(decoded.metadata.trace_id(), frame.metadata.trace_id());
  QZ_CHECK_EQ(decoded.metadata.timeout_ms(), 5000U);
  QZ_CHECK(decoded.metadata.has_idempotent());
  QZ_CHECK(decoded.metadata.idempotent());
  QZ_CHECK_EQ(decoded.metadata.caller(), std::string("demo.bench"));
  QZ_CHECK_EQ(decoded.metadata.retry_count(), 2U);
  QZ_CHECK_EQ(decoded.metadata.deadline_unix_ms(), 1714559696789ULL);
  QZ_CHECK_EQ(decoded.metadata.GetExtra(100), std::string("ext-a"));
  QZ_CHECK_EQ(decoded.metadata.GetExtra(101), std::string("ext-b"));
  QZ_CHECK_EQ(decoded.metadata.GetExtra(999), std::string());  // 不存在 -> 空串
  QZ_CHECK_EQ(decoded.payload, frame.payload);
  QZ_CHECK_EQ(decoded.header.seq, 7U);
}

// 未知 metadata 键必须被跳过而不是整体报错（前向兼容：新版本客户端发来的新键，
// 老服务端要能忽略它继续服务）。
void TestUnknownMetadataKeySkipped() {
  Frame frame;
  frame.Reset(1, 0x21678893U, true);
  frame.metadata.SetTimeoutMs(100);

  qzrpc::Buffer output;
  QZ_CHECK(Codec::Encode(&frame, &output));
  const std::string base(output.Peek(), output.ReadableBytes());
  QZ_CHECK(base.size() > 24);
  const std::size_t metadata_len = base.size() - 24;  // 20 字节头 + 4 字节长度前缀

  // 在 metadata 末尾插入一个"未来版本才有"的键：key=200, wt=0, varint(7)
  const std::string extra = Bytes("C00C07");  // tag=(200<<3)|0=1600 -> varint C0 0C，值 07
  std::string raw = base;
  raw.insert(24 + metadata_len, extra);

  auto big32 = [](std::uint32_t v) {
    std::uint8_t buf[4];
    qzrpc::StoreBig32(buf, v);
    return std::string(reinterpret_cast<const char*>(buf), sizeof(buf));
  };
  raw.replace(20, 4, big32(static_cast<std::uint32_t>(metadata_len + extra.size())));  // metadata_len
  raw.replace(16, 4, big32(static_cast<std::uint32_t>(4 + metadata_len + extra.size())));  // body_len

  Frame decoded;
  std::string err;
  QZ_CHECK(DecodeFromBytes(raw, &decoded, &err) == Codec::Result::kOk);
  QZ_CHECK_EQ(decoded.metadata.timeout_ms(), 100U);
  QZ_CHECK_EQ(decoded.header.seq, 1U);
}

// ---------------------------------------------------------------------------
// method_id 与 codegen 的一致性（跨语言 golden）
// ---------------------------------------------------------------------------

void TestMethodIdGolden() {
  // doc 03 §7/§10 给出的定值；Python 侧 test_methodid.py 用同一组全名交叉验证
  QZ_CHECK_EQ(qzrpc::protocol::MethodIdOfName("demo.EchoService.Echo"), 0x21678893U);
  QZ_CHECK_EQ(qzrpc::protocol::MethodIdOfName("demo.EchoService.Notify"),
              qzrpc::protocol::MethodIdOfName(std::string("demo.EchoService.Notify")));
  // 大小写敏感：protobuf/IDL 全名不做归一化
  QZ_CHECK(qzrpc::protocol::MethodIdOfName("demo.EchoService.echo") !=
           qzrpc::protocol::MethodIdOfName("demo.EchoService.Echo"));
}

}  // namespace

int main() {
  TestGoldenRequestDecode();
  TestGoldenRequestReencode();
  TestGoldenResponse();
  TestSplitAtEveryOffset();
  TestTwoFramesStuckTogether();
  TestHugeBodyLenRejected();
  TestBadMagic();
  TestRequestWithNonZeroStatusReachesServer();
  TestHeaderValidation();
  TestMaxFrameSize();
  TestPeekFrameSize();
  TestMetadataRoundTrip();
  TestUnknownMetadataKeySkipped();
  TestMethodIdGolden();
  return qztest::Summary("test_codec");
}
