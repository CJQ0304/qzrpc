// 轻舟 RPC (Qingzhou RPC)
// src/net/channel.h
//
// 职责：把「一个 fd + 一组关注事件 + 一组回调」绑成一个对象，是 EventLoop 与
//       具体 IO 载体（Acceptor / Connector / Connection）之间的适配器。
//
// 不拥有 fd：Channel 绝不 close(fd)。fd 的生命周期归 Acceptor / Connector / Connection 所有。
//            Channel 只负责"在 Poller 里登记/注销这个 fd 的关注事件"。
//
// 线程安全：**非线程安全**。所有方法必须在 owner_loop() 的 loop 线程调用。
//           其他线程需要改事件时，用 EventLoop::RunInLoop 转投，例如：
//             loop->RunInLoop([ch]{ ch->EnableWriting(); });
#pragma once

#include <cstdint>
#include <functional>

#include "common/noncopyable.h"
#include "net/poller.h"

namespace qzrpc {
namespace net {

class EventLoop;

// fd 事件载体。
class Channel : private NonCopyable {
 public:
  // 事件回调。触发原因由注册的回调种类区分（读/写/错误/挂断各一个）。
  using EventCallback = std::function<void()>;

  // 构造。参数：loop - 归属的事件循环（非空）；fd - 已就绪的非阻塞句柄。
  // 构造本身不向 Poller 注册任何事件（events() 为 kNone）。
  Channel(EventLoop* loop, int fd);

  // 析构。若仍 attached()，会尝试从 Poller 注销（要求当前线程是 loop 线程；
  // 否则只是把 attached_ 置 false，由调用方保证不在此后访问）。
  ~Channel();

  // 归属的事件循环。
  EventLoop* owner_loop() const noexcept { return loop_; }

  // 文件描述符。Channel 不拥有它。
  int fd() const noexcept { return fd_; }

  // ---- 回调注册（必须在 EnableXxx 之前或同时完成） ----

  // 可读事件。回调内必须把数据读干净，否则会触发水平触发风暴。
  void SetReadCallback(EventCallback cb);
  // 可写事件。
  void SetWriteCallback(EventCallback cb);
  // 错误事件（SO_ERROR 非零）。回调后连接通常已不可用。
  void SetErrorCallback(EventCallback cb);
  // 对端关闭 / HUP。回调内应关闭并销毁连接。
  void SetCloseCallback(EventCallback cb);

  // ---- 事件开关 ----

  // 关注可读。已在关注中则幂等。
  void EnableReading();
  // 取消关注可读。用于背压（如输出缓冲积压时暂停读）。
  void DisableReading();
  // 关注可写。注册写回调后调用。
  void EnableWriting();
  // 取消关注可写（写完后必须调用，否则会 busy loop）。
  void DisableWriting();
  // 取消全部关注（不注销 fd）。
  void DisableAll();

  // 当前是否在关注读 / 写。
  bool reading() const noexcept;
  bool writing() const noexcept;

  // 当前关注的事件位（Poller::Event 的组合）。
  std::uint32_t events() const noexcept { return events_; }

  // 是否已登记在 Poller 中。
  bool attached() const noexcept { return attached_; }

  // 从事件循环中彻底注销。注销后本 Channel 不再收到任何事件，
  // 但 fd 仍然有效（关闭 fd 是持有者的责任）。
  void Remove();

 private:
  friend class EventLoop;

  // 由 EventLoop 派发：按 revents 调用对应回调。
  // 顺序固定为 Error → Hup(Close) → Read → Write，保证出错时不再处理数据。
  void HandleEvent(std::uint32_t revents);

  // 把 events_ 的变化同步到 Poller。只能在 loop 线程调用。
  void SyncToPoller();

  EventLoop* loop_{nullptr};
  int fd_{-1};
  std::uint32_t events_{Poller::kNone};
  bool attached_{false};
  bool handling_event_{false};

  EventCallback read_cb_;
  EventCallback write_cb_;
  EventCallback error_cb_;
  EventCallback close_cb_;
};

}  // namespace net
}  // namespace qzrpc
