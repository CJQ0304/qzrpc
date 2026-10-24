// 轻舟 RPC (Qingzhou RPC)
// src/net/channel.cpp
//
// 职责：fd 上的关注事件与回调的适配器。**不拥有 fd**，绝不 close。
//
// 派发顺序固定 Error → Hup → Read → Write（见 doc 00 §3 N-06）：
//   出错/挂断后不再处理数据，否则会在已经半关闭的 fd 上做无意义的 recv。
//
// handling_event_ 的作用：回调里可能调 Remove()（甚至触发上层销毁本对象）。
// Remove() 会把 handling_event_ 置 false，派发器看到它就不再继续后面的回调，
// 从而避免"事件已注销却还在派发"。

#include "net/channel.h"

#include <utility>

#include "net/event_loop.h"

namespace qzrpc {
namespace net {

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

Channel::~Channel() {
  if (!attached_) return;
  if (loop_ != nullptr && loop_->IsInLoopThread()) {
    Remove();
  } else {
    // 不在 loop 线程：按下标摘除是不安全的（channels_ 只能由 loop 线程改），
    // 头文件契约要求调用方保证此后不再访问本对象，因此这里只撤销本地标记，
    // 让 Poller 里的那个 fd 由后续的 Remove/Close 路径清理。
    events_ = Poller::kNone;
    attached_ = false;
  }
}

void Channel::SetReadCallback(EventCallback cb) { read_cb_ = std::move(cb); }

void Channel::SetWriteCallback(EventCallback cb) { write_cb_ = std::move(cb); }

void Channel::SetErrorCallback(EventCallback cb) { error_cb_ = std::move(cb); }

void Channel::SetCloseCallback(EventCallback cb) { close_cb_ = std::move(cb); }

void Channel::EnableReading() {
  const bool changed = (events_ & Poller::kReadable) == 0;
  events_ |= Poller::kReadable;
  if (changed || !attached_) SyncToPoller();
}

void Channel::DisableReading() {
  if ((events_ & Poller::kReadable) == 0) return;
  events_ &= ~static_cast<std::uint32_t>(Poller::kReadable);
  SyncToPoller();
}

void Channel::EnableWriting() {
  const bool changed = (events_ & Poller::kWritable) == 0;
  events_ |= Poller::kWritable;
  if (changed || !attached_) SyncToPoller();
}

void Channel::DisableWriting() {
  if ((events_ & Poller::kWritable) == 0) return;
  events_ &= ~static_cast<std::uint32_t>(Poller::kWritable);
  SyncToPoller();
}

void Channel::DisableAll() {
  if (events_ == Poller::kNone && !attached_) return;
  events_ = Poller::kNone;
  SyncToPoller();
}

bool Channel::reading() const noexcept { return (events_ & Poller::kReadable) != 0; }

bool Channel::writing() const noexcept { return (events_ & Poller::kWritable) != 0; }

void Channel::Remove() {
  if (loop_ != nullptr) loop_->RemoveChannel(this);
  // 即使之前没登记在 Poller 里，也要把本地状态归零：调用方的语义是"以后别再给我事件"。
  events_ = Poller::kNone;
  attached_ = false;
  handling_event_ = false;  // 通知派发器停止后续回调
}

void Channel::SyncToPoller() {
  if (loop_ == nullptr) return;
  if (events_ == Poller::kNone) {
    if (attached_) loop_->RemoveChannel(this);
    return;
  }
  // UpdateChannel 内部会区分"首次 Add"与"已登记则 Mod"，并把 attached_ 置 true。
  loop_->UpdateChannel(this);
}

void Channel::HandleEvent(std::uint32_t revents) {
  if (revents == Poller::kNone) return;
  handling_event_ = true;
  if ((revents & Poller::kError) != 0 && error_cb_) error_cb_();
  if (handling_event_ && (revents & Poller::kHup) != 0 && close_cb_) close_cb_();
  if (handling_event_ && (revents & Poller::kReadable) != 0 && read_cb_) read_cb_();
  if (handling_event_ && (revents & Poller::kWritable) != 0 && write_cb_) write_cb_();
  handling_event_ = false;
}

}  // namespace net
}  // namespace qzrpc
