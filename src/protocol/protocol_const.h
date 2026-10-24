// 轻舟 RPC (Qingzhou RPC)
// src/protocol/protocol_const.h
//
// 职责：线协议的**全部常量**集中地。任何魔数都不得散落在其它文件里。
//
// 为什么不用 enum class 装魔数：这些值要参与位运算与算术，用 constexpr 更好用；
// 而"状态码"已经在 common/status.h 里做成强类型枚举，此处只做引用。
//
// 线程安全：全部为编译期常量与无状态 inline 函数，天然线程安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/string_util.h"

namespace qzrpc {
namespace protocol {

// 帧魔数 'Q''Z''R''P' = 0x51 0x5A 0x52 0x50。
// 大端读出为 0x515A5250。抓包时肉眼可见 "QZRP" 四个字符。
constexpr std::uint32_t kMagic = 0x515A5250U;

// 协议版本。任何不兼容的线格式变化都必须递增它。
constexpr std::uint8_t kVersion = 1;

// 定长帧头字节数。
constexpr std::size_t kFrameHeaderSize = 20;

// metadata 长度前缀字节数（body 开头的 4 字节）。
constexpr std::size_t kMetadataLengthSize = 4;

// 单帧 body（metadata + payload）默认上限：16 MiB。
// 超过即视为恶意包或配置错误，直接断开连接（见 doc 03 防护策略）。
constexpr std::uint32_t kDefaultMaxFrameSize = 16U * 1024U * 1024U;

// 单帧 body 硬上限（编译期不可突破），防止配置写成超大值导致 OOM。
constexpr std::uint32_t kAbsoluteMaxFrameSize = 64U * 1024U * 1024U;

// 帧标志位。
enum FrameFlags : std::uint8_t {
  kFlagNone = 0x00,
  kFlagRequest = 0x01,     // 请求帧
  kFlagResponse = 0x02,    // 响应帧（与 kFlagRequest 互斥）
  kFlagKeepAlive = 0x04,   // 请求方希望复用连接（默认行为，置位表示显式声明）
  kFlagOneWay = 0x08,      // 单向调用：服务端不回包
  kFlagCompressed = 0x10,  // payload 已压缩（v1 不实现任何压缩算法，置位即视为协议错误）
  kFlagDraining = 0x20,    // 服务端正在优雅停机，本帧是对端的最后一个请求
};

// v1 已定义的标志位全集。0x40 / 0x80 留空，**并且必须被拒绝**（见 IsValidFlags 的注释）。
constexpr std::uint8_t kKnownFlagsMask = 0x3F;

// 判断标志组合是否合法。
//
// 三条规则，顺序有意：先拒未知位，再判互斥，最后拒压缩。
//   1. 任何未定义位（0x40 / 0x80）一律拒绝；
//   2. 请求与响应必须**恰好**一个置位；
//   3. compressed 在 v1 不支持（不实现任何压缩算法），置位即协议错误。
//
// ★ 为什么"未知位"是拒绝而不是忽略（这与很多协议文档里的"忽略未知位"相反）：
//   v1 **没有任何关键位/非关键位的协商机制**。如果将来的 v2 用 0x40 表示
//   "payload 换了一种封帧方式"或"body_len 语义变了"，而 v1 解码器放行它、
//   再按 v1 的规则逐字节解析，产出的就是**静默的错误数据**——业务拿到一堆
//   看起来合法、实际上错位的字段，比连不上难查一个数量级。
//   在线上协议里，"我不认识这个位但我当它不存在"是 corruption 的来源，不是兼容性。
//
//   帧边界上**显式失败**的代价是可控的：对端立刻收到一个明确错误，可以重连、
//   可以降级、可以打点报警；而静默误解析没有任何告警面。
//
//   将来真要前向兼容，正确做法是加**版本协商**（在握手里声明双方能力），
//   而不是在每帧上偷偷放行未知位。见 docs/03-wire-protocol.md 的版本策略。
inline bool IsValidFlags(std::uint8_t flags) noexcept {
  if ((flags & static_cast<std::uint8_t>(~kKnownFlagsMask)) != 0) return false;  // 未定义位
  const bool is_req = (flags & kFlagRequest) != 0;
  const bool is_resp = (flags & kFlagResponse) != 0;
  if (is_req == is_resp) return false;                    // 必须恰好一个
  if ((flags & kFlagCompressed) != 0) return false;        // v1 不支持
  return true;
}

// metadata 标准键（wire_type 见注释）。0 保留不用；100 以上留给用户扩展。
enum MetaKey : std::uint32_t {
  kMetaTraceId = 1,         // wire_type=2，string，W3C traceparent 或自定义 id
  kMetaTimeoutMs = 2,       // wire_type=0，varint，服务端业务超时（毫秒）
  kMetaIdempotent = 3,      // wire_type=0，varint，0/1，标识该调用可安全重试
  kMetaCompression = 4,     // wire_type=0，varint，0=无压缩（v1 唯一合法值）
  kMetaCaller = 5,          // wire_type=2，string，调用方服务名，用于链路排查
  kMetaRetryCount = 6,      // wire_type=0，varint，本帧是第几次重试（从 0 开始）
  kMetaDeadlineUnixMs = 7,  // wire_type=0，varint，绝对 deadline（Unix 毫秒）
  kMetaUserExtBase = 100,   // 用户扩展键从此开始
};

// 默认监听端口（example 与 registry 使用）。
constexpr std::uint16_t kDefaultPort = 16161;
constexpr std::uint16_t kDefaultRegistryPort = 16162;

// ---- method_id 生成规则（**codegen 必须与之一致**） ----
//
// method_id = (FNV-1a 64(full_name) & 0xFFFFFFFF)
//   full_name = "<package>.<Service>.<Method>"，全部使用 IDL 中的原始大小写，
//   IDL 未声明 package 时 full_name = "<Service>.<Method>"（不带前导点）。
//
// 一致性要求：
//   * codegen（Python）必须实现完全相同的 FNV-1a 64（offset basis 0xcbf29ce484222325，
//     prime 0x100000001b3），并取低 32 位写成生成代码里的 constexpr 常量；
//   * 运行时的 ServiceRegistry 用本函数复核，二者不等即为 codegen bug，启动时报 fatal。

// 计算 method_id。
// 参数：full_name - "package.Service.Method" 形式的 C 字符串（非空）。
inline std::uint32_t MethodIdOfName(const char* full_name) noexcept {
  return static_cast<std::uint32_t>(StringUtil::Fnv1a64(full_name) & 0xFFFFFFFFULL);
}

// std::string 重载。
inline std::uint32_t MethodIdOfName(const std::string& full_name) noexcept {
  return static_cast<std::uint32_t>(StringUtil::Fnv1a64(full_name) & 0xFFFFFFFFULL);
}

}  // namespace protocol
}  // namespace qzrpc
