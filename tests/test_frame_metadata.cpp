// 轻舟 RPC (Qingzhou RPC)
// tests/test_frame_metadata.cpp
//
// 帧头 + metadata 的**契约测试**（T-06）。与 test_codec 的分工：
//   test_codec 关心"整帧在字节流里长什么样、如何拆包"（粘包/半包/长度校验）；
//   本文件关心"FrameHeader 的字段级校验"与"Metadata 的编解码语义"。
//
// 为什么 metadata 值得单测：
//   它是唯一一处"同一个 key 在不同版本里可以有不同 wire_type、且必须静默跳过"的协议区，
//   前向兼容全部压在这里。而它又不在 payload 里，业务方（甚至 echo demo）根本不读它 ——
//   也就是说这里的 bug 不会在 demo 里暴露，只会在"线上要按 trace_id 查日志时发现查不到"。
//   实测踩过的一个真实例子：Encode 侧写出去的扩展键，Decode 侧被 default 分支一律
//   SkipField 掉，往返静默丢数据（见 TestExtrasRoundTrip 的注释）。
//
// 钉住的几条容易"被改坏"的契约：
//   1. 帧头 20 字节**全大端**（与 payload 里的 fixed 小端相反）；
//   2. has_xxx 与值为 0 是两件事：SetTimeoutMs(0) 必须真的写出 tag+0，否则
//      "显式要求 0 超时"会被对端当成"未设置"；
//   3. compression==0 / retry_count==0 / deadline==0 则**省略不写**（proto3 语义），
//      因此"编码后的字节数"不是各字段之和；
//   4. 编码按 key 升序，同一份 metadata 永远得到同一串字节（便于 diff / 哈希）；
//   5. 解码对未知 key、wire_type 不匹配一律**跳过而不报错**（前向兼容），
//      只有真正越界/截断才失败；
//   6. 扩展键在接收侧有上限（32 个键 / 单值 1024 字节），超限**静默丢弃**而不是断连。

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/string_util.h"
#include "protocol/frame.h"
#include "protocol/protocol_const.h"
#include "serialize/reader.h"
#include "serialize/writer.h"
#include "test_main.h"

