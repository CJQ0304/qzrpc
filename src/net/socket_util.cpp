// 轻舟 RPC (Qingzhou RPC)
// src/net/socket_util.cpp
//
// 职责：全部 socket 系统调用的唯一封装处。平台差异全部收敛在本文件。
//
// 约定：
//   * 对外只暴露 `int fd`，内部转成 SocketHandle（Windows 是 SOCKET）。
//   * 所有返回 fd 的函数**都已经把 fd 设为非阻塞**（Accept/ConnectWithTimeout/Listen），
//     避免上层忘掉一步就退化成阻塞 IO。
//   * 所有失败路径都必须把 err 填成人类可读文本，不允许返回 -1 却留空 err。

#include "net/socket_util.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "net/platform.h"

namespace qzrpc {
namespace net {

namespace {

#ifdef _WIN32
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

// 把 "ip:port" 的端口部分格式化成字符串（getaddrinfo 需要）。
void AppendPortString(std::uint16_t port, char* buf, std::size_t len) {
  std::snprintf(buf, len, "%u", static_cast<unsigned>(port));
}

// 由 IP 字面量填充 sockaddr。空串视为 "0.0.0.0"（全部网卡）。
// 返回 false 表示 ip 不是合法的 IPv4/IPv6 字面量。
bool FillSockAddr(const std::string& ip, std::uint16_t port, sockaddr_storage* out,
                  SockLen* out_len) {
  std::memset(out, 0, sizeof(*out));
  if (ip.find(':') != std::string::npos) {
    sockaddr_in6* addr6 = reinterpret_cast<sockaddr_in6*>(out);
    addr6->sin6_family = static_cast<unsigned short>(AF_INET6);
    addr6->sin6_port = ::htons(port);
    if (::inet_pton(AF_INET6, ip.c_str(), &addr6->sin6_addr) != 1) return false;
    *out_len = static_cast<SockLen>(sizeof(sockaddr_in6));
    return true;
  }
  const std::string text = ip.empty() ? std::string("0.0.0.0") : ip;
  sockaddr_in* addr4 = reinterpret_cast<sockaddr_in*>(out);
  addr4->sin_family = static_cast<unsigned short>(AF_INET);
  addr4->sin_port = ::htons(port);
  if (::inet_pton(AF_INET, text.c_str(), &addr4->sin_addr) != 1) return false;
  *out_len = static_cast<SockLen>(sizeof(sockaddr_in));
  return true;
}

// 把 sockaddr 转成 Endpoint（IP 字面量形式）。
bool SockAddrToEndpoint(const sockaddr* sa, Endpoint* out) {
  if (sa == nullptr || out == nullptr) return false;
  char text[64];
  std::memset(text, 0, sizeof(text));
  if (sa->sa_family == AF_INET) {
    const sockaddr_in* addr4 = reinterpret_cast<const sockaddr_in*>(sa);
    if (::inet_ntop(AF_INET, &addr4->sin_addr, text, static_cast<SockLen>(sizeof(text))) ==
        nullptr) {
      return false;
    }
    out->ip.assign(text);
    out->port = ::ntohs(addr4->sin_port);
    return true;
  }
  if (sa->sa_family == AF_INET6) {
    const sockaddr_in6* addr6 = reinterpret_cast<const sockaddr_in6*>(sa);
    if (::inet_ntop(AF_INET6, &addr6->sin6_addr, text, static_cast<SockLen>(sizeof(text))) ==
        nullptr) {
      return false;
    }
    out->ip.assign(text);
    out->port = ::ntohs(addr6->sin6_port);
    return true;
  }
  return false;
}

// 把主机序 fd 转回平台句柄类型。
SocketHandle ToHandle(int fd) {
  return static_cast<SocketHandle>(fd);
}

// 从一个刚创建的 socket 取出 int 形式的 fd；失败返回 -1。
int FromHandle(SocketHandle s) {
  if (s == kInvalidSocket) return -1;
  return static_cast<int>(s);
}

// 等待 fd 可写（用于非阻塞 connect 的完成判定）。
// 返回：>0 就绪（可写**或**出错，真正结果由调用方用 getsockopt(SO_ERROR) 判定）；
//       0 超时；<0 出错。
//
// ⚠️ Windows 上这里用 select + exceptfds，而不是 WSAPoll(POLLWRNORM)：
//   Windows 的 select 会把"连接失败"的 socket 放进 **exceptfds**，这是 connect 完成判定
//   的官方做法；而 WSAPoll 对失败连接的事件上报并不可靠（只给 POLLERR、不给 POLLWRNORM），
//   我们不想把"连接到底成没成"押在它的实现细节上。
//
// 实测记录（避免后人重复踩）：本机网络栈对**未监听端口**不返回 RST，而是静默丢弃 SYN ——
// 用裸 winsock 探针确认过：`connect` 返回 WSAEWOULDBLOCK 后 select 一秒内无任何事件、
// `SO_ERROR` 仍为 0。所以"连一个死实例要花满 connect_timeout"在这个环境里是**必然**的，
// 不是这里的实现缺陷。端点冷却把这份代价限制为"每个 endpoint 每 30s 最多一次"，
// 这才是我们要的答案（而不是把 connect_timeout 调小，那会误杀慢启动的实例）。
int WaitWritable(int fd, int timeout_ms) {
#ifdef _WIN32
  fd_set write_fds;
  fd_set except_fds;
  FD_ZERO(&write_fds);
  FD_ZERO(&except_fds);
  FD_SET(ToHandle(fd), &write_fds);
  FD_SET(ToHandle(fd), &except_fds);
  timeval tv;
  tv.tv_sec = static_cast<long>(timeout_ms / 1000);
  tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
  // 第一个参数在 Windows 上被忽略（传 0 即可）。
  const int rc = ::select(0, nullptr, &write_fds, &except_fds, &tv);
  if (rc < 0) return -1;
  if (rc == 0) return 0;
  return 1;  // 可写或出异常：两者都意味着"connect 已有结论"，交给 SO_ERROR 判断
#else
  pollfd entry;
  entry.fd = fd;
  entry.events = POLLOUT;
  entry.revents = 0;
  const int rc = ::poll(&entry, 1, timeout_ms);
  if (rc <= 0) return rc;
  return 1;  // POLLOUT / POLLERR / POLLHUP 都表示有结论
#endif
}

// 取出 SO_ERROR（非阻塞 connect 的真正结果）。
bool FetchSocketError(int fd, int* out_err) {
  int err = 0;
  SockLen len = static_cast<SockLen>(sizeof(err));
  const int rc = ::getsockopt(ToHandle(fd), SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&err), &len);
  if (rc != 0) {
    *out_err = SocketUtil::LastError();
    return false;
  }
  *out_err = err;
  return true;
}

}  // namespace

std::string Endpoint::ToString() const {
  if (ip.find(':') != std::string::npos) {
    return "[" + ip + "]:" + std::to_string(static_cast<unsigned>(port));
  }
  return ip + ":" + std::to_string(static_cast<unsigned>(port));
}

// ---------------------------------------------------------------------------
// 创建与配置
// ---------------------------------------------------------------------------

int SocketUtil::CreateTcp() {
  PlatformInit::EnsureInit();
  const SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  return FromHandle(s);
}

bool SocketUtil::SetNonBlocking(int fd, bool on) {
  if (fd < 0) return false;
#ifdef _WIN32
  u_long mode = on ? 1UL : 0UL;
  return ::ioctlsocket(ToHandle(fd), static_cast<long>(FIONBIO), &mode) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  const int next = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (next == flags) return true;
  return ::fcntl(fd, F_SETFL, next) == 0;
#endif
}

bool SocketUtil::SetNoDelay(int fd, bool on) {
  if (fd < 0) return false;
  const int value = on ? 1 : 0;
  return ::setsockopt(ToHandle(fd), IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&value), static_cast<SockLen>(sizeof(value))) ==
         0;
}

