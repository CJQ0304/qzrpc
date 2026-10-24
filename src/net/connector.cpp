// 轻舟 RPC (Qingzhou RPC)
// src/net/connector.cpp
//
// 职责：异步发起一次 TCP 连接（非阻塞 connect + 可写事件 + 超时定时器）。
//
// ★ 核心竞态（doc 00 §3.1 第 2 条）：
//   可写事件与超时定时器可能在同一轮 EventLoop 里都触发。如果没有单次收敛，
//   会回调两次，并把同一个 fd 释放两次。本文件用 finished_ 作为唯一闸门：
//   所有出口（成功 / SO_ERROR / 超时 / 取消）都必须先过 `if (finished_) return;`
//   然后置位，再清理资源，最后回调（回调时资源已清理干净，位置也已从 Poller 摘除）。
//
// 契约缺口说明：SocketUtil 只提供"阻塞版 ConnectWithTimeout"，没有提供
// "在已有 fd 上发起非阻塞 connect"。本文件用匿名 namespace 内的
// StartNonBlockingConnect/FetchSocketError 补齐（这属于实现细节，不进公共头）。
//
// ⚠️ 当前**没有调用方**（`grep Connector src/**/*.cpp` 只命中本文件自己）。
//   保留它是因为网络层的"异步建连"能力本身是完整的，将来连接预热 / 后台建连会用到。
//   客户端连接池是**故意**改成同步建连的（见 connection_pool.cpp 文件头的理由：
//   异步建连需要 loop 线程与调用方共享连接对象，在 Channel 停机路径上会撞上
//   "shared_ptr 最后一个引用必须在 loop 线程释放"的约束，反而更容易出 use-after-free）。
//   所以：**不要**为了"看起来更异步"把连接池改回用 Connector，那正好会重新踩上那个坑。

#include "net/connector.h"

#include <cstring>
#include <utility>

#include "common/logger.h"
#include "net/channel.h"
#include "net/event_loop.h"
#include "net/platform.h"