namespace {

using qzrpc::protocol::Frame;
using qzrpc::protocol::FrameHeader;
using qzrpc::protocol::IsValidFlags;
using qzrpc::protocol::Metadata;
using qzrpc::protocol::kAbsoluteMaxFrameSize;
using qzrpc::protocol::kFlagCompressed;
using qzrpc::protocol::kFlagDraining;
using qzrpc::protocol::kFlagKeepAlive;
using qzrpc::protocol::kFlagOneWay;
using qzrpc::protocol::kFlagRequest;
using qzrpc::protocol::kFlagResponse;
using qzrpc::protocol::kFrameHeaderSize;
using qzrpc::protocol::kKnownFlagsMask;
using qzrpc::protocol::kMagic;
using qzrpc::protocol::kMetaUserExtBase;
using qzrpc::protocol::kVersion;
using qzrpc::serde::Reader;
using qzrpc::serde::Writer;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string Hex(const void* data, std::size_t len) {
  return qzrpc::StringUtil::HexEncode(data, len);
}

std::string Hex(const std::string& raw) { return Hex(raw.data(), raw.size()); }

// 十六进制字面量 -> 字节串。用解析器而不是手写 \xNN：手写的转义在 UTF-8 源文件里
// 容易与中文字符的字节混淆，且抄错一位不会有任何提示。
std::string Bytes(const char* hex) {
  std::string out;
  const bool ok = qzrpc::StringUtil::HexDecode(std::string(hex), &out);
  QZ_EXPECT(ok);
  return out;
}

// 把 metadata 编码成裸字节（不含 4 字节长度前缀，那是 Codec 的事）。
std::string EncodeMeta(const Metadata& meta) {
  Writer writer;
  const bool ok = meta.Encode(&writer);
  QZ_EXPECT(ok);
  return writer.data();
}

// 从裸字节解码 metadata。
bool DecodeMeta(const std::string& raw, Metadata* out) {
  Reader reader(raw.data(), raw.size());
  return out->Decode(&reader);
}

// 依次读出流里的每个 tag 的 key，用来验证"编码按 key 升序"。
std::vector<std::uint32_t> TagKeys(const std::string& raw) {
  std::vector<std::uint32_t> keys;
  Reader reader(raw.data(), raw.size());
  while (!reader.AtEnd()) {
    std::uint32_t key = 0;
    std::uint8_t wire_type = 0;
    if (!reader.ReadTag(&key, &wire_type)) { break; }
    keys.push_back(key);
    if (!reader.SkipField(wire_type)) { break; }
  }
  return keys;
}

// 造一个"字段齐全"的 metadata，供顺序/尺寸/往返类用例复用。
void FillAllFields(Metadata* meta) {
  meta->SetTraceId("0123456789abcdef0123456789abcdef");
  meta->SetTimeoutMs(5000);
  meta->SetIdempotent(true);
  meta->SetCompression(1);
  meta->SetCaller("demo.caller");
  meta->SetRetryCount(3);
  meta->SetDeadlineUnixMs(1714559696789ULL);
  QZ_EXPECT(meta->SetExtra(100, "ext-a"));
  QZ_EXPECT(meta->SetExtra(101, "ext-b"));
}

// ===========================================================================
// A. FrameHeader
// ===========================================================================

// 帧头 20 字节的**逐字节**布局。这是全项目唯二的大端区（另一处是 metadata 长度前缀），
// 而 payload 里的 fixed32/64 是小端 —— 同一条报文两种字节序，写反了不会报错，
// 只会在对端表现为字段值全错。所以这里比对十六进制串，而不是"编码后再解码相等"
// （后者对"编码和解码同时写反"完全无感）。
void TestHeaderByteLayout() {
  FrameHeader header;
  header.magic = kMagic;
  header.version = kVersion;
  header.flags = static_cast<std::uint8_t>(kFlagRequest | kFlagKeepAlive);
  header.status = 0;
  header.seq = 0x01020304U;
  header.method_id = 0x21678893U;
  header.body_len = 0x00000011U;

  std::uint8_t buffer[kFrameHeaderSize];
  std::memset(buffer, 0xCC, sizeof(buffer));
  header.EncodeTo(buffer);

  // 51 5A 52 50 | 01 | 05 | 0000 | 01020304 | 21678893 | 00000011
  QZ_CHECK_EQ(Hex(buffer, sizeof(buffer)),
              std::string("515a525001050000" "01020304" "21678893" "00000011"));

  // status 非 0 时同样是**大端**两字节（小端会写成 02 01）。
  header.status = 0x0102U;
  header.body_len = 0xFFFFFFFFU;
  header.EncodeTo(buffer);
  QZ_CHECK_EQ(Hex(buffer, sizeof(buffer)),
              std::string("515a525001050102" "01020304" "21678893" "ffffffff"));

  // EncodeTo(nullptr) 是空操作而不是崩溃（调用方传空不该炸进程）。
  header.EncodeTo(nullptr);

  // 解码回来逐字段相等。注意先把 body_len 换回合法值：0xFFFFFFFF 超过硬上限，
  // 那个组合属于"编码器不管、解码器必拒"的非法帧（下一组用例专门测它）。
  header.body_len = 0x00000011U;
  header.status = 0x0102U;
  header.EncodeTo(buffer);

  FrameHeader decoded;
  std::string err;
  QZ_CHECK(FrameHeader::DecodeFrom(buffer, &decoded, &err));
  QZ_CHECK(err.empty());
  QZ_CHECK_EQ(decoded.magic, kMagic);
  QZ_CHECK_EQ(decoded.version, kVersion);
  // status 是 uint16_t，会先整型提升成 int；与 unsigned 字面量直接比会触发
  // C4389（signed/unsigned mismatch），在 /W4 /WX 下直接失败。统一显式转 unsigned。
  QZ_CHECK_EQ(static_cast<unsigned>(decoded.status), 0x0102U);
  QZ_CHECK_EQ(decoded.seq, 0x01020304U);
  QZ_CHECK_EQ(decoded.method_id, 0x21678893U);
  QZ_CHECK_EQ(decoded.body_len, 0x00000011U);
}

// 帧头的每一类结构性错误都必须被拒绝，且 err 能区分原因（排障时 err 是唯一线索）。
void TestHeaderDecoderRejects() {
  FrameHeader good;
  good.seq = 1;
  good.body_len = 4;
  std::uint8_t base[kFrameHeaderSize];
  good.EncodeTo(base);

  struct Case {
    std::size_t offset;
    std::uint8_t value;
    const char* fragment;  // err 里必须出现的片段
  };
  const Case cases[] = {
      {0, 0x00, "magic"},       // magic 首字节错
      {4, 2, "version"},        // 版本 2 不支持
      {5, 0x00, "flags"},       // flags 一个都没置
      {5, 0x03, "flags"},       // 请求与响应同时置位
      {5, 0x11, "flags"},       // compressed 在 v1 不支持
      // 未定义位：0x40 / 0x80。放行它们等于"我不认识这个位但我当它不存在"，
      // 将来 v2 用这两位改语义时 v1 会静默误解析 —— 所以必须在帧边界就拒掉。
      {5, 0x41, "flags"},       // request + 未定义位 0x40
      {5, 0x82, "flags"},       // response + 未定义位 0x80
      {5, 0xFF, "flags"},       // 全置（同时踩压缩位与未定义位）
  };

  for (const Case& item : cases) {
    std::uint8_t raw[kFrameHeaderSize];
    std::memcpy(raw, base, sizeof(raw));
    raw[item.offset] = item.value;

    FrameHeader out;
    std::string err;
    QZ_CHECK(!FrameHeader::DecodeFrom(raw, &out, &err));
    QZ_CHECK(err.find(item.fragment) != std::string::npos);

    // err 传 nullptr 时不能崩（内部 SetError 会跳过写）。
    QZ_CHECK(!FrameHeader::DecodeFrom(raw, &out, nullptr));
  }

  // body_len 的两条边界：< 4（连长度前缀都放不下）与 > 硬上限。
  {
    std::uint8_t raw[kFrameHeaderSize];
    std::memcpy(raw, base, sizeof(raw));
    raw[16] = 0; raw[17] = 0; raw[18] = 0; raw[19] = 3;  // body_len = 3
    FrameHeader out;
    std::string err;
    QZ_CHECK(!FrameHeader::DecodeFrom(raw, &out, &err));
    QZ_CHECK(err.find("body_len") != std::string::npos);
  }
  {
    std::uint8_t raw[kFrameHeaderSize];
    std::memcpy(raw, base, sizeof(raw));
    // body_len = 64 MiB + 1：刚好越过硬上限。
    const std::uint32_t over = kAbsoluteMaxFrameSize + 1U;
    raw[16] = static_cast<std::uint8_t>((over >> 24) & 0xFF);
    raw[17] = static_cast<std::uint8_t>((over >> 16) & 0xFF);
    raw[18] = static_cast<std::uint8_t>((over >> 8) & 0xFF);
    raw[19] = static_cast<std::uint8_t>(over & 0xFF);
    FrameHeader out;
    std::string err;
    QZ_CHECK(!FrameHeader::DecodeFrom(raw, &out, &err));
    QZ_CHECK(err.find("absolute") != std::string::npos);
  }
  {
    // 恰好等于硬上限是合法的（边界值本身不该被拒）。
    std::uint8_t raw[kFrameHeaderSize];
    std::memcpy(raw, base, sizeof(raw));
    const std::uint32_t at_limit = kAbsoluteMaxFrameSize;
    raw[16] = static_cast<std::uint8_t>((at_limit >> 24) & 0xFF);
    raw[17] = static_cast<std::uint8_t>((at_limit >> 16) & 0xFF);
    raw[18] = static_cast<std::uint8_t>((at_limit >> 8) & 0xFF);
    raw[19] = static_cast<std::uint8_t>(at_limit & 0xFF);
    FrameHeader out;
    QZ_CHECK(FrameHeader::DecodeFrom(raw, &out, nullptr));
    QZ_CHECK_EQ(out.body_len, kAbsoluteMaxFrameSize);
  }

  // 空指针：两个入参都要能安全拒绝。
  {
    FrameHeader out;
    QZ_CHECK(!FrameHeader::DecodeFrom(nullptr, &out, nullptr));
    std::uint8_t raw[kFrameHeaderSize];
    std::memcpy(raw, base, sizeof(raw));
    QZ_CHECK(!FrameHeader::DecodeFrom(raw, nullptr, nullptr));
  }
}

// 解码失败时**不得**改动调用方的 out —— 否则"解析失败后继续用半成品帧"是最难查的一类
// 错误（字段一半新一半旧）。实现是在局部对象上校验通过后才整体赋值，这里把它钉住。
void TestHeaderDecodeIsTransactional() {
  FrameHeader out;
  out.seq = 0xDEADBEEFU;
  out.method_id = 0xFEEDFACEU;
  out.body_len = 12345;

  std::uint8_t raw[kFrameHeaderSize];
  std::memset(raw, 0, sizeof(raw));  // magic 全 0，必然失败

  QZ_CHECK(!FrameHeader::DecodeFrom(raw, &out, nullptr));
  QZ_CHECK_EQ(out.seq, 0xDEADBEEFU);
  QZ_CHECK_EQ(out.method_id, 0xFEEDFACEU);
  QZ_CHECK_EQ(out.body_len, 12345U);
}

// flags 合法性是 256 种组合上的纯函数，这里穷举而不是抽样。
//
// 三条规则，判定顺序即语义顺序：
//   1. 未定义位（0x40 / 0x80）**一律拒绝**；
//   2. 请求/响应**恰好**一个置位；
//   3. compressed(0x10) 不得置位（v1 无压缩算法）。
//
// 于是 8 位里：req/resp 2 位 -> 恰好一个 = 2 种；
// 已定义的自由位只有 3 个（keep_alive 0x04 / one_way 0x08 / draining 0x20）= 8 种；
// 合计 2 * 8 = **16** 种。
//
// ★ 这张表曾经是 64（放行 0x40/0x80）。改成 16 是 team-lead 的协议裁决，不是测试算错：
//   v1 没有"关键位/非关键位"的协商机制，放行未知位意味着"将来的 v2 用 0x40 改了封帧方式，
//   而 v1 解码器把它当不存在、继续按 v1 解析"，产出**静默的错误数据**。
//   详细论证见 protocol_const.h 里 IsValidFlags 的注释。
//   所以下面那两条"0x40 / 0x80 必须被拒"才是这次改动的目的，别在将来被"前向兼容"之类的
//   直觉改回去 —— 真要兼容，加版本协商。
void TestFlagsTruthTable() {
  int valid = 0;
  int valid_unknown_bit = 0;
  for (int value = 0; value <= 0xFF; ++value) {
    const std::uint8_t flags = static_cast<std::uint8_t>(value);
    const bool is_req = (flags & kFlagRequest) != 0;
    const bool is_resp = (flags & kFlagResponse) != 0;
    const bool compressed = (flags & kFlagCompressed) != 0;
    const bool unknown_bit = (flags & static_cast<std::uint8_t>(~kKnownFlagsMask)) != 0;
    const bool expected = !unknown_bit && (is_req != is_resp) && !compressed;
    if (IsValidFlags(flags)) ++valid;
    if (IsValidFlags(flags) && unknown_bit) ++valid_unknown_bit;
    QZ_EXPECT(IsValidFlags(flags) == expected);
  }
  QZ_CHECK_EQ(valid, 16);             // 2（req/resp 互斥）× 8（三个自由位）
  QZ_CHECK_EQ(valid_unknown_bit, 0);  // 带着未定义位"还能通过"的组合数必须是 0

  // 这次改动的目的：单独一个 0x40 或 0x80，配上合法的 req/resp，也必须被拒。
  QZ_CHECK(!IsValidFlags(static_cast<std::uint8_t>(kFlagRequest | 0x40)));
  QZ_CHECK(!IsValidFlags(static_cast<std::uint8_t>(kFlagResponse | 0x40)));
  QZ_CHECK(!IsValidFlags(static_cast<std::uint8_t>(kFlagRequest | 0x80)));
  QZ_CHECK(!IsValidFlags(static_cast<std::uint8_t>(kFlagResponse | 0x80)));
  QZ_CHECK(!IsValidFlags(static_cast<std::uint8_t>(kFlagRequest | kFlagDraining | 0x80)));
  QZ_CHECK(!IsValidFlags(0x40));
  QZ_CHECK(!IsValidFlags(0x80));
  QZ_CHECK(!IsValidFlags(0xFF));
  // 已知位全部置上（无 compressed）是合法的，说明拒绝的是"未知"而不是"位多"。
  QZ_CHECK(IsValidFlags(static_cast<std::uint8_t>(kFlagRequest | kFlagKeepAlive | kFlagOneWay |
                                                  kFlagDraining)));
  QZ_CHECK_EQ(static_cast<int>(kKnownFlagsMask), 0x3F);

  // 几个有代表性的组合，名字比数字好读。
  QZ_CHECK(IsValidFlags(kFlagRequest));
  QZ_CHECK(IsValidFlags(kFlagResponse));
  QZ_CHECK(IsValidFlags(static_cast<std::uint8_t>(kFlagRequest | kFlagOneWay)));
  QZ_CHECK(IsValidFlags(static_cast<std::uint8_t>(kFlagResponse | kFlagDraining)));
  QZ_CHECK(!IsValidFlags(0));
  QZ_CHECK(!IsValidFlags(static_cast<std::uint8_t>(kFlagRequest | kFlagResponse)));
  QZ_CHECK(!IsValidFlags(static_cast<std::uint8_t>(kFlagRequest | kFlagCompressed)));
}

// 标志位访问器：每个只认自己那一位，不能顺手把邻居也认了。
void TestFlagAccessors() {
  FrameHeader header;
  header.flags = 0;
  QZ_CHECK(!header.is_request());
  QZ_CHECK(!header.is_response());
  QZ_CHECK(!header.one_way());
  QZ_CHECK(!header.keep_alive());
  QZ_CHECK(!header.draining());

  header.flags = static_cast<std::uint8_t>(kFlagRequest | kFlagKeepAlive | kFlagDraining);
  QZ_CHECK(header.is_request());
  QZ_CHECK(!header.is_response());
  QZ_CHECK(header.keep_alive());
  QZ_CHECK(header.draining());
  QZ_CHECK(!header.one_way());

  header.flags = static_cast<std::uint8_t>(kFlagResponse | kFlagOneWay);
  QZ_CHECK(!header.is_request());
  QZ_CHECK(header.is_response());
  QZ_CHECK(header.one_way());
}

// ===========================================================================
// B. Metadata 编码
// ===========================================================================

// 空 metadata：编码 0 字节、size 为 0、empty() 为真。
// 这直接决定了空 metadata 帧的 body_len 恰好是 4，是最常见的线上形态。
void TestMetadataEmpty() {
  Metadata meta;
  QZ_CHECK(meta.empty());
  QZ_CHECK_EQ(meta.EncodedSize(), std::size_t{0});
  QZ_CHECK_EQ(EncodeMeta(meta), std::string());

  // Decode 一个空流：成功且仍为空。
  QZ_CHECK(DecodeMeta(std::string(), &meta));
  QZ_CHECK(meta.empty());
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{0});
}