bool SocketUtil::SetReuseAddr(int fd, bool on) {
  if (fd < 0) return false;
  const int value = on ? 1 : 0;
  return ::setsockopt(ToHandle(fd), SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&value), static_cast<SockLen>(sizeof(value))) ==
         0;
}

bool SocketUtil::SetKeepAlive(int fd, bool on, int idle_s) {
  if (fd < 0) return false;
  if (idle_s <= 0) idle_s = 60;
  const int value = on ? 1 : 0;
  if (::setsockopt(ToHandle(fd), SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char*>(&value),
                   static_cast<SockLen>(sizeof(value))) != 0) {
    return false;
  }
  if (!on) return true;
#ifdef _WIN32
  tcp_keepalive ka;
  std::memset(&ka, 0, sizeof(ka));
  ka.onoff = 1;
  ka.keepalivetime = static_cast<u_long>(idle_s) * 1000UL;
  ka.keepaliveinterval = 1000UL;  // 探测间隔固定 1s
  DWORD returned = 0;
  return ::WSAIoctl(ToHandle(fd), SIO_KEEPALIVE_VALS, &ka, static_cast<DWORD>(sizeof(ka)),
                    nullptr, 0, &returned, nullptr, nullptr) == 0;
#else
  // 平台差异：macOS 没有 TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT（用 TCP_KEEPALIVE），
  // 因此只在明确支持的平台上设置细粒度参数，其余平台仅开 SO_KEEPALIVE。
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
  int idle = idle_s;
  int interval = 1;
  int count = 3;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0) return false;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0) return false;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) return false;
