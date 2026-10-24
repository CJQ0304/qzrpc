// 轻舟 RPC (Qingzhou RPC)
// src/net/platform.h
//
// 职责：**唯一的平台隔离层**。本文件是全部公共头文件中唯一允许出现
//       <winsock2.h> / <sys/socket.h> 等平台头的地方。
//
// 使用规则（强约束）：
//   1) 只有 src/net/*.cpp 与 src/net/event_loop.cpp 这类实现文件可以 include 本文件；
//   2) src 下所有对外头文件**一律不得** include 本文件，也不得出现 SOCKET/fd_set 等平台类型；
//   3) 对外接口统一用 `int fd` 表达句柄。Windows 的 SOCKET 在 64 位下是 UINT_PTR，
//      但真实取值始终落在 32 位 int 可表示范围内（Windows 句柄值不连续但很小），
//      因此在边界处用 static_cast<int> 转换，并在转换前断言 <= INT_MAX。
//
// 线程安全：PlatformInit::EnsureInit 线程安全且幂等。
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// 顺序不可调整：winsock2.h 必须先于 windows.h（否则会拉进 winsock.h 造成冲突）；
// mstcpip.h 依赖 windows.h 提供的 GUID 宏，必须排在最后。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cstdint>

namespace qzrpc {
namespace net {

#ifdef _WIN32
// Windows 下 socket 句柄类型。
using SocketHandle = SOCKET;
// 无效句柄值。
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
// POSIX 下 socket 句柄类型。
using SocketHandle = int;
// 无效句柄值。
constexpr SocketHandle kInvalidSocket = -1;
#endif

// 进程级平台初始化（Windows 下执行 WSAStartup、POSIX 下忽略 SIGPIPE）。
// 幂等：多次调用只有第一次真正生效。所有需要创建 socket 的入口都必须先调用它。
class PlatformInit {
 public:
  static void EnsureInit();
};

}  // namespace net
}  // namespace qzrpc