// 单字段的**逐字节**编码。重点在两类易错：
//   * has_xxx 与值为 0 的区分（timeout=0 必须写出去）；
//   * 默认值省略（compression=0 / retry=0 / deadline=0 不写）。
void TestMetadataFieldBytes() {
  {
    Metadata meta;
    meta.SetTraceId("abc");  // tag(1,wt2)=0x0A + len 3 + "abc"
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("0a03616263"));
  }
  {
    Metadata meta;
    meta.SetTimeoutMs(0);  // 显式要求 0：与"未设置"不同，必须写 tag+0
    QZ_CHECK(!meta.empty());
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("1000"));

    Metadata back;
    QZ_CHECK(DecodeMeta(EncodeMeta(meta), &back));
    QZ_CHECK(back.has_timeout());
    QZ_CHECK_EQ(back.timeout_ms(), 0U);
  }
  {
    Metadata meta;
    meta.SetTimeoutMs(5000);  // varint(5000) = 88 27
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("108827"));
  }
  {
    Metadata meta;
    meta.SetIdempotent(false);  // 显式 false 也要写（只有"没设过"才省略）
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("1800"));
  }
  {
    Metadata meta;
    meta.SetIdempotent(true);
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("1801"));
  }
  {
    Metadata meta;
    meta.SetCompression(0);  // 默认值：省略
    QZ_CHECK(meta.empty());
    QZ_CHECK_EQ(EncodeMeta(meta), std::string());

    meta.SetCompression(1);
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("2001"));
  }
  {
    Metadata meta;
    meta.SetCaller("svc");  // tag(5,wt2)=0x2A + len 3 + "svc"
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("2a03737663"));
  }
  {
    Metadata meta;
    meta.SetRetryCount(0);  // 默认值：省略
    QZ_CHECK(meta.empty());
    meta.SetRetryCount(3);
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("3003"));
  }
  {
    Metadata meta;
    meta.SetDeadlineUnixMs(0);  // 默认值：省略
    QZ_CHECK(meta.empty());
    meta.SetDeadlineUnixMs(1714559696789ULL);
    // tag=0x38；varint(1714559696789) 的字节流较长，这里用往返验证值而不是硬编码
    // （硬编码 varint 抄错一位是最常见的"假失败"来源）。
    Metadata back;
    QZ_CHECK(DecodeMeta(EncodeMeta(meta), &back));
    QZ_CHECK_EQ(back.deadline_unix_ms(), 1714559696789ULL);
    QZ_CHECK(EncodeMeta(meta).compare(0, 1, "\x38") == 0);
  }
  {
    // 扩展键 key=100 → tag = (100<<3)|2 = 802 → varint A2 06，值 "x"
    Metadata meta;
    QZ_CHECK(meta.SetExtra(100, "x"));
    QZ_CHECK_EQ(Hex(EncodeMeta(meta)), std::string("a2060178"));
  }
}

