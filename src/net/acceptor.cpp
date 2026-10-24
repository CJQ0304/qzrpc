// 轻舟 RPC (Qingzhou RPC)
// src/net/acceptor.cpp
//
// 职责：持有监听 fd，在 EventLoop 线程把新连接 accept 出来交给上层。
//
// 关键约定：
//   * Stop() 只把监听 fd 从 Poller 摘掉，**不 close** —— 内核继续完成三次握手，
//     连接堆在 backlog 里，客户端不会立刻收到 RST（优雅降级）。
//     监听 fd 的关闭只发生在析构时。
//   * 每 accept 出一个 fd 就立刻设非阻塞与 TCP_NODELAY（socket_util 已做），
//     避免上层忘记设置导致某个连接退化成阻塞 IO。
//   * 单次可读事件最多 accept accept_batch_size_ 个，避免在本回调里长时间占用
//     loop 线程、饿死定时器与任务队列。剩下的连接下一轮可读事件会继续取（LT）。

#include "net/acceptor.h"

#include <utility>

#include "common/logger.h"
#include "net/channel.h"
#include "net/event_loop.h"
#include "net/platform.h"  // 仅为了判 ECONNABORTED / WSAECONNABORTED 的平台差异

namespace qzrpc {
namespace net {

Acceptor::Acceptor(EventLoop* loop, const std::string& bind_ip, std::uint16_t port)
    : loop_(loop), bind_ip_(bind_ip), bind_port_(port) {}

Acceptor::~Acceptor() {
  if (channel_) {
    channel_->DisableAll();
    channel_->Remove();
    channel_.reset();
  }
  if (listen_fd_ >= 0) {
    SocketUtil::Close(listen_fd_);
    listen_fd_ = -1;
  }
}

bool Acceptor::Listen(int backlog, std::string* err) {
  if (listen_fd_ >= 0) return true;  // 幂等：已监听
  if (loop_ == nullptr) {
    if (err != nullptr) *err = "acceptor: null event loop";
    return false;
  }
  std::string local_err;
  const int fd = SocketUtil::Listen(bind_ip_, bind_port_, backlog, &local_err);
  if (fd < 0) {
    if (err != nullptr) *err = local_err;
    return false;
  }
  listen_fd_ = fd;

  Endpoint local;
  if (SocketUtil::GetLocalAddress(fd, &local)) {
    local_port_ = local.port;
  } else {
    local_port_ = bind_port_;
  }

  // Channel 不拥有 fd：监听 fd 的关闭由本类析构负责。
  channel_.reset(new Channel(loop_, fd));
  channel_->SetReadCallback([this] { HandleReadable(); });
  channel_->SetErrorCallback([this] { HandleReadable(); });
  return true;
}

void Acceptor::SetNewConnectionCallback(NewConnectionCallback cb) {
  new_conn_cb_ = std::move(cb);
}

void Acceptor::Start() {
  if (channel_ == nullptr) return;
  channel_->EnableReading();
}

void Acceptor::Stop() {
  if (channel_ == nullptr) return;
  // DisableAll 会把 fd 从 Poller 摘除（不关闭 fd）。
  channel_->DisableAll();
}

bool Acceptor::accepting() const noexcept {
  return channel_ != nullptr && channel_->reading();
}

void Acceptor::set_accept_batch_size(int n) noexcept {
  accept_batch_size_ = n > 0 ? n : 1;
}

std::uint64_t Acceptor::accepted_total() const noexcept { return accepted_total_; }

std::uint64_t Acceptor::accept_failed_total() const noexcept { return accept_failed_total_; }

void Acceptor::HandleReadable() {
  for (int i = 0; i < accept_batch_size_; ++i) {
    Endpoint peer;
    const int conn_fd = SocketUtil::Accept(listen_fd_, &peer);
    if (conn_fd < 0) {
      const int err = SocketUtil::LastError();
      if (SocketUtil::WouldBlock(err)) return;  // 正常：本轮连接已取完
#ifdef _WIN32
      const bool aborted = (err == WSAECONNABORTED);
#else
      const bool aborted = (err == ECONNABORTED);
#endif
      if (aborted) continue;  // 对端在握手后立刻 RST：跳过，不算错误
      ++accept_failed_total_;
      // 走到这里通常是 EMFILE / ENFILE（fd 用尽）或监听 fd 已失效。
      // 不做等待重试（会卡住 loop 线程），直接返回，下一轮可读事件再试。
      QZ_LOG_ERROR << "acceptor: accept failed on " << bind_ip_ << ":" << bind_port_
                   << ": " << SocketUtil::ErrorString(err);
      return;
    }
    ++accepted_total_;
    if (new_conn_cb_) {
      // 所有权在此移交：回调必须接管（构造 Connection）或关闭该 fd。
      new_conn_cb_(conn_fd, peer);
    } else {
      // 没设回调就 accept 是配置错误，但不能泄漏 fd。
      QZ_LOG_WARN << "acceptor: accepted fd " << conn_fd << " but no callback set, closing";
      SocketUtil::Close(conn_fd);
    }
  }
}

}  // namespace net
}  // namespace qzrpc
