// 轻舟 RPC (Qingzhou RPC)
// src/net/acceptor.h
//
// 职责：持有监听 fd，在 EventLoop 线程上把新连接 accept 出来并交给上层。
//       服务端的 Acceptor 运行在**主 Reactor 线程**，与 N 个 IO 从 Reactor 分离。
//
// 背压开关：Stop() 只是把监听 fd 从 Poller 上摘掉，内核仍然会完成 TCP 三次握手
//          （连接堆积在 backlog 里），因此客户端不会立刻收到 RST——这正是优雅降级
//          想要的行为（配合 SO_REUSEADDR 与合理 backlog）。
//
// 线程安全：**非线程安全**，除标注外必须在 owner loop 线程调用。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/noncopyable.h"
#include "net/socket_util.h"

namespace qzrpc {
namespace net {

class Channel;
class EventLoop;

// TCP 监听器。
class Acceptor : private NonCopyable {
 public:
  // 新连接回调。
  // 参数：conn_fd - 已完成连接的非阻塞 fd（**所有权移交给回调**，回调必须接管或关闭）；
  //       peer - 对端地址。
  using NewConnectionCallback = std::function<void(int conn_fd, const Endpoint& peer)>;

  // 构造。参数：loop - 归属事件循环；bind_ip - 绑定地址；port - 端口（0 表示系统分配）。
  // 构造不监听，需显式调用 Listen()。
  Acceptor(EventLoop* loop, const std::string& bind_ip, std::uint16_t port);
  ~Acceptor();

  // 创建监听 fd 并登记到 Poller（此时还未 accept）。
  // 参数：backlog - listen 的 backlog；err - 失败原因。
  // 返回：true 表示监听成功。
  bool Listen(int backlog, std::string* err);

  // 设置新连接处理回调。**必须在 Start() 之前设置**。
  void SetNewConnectionCallback(NewConnectionCallback cb);

  // 开始 accept（把监听 fd 的可读事件打开）。
  void Start();

  // 停止 accept（把监听 fd 从 Poller 摘除，但**不关闭**监听 fd）。
  // 用途：优雅停机第一步、过载时拒绝新连接。
  void Stop();

  // 是否已成功 Listen。
  bool listening() const noexcept { return listen_fd_ >= 0; }

  // 当前是否在 accept。
  bool accepting() const noexcept;

  // 监听 fd（不属于调用方，不要关闭）。
  int listen_fd() const noexcept { return listen_fd_; }

  // 实际监听端口（port 传 0 时用于取系统分配的端口，测试常用）。
  std::uint16_t local_port() const noexcept { return local_port_; }

  // 单次可读事件里最多 accept 多少个连接，默认 64。
  // 目的：避免"惊群式"地在一个连接上耗太久，让定时器与任务队列有机会执行。
  void set_accept_batch_size(int n) noexcept;

  // 累计 accept 成功/失败的连接数（metrics 用，读时无锁）。
  std::uint64_t accepted_total() const noexcept;
  std::uint64_t accept_failed_total() const noexcept;

 private:
  // Channel 的可读回调：循环 accept 直到 EAGAIN 或达到 batch 上限。
  void HandleReadable();

  EventLoop* loop_{nullptr};
  std::string bind_ip_;
  std::uint16_t bind_port_{0};
  std::uint16_t local_port_{0};
  int listen_fd_{-1};
  int accept_batch_size_{64};
  std::unique_ptr<Channel> channel_;
  NewConnectionCallback new_conn_cb_;
  std::uint64_t accepted_total_{0};
  std::uint64_t accept_failed_total_{0};
};

}  // namespace net
}  // namespace qzrpc