// 编码必须按 key 升序，且 extras 也参与排序（它们 key >= 100，天然排在标准键之后）。
void TestMetadataKeyOrder() {
  Metadata meta;
  // 故意乱序设置，编码顺序不能跟着设置顺序走。
  QZ_CHECK(meta.SetExtra(101, "b"));
  meta.SetCaller("c");
  meta.SetRetryCount(1);
  meta.SetTraceId("t");
  QZ_CHECK(meta.SetExtra(100, "a"));
  meta.SetTimeoutMs(1);
  meta.SetDeadlineUnixMs(1);
  meta.SetIdempotent(true);
  meta.SetCompression(1);

  const std::string raw = EncodeMeta(meta);
  const std::vector<std::uint32_t> keys = TagKeys(raw);
  QZ_CHECK_EQ(keys.size(), std::size_t{9});
  if (keys.size() == 9) {
    QZ_CHECK_EQ(keys[0], 1U);    // trace_id
    QZ_CHECK_EQ(keys[1], 2U);    // timeout_ms
    QZ_CHECK_EQ(keys[2], 3U);    // idempotent
    QZ_CHECK_EQ(keys[3], 4U);    // compression
    QZ_CHECK_EQ(keys[4], 5U);    // caller
    QZ_CHECK_EQ(keys[5], 6U);    // retry_count
    QZ_CHECK_EQ(keys[6], 7U);    // deadline_unix_ms
    QZ_CHECK_EQ(keys[7], 100U);  // 扩展键按 map 序
    QZ_CHECK_EQ(keys[8], 101U);
  }

  // 同一份 metadata 编码两次必须逐字节相同（编码是纯函数，不含时间/地址等隐式状态）。
  QZ_CHECK_EQ(EncodeMeta(meta), raw);
}