#endif
  return true;
#endif
}

namespace {

template <typename OptValue>
bool SetIntOption(int fd, int level, int opt, OptValue value) {
  if (fd < 0) return false;
  return ::setsockopt(ToHandle(fd), level, opt, reinterpret_cast<const char*>(&value),
                      static_cast<SockLen>(sizeof(value))) == 0;
}

}  // namespace

bool SocketUtil::SetSendBufferSize(int fd, int bytes) {
  return SetIntOption(fd, SOL_SOCKET, SO_SNDBUF, bytes);
}

bool SocketUtil::SetRecvBufferSize(int fd, int bytes) {
  return SetIntOption(fd, SOL_SOCKET, SO_RCVBUF, bytes);
}

// ---------------------------------------------------------------------------
// 地址解析
// ---------------------------------------------------------------------------

bool SocketUtil::ResolveHost(const std::string& host, std::uint16_t port,
                             std::vector<Endpoint>* out) {
  if (out == nullptr) return false;
  out->clear();
  PlatformInit::EnsureInit();

  char port_text[8];
  AppendPortString(port, port_text, sizeof(port_text));

  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), port_text, &hints, &result);
  if (rc != 0 || result == nullptr) {
    if (result != nullptr) ::freeaddrinfo(result);
    return false;
  }
  for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
    Endpoint ep;
    if (!SockAddrToEndpoint(it->ai_addr, &ep)) continue;
    ep.port = port;  // 以调用方传入的端口为准，避免 getaddrinfo 的格式差异
    bool duplicated = false;
    for (const Endpoint& existing : *out) {
      if (existing.ip == ep.ip && existing.port == ep.port) {
        duplicated = true;
        break;
      }
    }
    if (!duplicated) out->push_back(ep);
  }
  ::freeaddrinfo(result);
  return !out->empty();
}

bool SocketUtil::GetPeerAddress(int fd, Endpoint* out) {
  if (fd < 0 || out == nullptr) return false;
  sockaddr_storage storage;
  std::memset(&storage, 0, sizeof(storage));
  SockLen len = static_cast<SockLen>(sizeof(storage));
  if (::getpeername(ToHandle(fd), reinterpret_cast<sockaddr*>(&storage), &len) != 0) return false;
  return SockAddrToEndpoint(reinterpret_cast<const sockaddr*>(&storage), out);
}

bool SocketUtil::GetLocalAddress(int fd, Endpoint* out) {
  if (fd < 0 || out == nullptr) return false;
  sockaddr_storage storage;
  std::memset(&storage, 0, sizeof(storage));
  SockLen len = static_cast<SockLen>(sizeof(storage));
  if (::getsockname(ToHandle(fd), reinterpret_cast<sockaddr*>(&storage), &len) != 0) return false;
  return SockAddrToEndpoint(reinterpret_cast<const sockaddr*>(&storage), out);
}