namespace qzrpc {
namespace net {

namespace {

#ifdef _WIN32
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

// 把 IP 字面量端点填成 sockaddr。
bool FillSockAddr(const Endpoint& ep, sockaddr_storage* out, SockLen* out_len) {
  std::memset(out, 0, sizeof(*out));
  const bool is_v6 = ep.ip.find(':') != std::string::npos;
  if (is_v6) {
    sockaddr_in6* addr6 = reinterpret_cast<sockaddr_in6*>(out);
    addr6->sin6_family = static_cast<unsigned short>(AF_INET6);
    addr6->sin6_port = ::htons(ep.port);
    if (::inet_pton(AF_INET6, ep.ip.c_str(), &addr6->sin6_addr) != 1) return false;
    *out_len = static_cast<SockLen>(sizeof(sockaddr_in6));
    return true;
  }
  sockaddr_in* addr4 = reinterpret_cast<sockaddr_in*>(out);
  addr4->sin_family = static_cast<unsigned short>(AF_INET);
  addr4->sin_port = ::htons(ep.port);
  if (::inet_pton(AF_INET, ep.ip.c_str(), &addr4->sin_addr) != 1) return false;
  *out_len = static_cast<SockLen>(sizeof(sockaddr_in));
  return true;
}

// 在已创建的非阻塞 fd 上发起 connect。
// 返回：0 已连上；1 正在进行（等可写事件）；-1 立即失败（err 填 LastError）。
int StartNonBlockingConnect(int fd, const Endpoint& ep, int* err) {
  sockaddr_storage storage;
  SockLen storage_len = 0;
  if (!FillSockAddr(ep, &storage, &storage_len)) {
    *err = 0;
    return -1;
  }
  if (::connect(static_cast<SocketHandle>(fd), reinterpret_cast<const sockaddr*>(&storage),
                storage_len) == 0) {
    return 0;
  }
  const int rc = SocketUtil::LastError();
#ifdef _WIN32
  if (rc == WSAEWOULDBLOCK || rc == WSAEINPROGRESS || rc == WSAEALREADY) return 1;
#else
  if (rc == EINPROGRESS || rc == EWOULDBLOCK) return 1;
#endif
  *err = rc;
  return -1;
}

// 取 SO_ERROR：非阻塞 connect 的最终结果在这里。
bool FetchSocketError(int fd, int* out_err) {
  int value = 0;
  SockLen len = static_cast<SockLen>(sizeof(value));
  if (::getsockopt(static_cast<SocketHandle>(fd), SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&value), &len) != 0) {
    *out_err = SocketUtil::LastError();
    return false;
  }
  *out_err = value;
  return true;
}

// 摘除并**延迟**析构 Channel。
//
// ★ 为什么不能直接 reset()：本函数几乎总是跑在这个 Channel 自己的事件派发栈上
//   （HandleWritable → FinishWithSuccess / FinishWithError）。
//   Channel::HandleEvent 返回时还要写 handling_event_，同步析构就是写已释放内存
//   （ASAN 实测：heap-use-after-free, WRITE of size 1，Channel 恰好 280 字节、写偏移 17）。
//   做法：把所有权塞进一个排队任务，由下一次 DoPendingTasks 释放。
//
// 放在匿名 namespace 是因为 connector.h 已冻结，不能新增私有成员函数。
void DetachChannelLater(std::unique_ptr<Channel>* channel, EventLoop* loop) {
  if (channel == nullptr || !*channel) return;
  (*channel)->DisableAll();  // 先从 Poller 摘除，避免 fd 关闭后还有事件上报
  (*channel)->Remove();
  // std::function 要求目标可拷贝，所以这里用 shared_ptr 而不是 unique_ptr 承载所有权：
  // 只要任务还在队列里，Channel 就不会被析构。
  std::shared_ptr<Channel> dying(std::move(*channel));
  channel->reset();
  if (loop == nullptr) return;  // 没有事件循环：不在派发栈上，直接析构
  loop->QueueInLoop([dying] { (void)dying; });
}

}  // namespace

Connector::Connector(EventLoop* loop, const Endpoint& peer, int connect_timeout_ms)
    : loop_(loop), peer_(peer), connect_timeout_ms_(connect_timeout_ms) {
  if (connect_timeout_ms_ <= 0) connect_timeout_ms_ = 200;
}

Connector::~Connector() {
  // 析构时若仍有在飞的连接，必须静默清理：fd 关掉、定时器取消、不回调。
  if (timeout_timer_ != 0 && loop_ != nullptr) {
    loop_->CancelTimer(timeout_timer_);
    timeout_timer_ = 0;
  }
  CloseAndDetach();
  finished_ = true;
}

void Connector::Start(ConnectResultCallback cb) {
  if (started_) return;  // 只能调一次
  started_ = true;
  cb_ = std::move(cb);
  if (loop_ == nullptr) {
    FinishWithError("connector: null event loop");
    return;
  }
  // 线程安全：统一转投到 loop 线程，避免"在调用方线程建 fd、在 loop 线程用它"。
  EventLoop* loop = loop_;
  loop->RunInLoop([this] { StartInLoop(); });
}

void Connector::Cancel() {
  if (loop_ == nullptr) return;
  // 注意：本闭包捕获裸 this。调用方必须保证 Connector 在回调触发前存活
  // （头文件契约：一次 Connector 只服务一次连接尝试，生命周期由 ConnectionPool 持有）。
  EventLoop* loop = loop_;
  loop->RunInLoop([this] {
    if (finished_) return;
    FinishWithError("cancelled");
  });
}

bool Connector::finished() const noexcept { return finished_; }

void Connector::StartInLoop() {
  if (finished_) return;  // Cancel 可能先到
  const int fd = SocketUtil::CreateTcp();
  if (fd < 0) {
    FinishWithError("socket() failed: " + SocketUtil::ErrorString(SocketUtil::LastError()));
    return;
  }
  fd_ = fd;
  if (!SocketUtil::SetNonBlocking(fd_, true)) {
    FinishWithError("set non-blocking failed: " + SocketUtil::ErrorString(SocketUtil::LastError()));
    return;
  }
  SocketUtil::SetNoDelay(fd_, true);

  int err = 0;
  const int rc = StartNonBlockingConnect(fd_, peer_, &err);
  if (rc == 0) {
    // 本机回环等场景可能立刻连上：直接收敛，不需要 Channel 与定时器。
    FinishWithSuccess();
    return;
  }
  if (rc < 0) {
    const std::string reason =
        err != 0 ? SocketUtil::ErrorString(err) : ("invalid peer address: " + peer_.ToString());
    FinishWithError("connect(" + peer_.ToString() + ") failed: " + reason);
    return;
  }

  channel_.reset(new Channel(loop_, fd_));
  // 非阻塞 connect 的完成既可能报"可写"，也可能直接报"错误"，两个回调走同一条路径。
  channel_->SetWriteCallback([this] { HandleWritable(); });
  channel_->SetErrorCallback([this] { HandleWritable(); });
  channel_->EnableWriting();

  timeout_timer_ = loop_->RunAfter(static_cast<std::uint64_t>(connect_timeout_ms_), [this] {
    HandleTimeout();
  });
}

void Connector::HandleWritable() {
  if (finished_) return;  // 定时器已经先一步收敛
  int err = 0;
  if (!FetchSocketError(fd_, &err)) {
    FinishWithError("getsockopt(SO_ERROR) failed: " + SocketUtil::ErrorString(SocketUtil::LastError()));
    return;
  }
  if (err != 0) {
    FinishWithError("connect(" + peer_.ToString() + ") failed: " + SocketUtil::ErrorString(err));
    return;
  }
  FinishWithSuccess();
}

void Connector::HandleTimeout() {
  timeout_timer_ = 0;  // 定时器已触发，句柄失效
  if (finished_) return;  // 成功路径已经先一步收敛
  FinishWithError("connect(" + peer_.ToString() + ") timeout after " +
                  std::to_string(connect_timeout_ms_) + "ms");
}

void Connector::CloseAndDetach() {
  DetachChannelLater(&channel_, loop_);
  if (fd_ >= 0) {
    SocketUtil::Close(fd_);
    fd_ = -1;
  }
}

void Connector::FinishWithError(const std::string& error) {
  if (finished_) return;
  finished_ = true;  // ← 单次收敛的唯一闸门，必须在任何清理/回调之前置位
  if (timeout_timer_ != 0 && loop_ != nullptr) {
    loop_->CancelTimer(timeout_timer_);
    timeout_timer_ = 0;
  }
  CloseAndDetach();
  ConnectResultCallback cb = std::move(cb_);
  cb_ = nullptr;
  if (cb) cb(-1, peer_, error);
}

void Connector::FinishWithSuccess() {
  if (finished_) return;
  finished_ = true;  // ← 同上
  if (timeout_timer_ != 0 && loop_ != nullptr) {
    loop_->CancelTimer(timeout_timer_);
    timeout_timer_ = 0;
  }
  // 成功路径：先把 Channel 从 Poller 摘掉（**不要**关 fd），再移交所有权。
  DetachChannelLater(&channel_, loop_);
  const int fd = fd_;
  fd_ = -1;
  ConnectResultCallback cb = std::move(cb_);
  cb_ = nullptr;
  if (cb) cb(fd, peer_, std::string());
  // 回调里调用方可能立刻开始使用该 fd，也可能失败后自行关闭；两种都不影响本对象。
}

}  // namespace net
}  // namespace qzrpc