// EncodedSize() 必须与真实 Encode 写出的字节数一致。
// 不一致的后果：Codec 用 EncodedSize 预分配/写长度前缀，整体偏移一位，整帧报废。
void TestMetadataEncodedSizeMatchesEncode() {
  std::vector<Metadata> samples;
  samples.push_back(Metadata());  // 空

  Metadata one;
  one.SetTraceId("abc");
  samples.push_back(one);

  Metadata with_zero;
  with_zero.SetTimeoutMs(0);
  with_zero.SetIdempotent(false);
  samples.push_back(with_zero);

  Metadata full;
  FillAllFields(&full);
  samples.push_back(full);

  Metadata wide;
  wide.SetTraceId(std::string(300, 'x'));   // 跨 varint 长度前缀的 1→2 字节边界
  wide.SetCaller(std::string(70000, 'y'));  // 再跨到 3 字节
  samples.push_back(wide);

  for (const Metadata& meta : samples) {
    const std::size_t declared = meta.EncodedSize();
    const std::string actual = EncodeMeta(meta);
    QZ_CHECK_EQ(declared, actual.size());
  }
}

// ===========================================================================
// C. 扩展键
// ===========================================================================

// key < kMetaUserExtBase(100) 是框架保留位，业务写不进去 ——
// 否则一个手滑的 SetExtra(2, ...) 就能覆盖 timeout_ms，变成"线上超时莫名其妙"。
void TestExtraKeyRange() {
  Metadata meta;
  QZ_CHECK(!meta.SetExtra(0, "x"));
  QZ_CHECK(!meta.SetExtra(1, "x"));
  QZ_CHECK(!meta.SetExtra(99, "x"));
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{0});

  QZ_CHECK(meta.SetExtra(100, "x"));
  QZ_CHECK(meta.SetExtra(101, "y"));
  QZ_CHECK(meta.SetExtra(0xFFFFFFFFU, "z"));  // 最大 key 也要能放进去
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{3});

  QZ_CHECK_EQ(meta.GetExtra(100), std::string("x"));
  QZ_CHECK_EQ(meta.GetExtra(999), std::string());        // 不存在 -> 空串
  QZ_CHECK_EQ(meta.GetExtra(102), std::string());

  // 空值与"不存在"都返回空串（API 如此），要区分就看 extras() 里有没有这个 key。
  QZ_CHECK(meta.SetExtra(102, ""));
  QZ_CHECK_EQ(meta.GetExtra(102), std::string());
  QZ_CHECK_EQ(meta.extras().count(102), std::size_t{1});
  QZ_CHECK_EQ(meta.extras().count(103), std::size_t{0});

  // 同名 key 覆盖：值变、数量不变。
  QZ_CHECK(meta.SetExtra(100, "replaced"));
  QZ_CHECK_EQ(meta.GetExtra(100), std::string("replaced"));
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{4});
}