// ---------------------------------------------------------------------------
// 监听与连接
// ---------------------------------------------------------------------------

int SocketUtil::Listen(const std::string& ip, std::uint16_t port, int backlog,
                       std::string* err) {
  PlatformInit::EnsureInit();
  if (backlog <= 0) backlog = 128;

  sockaddr_storage storage;
  SockLen storage_len = 0;
  if (!FillSockAddr(ip, port, &storage, &storage_len)) {
    if (err != nullptr) *err = "invalid bind address: " + ip;
    return -1;
  }

  const int family = reinterpret_cast<const sockaddr*>(&storage)->sa_family;
  const int fd = FromHandle(::socket(family, SOCK_STREAM, IPPROTO_TCP));
  if (fd < 0) {
    if (err != nullptr) *err = "socket() failed: " + ErrorString(LastError());
    return -1;
  }
  // 监听 socket 的"端口复用"：**Windows 与 POSIX 语义完全不同，不能共用 SO_REUSEADDR。**
  //
  //   * POSIX：SO_REUSEADDR 只影响 TIME_WAIT 状态的连接，是"重启后能立刻重新 bind"的
  //     正确做法，不会让另一个进程抢占同一端口 —— 必须保留。
  //   * Windows：SO_REUSEADDR 允许**另一个进程 bind 同一个 ip:port**，而且后绑的进程会把
  //     连接抢走，先绑的那个变成聋子。症状是"两个实例都起来了、各收到一半请求、随机超时"，
  //     比 bind 失败恶劣得多（失败会立刻被发现，这个会被当成网络抖动查很久）。
  //     所以 Windows 上监听 socket 用 SO_EXCLUSIVEADDRUSE（独占），且**不再**设
  //     SO_REUSEADDR（两者同时设时 EXCLUSIVE 优先，但不要留歧义）。
  //
  // 代价：Windows 上进程刚退出时若仍有连接处于 TIME_WAIT，立刻重启**可能** bind 失败。
  // 这个代价是可接受的：我们的收尾路径（Server::CoordinatorLoop）在主 loop 线程里析构
  // Acceptor 并关掉监听 fd，端口不会被自己占着；真遇到 WSAEADDRINUSE 时调用方会拿到
  // 明确错误，而不是"看起来起来了、实际在抢一半连接"。
  // `SocketUtil::SetReuseAddr` 这个公有 API 保留原语义（连接级复用/用户显式调用），
  // 只是监听路径不再用它。
#ifdef _WIN32
  {
    const int one = 1;
    if (::setsockopt(ToHandle(fd), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&one),
                     static_cast<SockLen>(sizeof(one))) != 0) {
      if (err != nullptr) *err = "SO_EXCLUSIVEADDRUSE failed: " + ErrorString(LastError());
      Close(fd);
      return -1;
    }
  }
#else
  if (!SetReuseAddr(fd, true)) {
    if (err != nullptr) *err = "SO_REUSEADDR failed: " + ErrorString(LastError());
    Close(fd);
    return -1;
  }
#endif
  if (!SetNonBlocking(fd, true)) {
    if (err != nullptr) *err = "set non-blocking failed: " + ErrorString(LastError());
    Close(fd);
    return -1;
  }
  if (::bind(ToHandle(fd), reinterpret_cast<const sockaddr*>(&storage), storage_len) != 0) {
    if (err != nullptr) {
      *err = "bind(" + ip + ":" + std::to_string(static_cast<unsigned>(port)) +
             ") failed: " + ErrorString(LastError());
    }
    Close(fd);
    return -1;
  }
  if (::listen(ToHandle(fd), backlog) != 0) {
    if (err != nullptr) *err = "listen() failed: " + ErrorString(LastError());
    Close(fd);
    return -1;
  }
  return fd;
}

