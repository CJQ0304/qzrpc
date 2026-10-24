// 轻舟 RPC (Qingzhou RPC)
// src/common/endian.h
//
// 职责：提供与主机字节序无关的定长整数读写工具。
// 设计：线协议（doc 03）约定所有定长字段一律大端（network byte order）。
//       本文件是唯一处理字节序的地方，其余模块不得自行编写 <<24 之类的代码。
//
// 线程安全：全部为无状态 inline 函数，天然线程安全。
// 依赖：<cstdint> / <cstring> / <cstdlib>。最后一个是必需的：
//       MSVC 的 _byteswap_* 内建函数声明在 <stdlib.h> 里，不包含它会在
//       "第一个 include 本文件"的 TU 中报 C3861（找不到标识符）——
//       其它 TU 能编过只是因为恰好先包含了 <cstdlib>，属于顺序依赖的假象。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace qzrpc {

// 判断当前主机是否为小端。
// 返回：true 表示小端（x86/x64/ARM 小端模式）。
inline bool IsLittleEndian() noexcept {
  const std::uint16_t probe = 0x0001U;
  return *reinterpret_cast<const std::uint8_t*>(&probe) == 0x01U;
}

namespace detail {

inline std::uint16_t Bswap16(std::uint16_t v) noexcept {
#if defined(_MSC_VER)
  return _byteswap_ushort(v);
#else
  return static_cast<std::uint16_t>((v >> 8) | (v << 8));
#endif
}

inline std::uint32_t Bswap32(std::uint32_t v) noexcept {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

inline std::uint64_t Bswap64(std::uint64_t v) noexcept {
#if defined(_MSC_VER)
  return _byteswap_uint64(v);
#else
  return __builtin_bswap64(v);
#endif
}

}  // namespace detail

// 主机序 <-> 大端序 转换（对同一函数：h2be 与 be2h 的位运算完全相同）。
inline std::uint16_t HostToBig16(std::uint16_t v) noexcept {
  return IsLittleEndian() ? detail::Bswap16(v) : v;
}
inline std::uint16_t BigToHost16(std::uint16_t v) noexcept { return HostToBig16(v); }

inline std::uint32_t HostToBig32(std::uint32_t v) noexcept {
  return IsLittleEndian() ? detail::Bswap32(v) : v;
}
inline std::uint32_t BigToHost32(std::uint32_t v) noexcept { return HostToBig32(v); }

inline std::uint64_t HostToBig64(std::uint64_t v) noexcept {
  return IsLittleEndian() ? detail::Bswap64(v) : v;
}
inline std::uint64_t BigToHost64(std::uint64_t v) noexcept { return HostToBig64(v); }

// ---------------------------------------------------------------------------
// 未对齐安全读写：一律用 memcpy，禁止 reinterpret_cast 到 uint32_t*。
// ---------------------------------------------------------------------------

// 从 dst 起 2 字节按大端解出 uint16。
// 前置条件：p 至少可读 2 字节。
inline std::uint16_t LoadBig16(const void* p) noexcept {
  std::uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return BigToHost16(v);
}

// 从 p 起 4 字节按大端解出 uint32。
// 前置条件：p 至少可读 4 字节。
inline std::uint32_t LoadBig32(const void* p) noexcept {
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return BigToHost32(v);
}

// 从 p 起 8 字节按大端解出 uint64。
// 前置条件：p 至少可读 8 字节。
inline std::uint64_t LoadBig64(const void* p) noexcept {
  std::uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return BigToHost64(v);
}

// 把 v 以大端写入 p 起的 2 字节。
// 前置条件：p 至少可写 2 字节。
inline void StoreBig16(void* p, std::uint16_t v) noexcept {
  const std::uint16_t be = HostToBig16(v);
  std::memcpy(p, &be, sizeof(be));
}

// 把 v 以大端写入 p 起的 4 字节。
inline void StoreBig32(void* p, std::uint32_t v) noexcept {
  const std::uint32_t be = HostToBig32(v);
  std::memcpy(p, &be, sizeof(be));
}

// 把 v 以大端写入 p 起的 8 字节。
inline void StoreBig64(void* p, std::uint64_t v) noexcept {
  const std::uint64_t be = HostToBig64(v);
  std::memcpy(p, &be, sizeof(be));
}

}  // namespace qzrpc