// 回归用例：扩展键必须能"写出去再收回来"。
//
// 曾经的真实 bug（src/protocol/frame.cpp 的 Metadata::Decode）：Encode 侧正确地用
// WriteTag(key, 2) + WriteString 写扩展键，但 Decode 的 default 分支一律 SkipField，
// 于是 extras_ 永远是空的 —— SetExtra 成了"只写不读"。echo demo 完全读不到这个差异
// （它不碰扩展键），只有这条往返断言能发现。
void TestExtrasRoundTrip() {
  Metadata meta;
  meta.SetTraceId("trace-1");
  QZ_CHECK(meta.SetExtra(100, "ext-a"));
  QZ_CHECK(meta.SetExtra(101, ""));           // 空值也要能往返（presence 语义）
  QZ_CHECK(meta.SetExtra(200, std::string("bin\0ary", 7)));  // 含 \0，长度定界不是 C 串

  Metadata back;
  QZ_CHECK(DecodeMeta(EncodeMeta(meta), &back));
  QZ_CHECK_EQ(back.trace_id(), std::string("trace-1"));
  QZ_CHECK_EQ(back.GetExtra(100), std::string("ext-a"));
  QZ_CHECK_EQ(back.extras().count(101), std::size_t{1});
  QZ_CHECK_EQ(back.GetExtra(101), std::string());
  QZ_CHECK_EQ(back.GetExtra(200), std::string("bin\0ary", 7));
  QZ_CHECK_EQ(back.extras().size(), std::size_t{3});

  // 再编码一次必须与原始字节一致（往返是幂等的）。
  QZ_CHECK_EQ(EncodeMeta(back), EncodeMeta(meta));
}

// 重复 key：后出现的覆盖先出现的（protobuf 的 last-wins 语义）。
// 这条不写下来，将来"优化"成 first-wins 也没人会发现 —— 直到有人依赖覆盖来打补丁。
void TestExtraLastWins() {
  // tag(100,wt2) + "first"，再来一遍 tag(100,wt2) + "second"
  const std::string raw = Bytes("a206" "05" "6669727374" "a206" "06" "7365636f6e64");
  Metadata meta;
  QZ_CHECK(DecodeMeta(raw, &meta));
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{1});
  QZ_CHECK_EQ(meta.GetExtra(100), std::string("second"));
}

// 接收侧上限：最多 32 个扩展键，超出部分**静默丢弃**（不是断连、不是报错）。
// 理由见 frame.cpp 里 kMaxUserExtras 的注释：可选展示字段丢了无所谓，
// 但让攻击者用几十个字节就能拒绝一条合法连接是不可接受的。
void TestDecodeExtraCountLimit() {
  Writer writer;
  constexpr std::uint32_t kFirst = 200;
  constexpr std::uint32_t kCount = 40;
  for (std::uint32_t i = 0; i < kCount; ++i) {
    writer.WriteTag(kFirst + i, 2);
    writer.WriteString("v");
  }
  // 末尾再放一个标准字段，验证"丢扩展键"不影响后续解析位置。
  writer.WriteTag(2, 0);
  writer.WriteVarint32(4242);

  Metadata meta;
  QZ_CHECK(DecodeMeta(writer.data(), &meta));
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{32});
  // 保留的是**先到的** 32 个，后面的安静消失。
  QZ_CHECK_EQ(meta.GetExtra(kFirst), std::string("v"));
  QZ_CHECK_EQ(meta.GetExtra(kFirst + 31), std::string("v"));
  QZ_CHECK_EQ(meta.GetExtra(kFirst + 32), std::string());
  QZ_CHECK_EQ(meta.GetExtra(kFirst + 39), std::string());
  // 关键：超限之后流的位置仍然正确，后面的 timeout 正常读出来。
  QZ_CHECK(meta.has_timeout());
  QZ_CHECK_EQ(meta.timeout_ms(), 4242U);
}