int SocketUtil::ConnectWithTimeout(const Endpoint& ep, int connect_timeout_ms, std::string* err) {
  PlatformInit::EnsureInit();

  sockaddr_storage storage;
  SockLen storage_len = 0;
  if (!FillSockAddr(ep.ip, ep.port, &storage, &storage_len)) {
    if (err != nullptr) *err = "invalid connect address: " + ep.ToString();
    return -1;
  }
  const int family = reinterpret_cast<const sockaddr*>(&storage)->sa_family;
  const int fd = FromHandle(::socket(family, SOCK_STREAM, IPPROTO_TCP));
  if (fd < 0) {
    if (err != nullptr) *err = "socket() failed: " + ErrorString(LastError());
    return -1;
  }
  if (!SetNonBlocking(fd, true)) {
    if (err != nullptr) *err = "set non-blocking failed: " + ErrorString(LastError());
    Close(fd);
    return -1;
  }
  SetNoDelay(fd, true);

  // 非阻塞 connect：三种结果
  //   rc == 0                      → 本机回环等场景直接成功
  //   errno 属于 in-progress      → 等可写事件
  //   其它                        → 立即失败
  bool in_progress = false;
  if (::connect(ToHandle(fd), reinterpret_cast<const sockaddr*>(&storage), storage_len) == 0) {
    return fd;
  }
  const int connect_err = LastError();
#ifdef _WIN32
  in_progress = (connect_err == WSAEWOULDBLOCK || connect_err == WSAEINPROGRESS ||
                 connect_err == WSAEALREADY || connect_err == WSAEINVAL);
#else
  in_progress = (connect_err == EINPROGRESS || connect_err == EWOULDBLOCK || connect_err == EINTR);
#endif
  if (!in_progress) {
    if (err != nullptr) *err = "connect(" + ep.ToString() + ") failed: " + ErrorString(connect_err);
    Close(fd);
    return -1;
  }

  const int ready = WaitWritable(fd, connect_timeout_ms);
  if (ready == 0) {
    if (err != nullptr) {
      *err = "connect(" + ep.ToString() + ") timeout after " +
             std::to_string(connect_timeout_ms) + "ms";
    }
    Close(fd);
    return -1;
  }
  if (ready < 0) {
    const int wait_err = LastError();
    if (err != nullptr) *err = "poll on connect failed: " + ErrorString(wait_err);
    Close(fd);
    return -1;
  }
  int so_error = 0;
  if (!FetchSocketError(fd, &so_error)) {
    if (err != nullptr) *err = "getsockopt(SO_ERROR) failed: " + ErrorString(LastError());
    Close(fd);
    return -1;
  }
  if (so_error != 0) {
    if (err != nullptr) *err = "connect(" + ep.ToString() + ") failed: " + ErrorString(so_error);
    Close(fd);
    return -1;
  }
  return fd;
}

int SocketUtil::Accept(int listen_fd, Endpoint* peer) {
  if (listen_fd < 0) return -1;
  sockaddr_storage storage;
  SockLen len = static_cast<SockLen>(sizeof(storage));
  for (;;) {
    std::memset(&storage, 0, sizeof(storage));
    len = static_cast<SockLen>(sizeof(storage));
    const SocketHandle conn =
        ::accept(ToHandle(listen_fd), reinterpret_cast<sockaddr*>(&storage), &len);
    if (conn == kInvalidSocket) {
      // EINTR（信号打断）时重试；其余（含 EAGAIN）交回调用方判断。
#ifdef _WIN32
      if (LastError() == WSAEINTR) continue;
#else
      if (LastError() == EINTR) continue;
#endif
      return -1;
    }
    const int fd = FromHandle(conn);
    if (fd < 0) return -1;
    if (!SetNonBlocking(fd, true)) {
      Close(fd);
      return -1;
    }
    SetNoDelay(fd, true);
    if (peer != nullptr) {
      Endpoint resolved;
      if (SockAddrToEndpoint(reinterpret_cast<const sockaddr*>(&storage), &resolved)) {
        *peer = resolved;
      } else {
        peer->ip.clear();
        peer->port = 0;
      }
    }
    return fd;
  }
}

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------

long SocketUtil::Recv(int fd, void* buf, std::size_t len) {
  if (fd < 0 || buf == nullptr || len == 0) return -1;
  // 单次 recv 的长度上限：Windows 的 recv 参数是 int。
  const std::size_t capped = len > 0x40000000U ? 0x40000000U : len;
#ifdef _WIN32
  const int rc = ::recv(ToHandle(fd), static_cast<char*>(buf), static_cast<int>(capped), 0);
  return static_cast<long>(rc);
#else
  const ssize_t rc = ::recv(fd, buf, capped, 0);
  return static_cast<long>(rc);
#endif
}

