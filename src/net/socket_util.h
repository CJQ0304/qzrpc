// 轻舟 RPC (Qingzhou RPC)
// src/net/socket_util.h
//
// 职责：socket 相关的全部自由函数。这里是对外唯一暴露"网络系统调用"语义的地方，
//       上层（Acceptor/Connector/Connection/rpc）不得直接调 ::socket/::connect。
//
// 平台差异：本头文件零平台依赖，所有实现差异在 socket_util.cpp（包含 net/platform.h）。
//
// 线程安全：除特别说明外，每个函数只操作传入的 fd，函数本身无共享状态，可并发调用。
//           同一 fd 的并发操作由调用方保证互斥。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/noncopyable.h"

namespace qzrpc {
namespace net {

// 一个网络端点（IP 字面量 + 端口）。不含域名，域名解析请用 ResolveHost。
struct Endpoint {
  std::string ip;
  std::uint16_t port = 0;

  // "1.2.3.4:8080" 形式。IPv6 会输出为 "[::1]:8080"。
  std::string ToString() const;
};

// socket 工具集合（纯静态）。
class SocketUtil : private StaticOnly {
 public:
  // ---- 创建与配置 ----

  // 创建一个 TCP socket（已执行 PlatformInit::EnsureInit）。
  // 返回：合法 fd；失败返回 -1，LastError() 给出原因。
  static int CreateTcp();

  // 设置非阻塞。返回是否成功。
  static bool SetNonBlocking(int fd, bool on);

  // 设置 TCP_NODELAY（关闭 Nagle）。RPC 场景几乎总是需要，可显著降低小包延迟。
  static bool SetNoDelay(int fd, bool on);

  // 设置 SO_REUSEADDR。服务端监听必须开启，否则重启会遇到 TIME_WAIT 占用端口。
  static bool SetReuseAddr(int fd, bool on);

  // 设置 SO_KEEPALIVE 并配置探测参数（空闲时间/间隔/次数）。
  // 参数：idle_s - 开始探测前的空闲秒数。
  static bool SetKeepAlive(int fd, bool on, int idle_s = 60);

  // 设置 SO_SNDBUF / SO_RCVBUF 建议值（实际值由内核决定）。
  static bool SetSendBufferSize(int fd, int bytes);
  static bool SetRecvBufferSize(int fd, int bytes);

  // ---- 地址解析与转换 ----

  // 域名/IP 解析为候选端点列表（内部 getaddrinfo，支持 IPv4 与 IPv6）。
  // 参数：host - 域名或 IP 字面量；port - 端口；out - 输出（会被清空后填充）。
  // 返回：true 表示至少解析出一个端点。
  static bool ResolveHost(const std::string& host, std::uint16_t port,
                          std::vector<Endpoint>* out);

  // 把已连接 fd 的对端地址填入 out。返回是否成功。
  static bool GetPeerAddress(int fd, Endpoint* out);
  // 把已绑定 fd 的本地地址填入 out。返回是否成功。
  static bool GetLocalAddress(int fd, Endpoint* out);

  // ---- 监听与连接 ----

  // 创建监听 socket 并 bind + listen。
  // 参数：ip - 绑定地址，"0.0.0.0" 表示全部网卡；port - 端口，0 表示由系统分配；
  //       backlog - 已完成连接队列长度（Windows 上映射到 listen 的 backlog）。
  // 返回：非阻塞监听 fd；失败返回 -1，err 写入原因。
  static int Listen(const std::string& ip, std::uint16_t port, int backlog,
                    std::string* err);

  // 同步带超时连接（内部：非阻塞 connect + poll 等可写 + 检查 SO_ERROR）。
  // 参数：ep - 目标端点；connect_timeout_ms - 超时毫秒。
  // 返回：已连接且已设为非阻塞的 fd；失败返回 -1，err 写入原因。
  // 约束：会阻塞调用线程，**只能在客户端 IO 线程或连接预热线程调用**，不得在 EventLoop 回调里调用。
  static int ConnectWithTimeout(const Endpoint& ep, int connect_timeout_ms, std::string* err);

  // 从监听 fd 接受一个连接。
  // 参数：listen_fd - 监听 fd；peer - 输出对端地址（可为 nullptr）。
  // 返回：已连接且已设为非阻塞的 fd；失败返回 -1（EAGAIN/WOULDBLOCK 属于"暂时没有新连接"，
  //       调用方应通过 WouldBlock() 判断而不是当成致命错误）。
  static int Accept(int listen_fd, Endpoint* peer);

  // ---- 读写 ----

  // 读。返回值语义与 POSIX recv 一致：>0 读到的字节数；0 表示对端有序关闭；-1 表示错误。
  static long Recv(int fd, void* buf, std::size_t len);

  // 写（发送部分数据）。返回值语义与 POSIX send 一致。
  static long Send(int fd, const void* buf, std::size_t len);

  // 半关闭写方向（发 FIN），用于优雅关闭。
  static bool ShutdownWrite(int fd);

  // 关闭句柄。对 -1 是空操作。
  static void Close(int fd);

  // ---- 错误 ----

  // 当前线程最近一次 socket 调用的错误码（Windows 用 WSAGetLastError，POSIX 用 errno）。
  static int LastError() noexcept;

  // 错误码转字符串。
  static std::string ErrorString(int err);

  // 判断错误码是否属于"暂时无数据"（EAGAIN/EWOULDBLOCK/EINTR）。
  static bool WouldBlock(int err) noexcept;

  // 判断错误码是否属于"连接被拒绝/对端不可达"（可重试类错误）。
  static bool IsRetryableConnectError(int err) noexcept;
};

}  // namespace net
}  // namespace qzrpc