// 单值长度上限 1024 字节；超长的那个丢掉，后面的字段照常解析。
void TestDecodeExtraValueLenLimit() {
  {
    Writer writer;
    writer.WriteTag(100, 2);
    writer.WriteString(std::string(1024, 'x'));  // 恰好等于上限：保留
    Metadata meta;
    QZ_CHECK(DecodeMeta(writer.data(), &meta));
    QZ_CHECK_EQ(meta.extras().size(), std::size_t{1});
    QZ_CHECK_EQ(meta.GetExtra(100).size(), std::size_t{1024});
  }
  {
    Writer writer;
    writer.WriteTag(100, 2);
    writer.WriteString(std::string(1025, 'x'));  // 超一字节：丢弃
    writer.WriteTag(101, 2);
    writer.WriteString("ok");                    // 后继字段必须还在
    writer.WriteTag(2, 0);
    writer.WriteVarint32(7);

    Metadata meta;
    QZ_CHECK(DecodeMeta(writer.data(), &meta));
    QZ_CHECK_EQ(meta.extras().size(), std::size_t{1});
    QZ_CHECK_EQ(meta.GetExtra(100), std::string());
    QZ_CHECK_EQ(meta.GetExtra(101), std::string("ok"));
    QZ_CHECK_EQ(meta.timeout_ms(), 7U);
  }
}

// ===========================================================================
// D. Decode 的前向兼容与健壮性
// ===========================================================================

// 未知的**保留键**（< 100，例如将来新增的框架字段）必须跳过而不是报错。
// 这是 doc 03 §6.1 承诺的向后兼容：老版本遇到新版本加的字段要继续服务。
void TestDecodeSkipsUnknownReservedKey() {
  // tag(50, wt0) = (50<<3)|0 = 400 -> 90 03；值 7；然后跟一个 timeout=100
  const std::string raw = Bytes("9003" "07" "1064");
  Metadata meta;
  QZ_CHECK(DecodeMeta(raw, &meta));
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{0});  // 保留键不进 extras
  QZ_CHECK(meta.has_timeout());
  QZ_CHECK_EQ(meta.timeout_ms(), 100U);
}

// 已知 key 但 wire_type 不匹配：跳过而不是判定协议错误。
// 理由：未来版本可能给同一个 key 换更紧凑的编码，跳过比断连更利于演进。
void TestDecodeSkipsWireTypeMismatch() {
  // timeout_ms(key=2) 用 wire_type=2 写成字符串 "abc"，后面跟 retry_count=1
  const std::string raw = Bytes("1203" "616263" "3001");
  Metadata meta;
  QZ_CHECK(DecodeMeta(raw, &meta));
  QZ_CHECK(!meta.has_timeout());          // 没被污染
  QZ_CHECK_EQ(meta.timeout_ms(), 0U);
  QZ_CHECK_EQ(meta.retry_count(), 1U);    // 后续字段正常
}

// 扩展键的 wire_type 不是 2：同样跳过，且不能进 extras（否则 GetExtra 会读到
// 一个本该是 varint 的字节流的字符串投影）。
void TestDecodeExtraWrongWireType() {
  // tag(100, wt0) = (100<<3)|0 = 800 -> a0 06；值 1
  const std::string raw = Bytes("a006" "01");
  Metadata meta;
  QZ_CHECK(DecodeMeta(raw, &meta));
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{0});
  QZ_CHECK(meta.extras().empty());
}

// 截断的 metadata：tag 声称有 N 字节但流已经到底 —— 必须失败（这是真损坏）。
void TestDecodeTruncated() {
  {
    // tag(1,wt2) + len=5，但只给了 2 字节
    const std::string raw = Bytes("0a05" "6162");
    Metadata meta;
    QZ_CHECK(!DecodeMeta(raw, &meta));
  }
  {
    // tag(1,wt2) 后面什么都没有
    const std::string raw = Bytes("0a");
    Metadata meta;
    QZ_CHECK(!DecodeMeta(raw, &meta));
  }
  {
    // tag(2,wt0) 缺 varint
    const std::string raw = Bytes("10");
    Metadata meta;
    QZ_CHECK(!DecodeMeta(raw, &meta));
  }
}

// Decode 开头会 Clear()：解析一份新 metadata 不能残留上一份的字段。
// 不复用 Metadata 对象时看不出这个差异，而 server 里恰恰是复用的（每帧复用）。
void TestDecodeClearsPreviousState() {
  Metadata meta;
  meta.SetTraceId("stale");
  meta.SetTimeoutMs(999);
  QZ_CHECK(meta.SetExtra(100, "stale-ext"));

  QZ_CHECK(DecodeMeta(Bytes("3001"), &meta));  // 只有 retry_count=1
  QZ_CHECK_EQ(meta.trace_id(), std::string());
  QZ_CHECK(!meta.has_timeout());
  QZ_CHECK_EQ(meta.timeout_ms(), 0U);
  QZ_CHECK_EQ(meta.extras().size(), std::size_t{0});
  QZ_CHECK_EQ(meta.retry_count(), 1U);
}