long SocketUtil::Send(int fd, const void* buf, std::size_t len) {
  if (fd < 0 || buf == nullptr || len == 0) return -1;
  const std::size_t capped = len > 0x40000000U ? 0x40000000U : len;
#ifdef _WIN32
  const int rc = ::send(ToHandle(fd), static_cast<const char*>(buf), static_cast<int>(capped), 0);
  return static_cast<long>(rc);
#else
  // MSG_NOSIGNAL 在 Linux 上可用；其他 POSIX 平台依赖 PlatformInit 忽略 SIGPIPE。
#if defined(MSG_NOSIGNAL)
  const ssize_t rc = ::send(fd, buf, capped, MSG_NOSIGNAL);
#else
  const ssize_t rc = ::send(fd, buf, capped, 0);
#endif
  return static_cast<long>(rc);
#endif
}

bool SocketUtil::ShutdownWrite(int fd) {
  if (fd < 0) return false;
#ifdef _WIN32
  return ::shutdown(ToHandle(fd), SD_SEND) == 0;
#else
  return ::shutdown(fd, SHUT_WR) == 0;
#endif
}

void SocketUtil::Close(int fd) {
  if (fd < 0) return;
#ifdef _WIN32
  ::closesocket(ToHandle(fd));
#else
  ::close(fd);
#endif
}

// ---------------------------------------------------------------------------
// 错误
// ---------------------------------------------------------------------------

int SocketUtil::LastError() noexcept {
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

std::string SocketUtil::ErrorString(int err) {
#ifdef _WIN32
  switch (err) {
    case 0:
      return "ok";
    case WSAEINTR:
      return "WSAEINTR (interrupted)";
    case WSAEWOULDBLOCK:
      return "WSAEWOULDBLOCK (would block)";
    case WSAEINPROGRESS:
      return "WSAEINPROGRESS (connect in progress)";
    case WSAEALREADY:
      return "WSAEALREADY (connect already in progress)";
    case WSAENOTSOCK:
      return "WSAENOTSOCK (not a socket)";
    case WSAECONNREFUSED:
      return "WSAECONNREFUSED (connection refused)";
    case WSAECONNRESET:
      return "WSAECONNRESET (connection reset by peer)";
    case WSAECONNABORTED:
      return "WSAECONNABORTED (software caused connection abort)";
    case WSAETIMEDOUT:
      return "WSAETIMEDOUT (connection timed out)";
    case WSAEHOSTUNREACH:
      return "WSAEHOSTUNREACH (no route to host)";
    case WSAENETUNREACH:
      return "WSAENETUNREACH (network unreachable)";
    case WSAEADDRINUSE:
      return "WSAEADDRINUSE (address already in use)";
    case WSAEADDRNOTAVAIL:
      return "WSAEADDRNOTAVAIL (address not available)";
    case WSAENOBUFS:
      return "WSAENOBUFS (no buffer space)";
    case WSAENOTCONN:
      return "WSAENOTCONN (not connected)";
    default:
      return "winsock error " + std::to_string(err);
  }
#else
  const char* text = std::strerror(err);
  return text != nullptr ? std::string(text) : ("errno " + std::to_string(err));
#endif
}

bool SocketUtil::WouldBlock(int err) noexcept {
#ifdef _WIN32
  return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
  return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

bool SocketUtil::IsRetryableConnectError(int err) noexcept {
#ifdef _WIN32
  return err == WSAECONNREFUSED || err == WSAECONNRESET || err == WSAETIMEDOUT ||
         err == WSAEHOSTUNREACH || err == WSAENETUNREACH || err == WSAEADDRNOTAVAIL ||
         err == WSAENETDOWN;
#else
  return err == ECONNREFUSED || err == ECONNRESET || err == ETIMEDOUT || err == EHOSTUNREACH ||
         err == ENETUNREACH || err == EADDRNOTAVAIL || err == ENETDOWN;
#endif
}

}  // namespace net
}  // namespace qzrpc