// ===========================================================================
// E. Frame 的装配
// ===========================================================================

// Reset 产出一个"空请求帧"：body_len 只有 4（metadata 长度前缀），metadata/payload 都空。
void TestFrameReset() {
  Frame frame;
  frame.Reset(7, 0x11223344U, true);

  QZ_CHECK_EQ(frame.header.magic, kMagic);
  QZ_CHECK_EQ(frame.header.version, kVersion);
  QZ_CHECK_EQ(frame.header.flags, static_cast<std::uint8_t>(kFlagRequest));
  QZ_CHECK_EQ(static_cast<unsigned>(frame.header.status), 0U);
  QZ_CHECK_EQ(frame.header.seq, 7U);
  QZ_CHECK_EQ(frame.header.method_id, 0x11223344U);
  QZ_CHECK_EQ(frame.header.body_len, 4U);
  QZ_CHECK(frame.metadata.empty());
  QZ_CHECK(frame.payload.empty());
  QZ_CHECK_EQ(frame.EncodedSize(), kFrameHeaderSize + 4);

  // EncodedSize 以 metadata/payload 的**实际内容**为准，而不是 header.body_len。
  // 二者会短暂不一致：Reset 之后直接改 payload、还没过 Codec::Encode（它才重算 body_len）。
  // 这条刻意钉住：如果 EncodedSize 改成读 header.body_len，这里立刻变红。
  frame.payload = "abc";
  QZ_CHECK_EQ(frame.header.body_len, 4U);       // 陈旧值，等 Codec::Encode 刷新
  QZ_CHECK_EQ(frame.EncodedSize(), kFrameHeaderSize + 4 + 3);

  frame.metadata.SetTraceId("ab12");
  // trace_id "ab12" 编码成 0a 04 'a' 'b' '1' '2' = 6 字节
  QZ_CHECK_EQ(frame.EncodedSize(), kFrameHeaderSize + 4 + 6 + 3);
}

// 响应帧：flags 换成 response、status 生效，其余同上。
void TestFrameResetResponse() {
  Frame frame;
  frame.ResetResponse(9, 0x21678893U, 0x0007U);

  QZ_CHECK(frame.header.is_response());
  QZ_CHECK(!frame.header.is_request());
  QZ_CHECK_EQ(static_cast<unsigned>(frame.header.status), 0x0007U);
  QZ_CHECK_EQ(frame.header.seq, 9U);
  QZ_CHECK_EQ(frame.header.method_id, 0x21678893U);

  // Reset 会先 Clear（把上一帧的 payload/metadata 清掉），复用 Frame 时不能串帧。
  frame.payload = "leftover";
  frame.metadata.SetTraceId("leftover");
  frame.ResetResponse(10, 1, 0);
  QZ_CHECK(frame.payload.empty());
  QZ_CHECK(frame.metadata.empty());
  QZ_CHECK_EQ(frame.header.seq, 10U);
}

// Clear 之后回到**默认构造**的状态：注意 flags 是 kFlagRequest（FrameHeader 的默认值），
// 不是 0 —— 0 是非法组合（IsValidFlags(0) == false）。这条容易被"清空就是全 0"的直觉写错。
void TestFrameClear() {
  Frame frame;
  frame.Reset(3, 4, false);
  frame.payload = "x";
  frame.metadata.SetCaller("c");
  frame.Clear();

  QZ_CHECK_EQ(frame.header.magic, kMagic);
  QZ_CHECK_EQ(frame.header.flags, static_cast<std::uint8_t>(kFlagRequest));
  QZ_CHECK_EQ(frame.header.seq, 0U);
  QZ_CHECK_EQ(frame.header.body_len, 0U);
  QZ_CHECK(frame.payload.empty());
  QZ_CHECK(frame.metadata.empty());
  QZ_CHECK_EQ(frame.EncodedSize(), kFrameHeaderSize + 4);
}

}  // namespace

int main() {
  // A. 帧头
  TestHeaderByteLayout();
  TestHeaderDecoderRejects();
  TestHeaderDecodeIsTransactional();
  TestFlagsTruthTable();
  TestFlagAccessors();

  // B. metadata 编码
  TestMetadataEmpty();
  TestMetadataFieldBytes();
  TestMetadataKeyOrder();
  TestMetadataEncodedSizeMatchesEncode();

  // C. 扩展键
  TestExtraKeyRange();
  TestExtrasRoundTrip();
  TestExtraLastWins();

  // D. 解码的前向兼容与健壮性
  TestDecodeSkipsUnknownReservedKey();
  TestDecodeSkipsWireTypeMismatch();
  TestDecodeExtraWrongWireType();
  TestDecodeExtraCountLimit();
  TestDecodeExtraValueLenLimit();
  TestDecodeTruncated();
  TestDecodeClearsPreviousState();

  // E. Frame 装配
  TestFrameReset();
  TestFrameResetResponse();
  TestFrameClear();

  return qztest::Summary("test_frame_metadata");
}
