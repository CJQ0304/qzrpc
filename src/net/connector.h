// 轻舟 RPC (Qingzhou RPC)
// src/net/connector.h
//
// 职责：异步发起一次 TCP 连接（非阻塞 connect + 可写事件 + 超时定时器），
//       成功后把 fd 交还调用方。客户端 ConnectionPool 用它补充连接。
//
// 为什么不用 SocketUtil::ConnectWithTimeout（阻塞版）：
//   客户端 ConnectionPool 需要在不阻塞调用方的线程的前提下预热连接（后台补连接）。
//   阻塞版只适合"调用线程自己愿意等"的简单场景（如 example 里的直连 demo）。
//
// 一次 Connector 只服务一次连接尝试，失败即弃（不复用）。
//
// 线程安全：Start/Cancel 线程安全（内部转投到 loop 线程）；其余方法非线程安全。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/noncopyable.h"
#include "net/socket_util.h"
#include "net/timer_queue.h"

namespace qzrpc {
namespace net {

class Channel;
class EventLoop;

// 异步连接器。
class Connector : private NonCopyable {
 public:
  // 连接结果回调（在 loop 线程执行）。
  // 参数：fd - 成功时为已连接的非阻塞 fd（所有权移交回调），失败为 -1；
  //       peer - 目标端点；error - 失败原因，成功时为空串。
  using ConnectResultCallback =
      std::function<void(int fd, const Endpoint& peer, const std::string& error)>;

  // 构造。参数：loop - 归属事件循环；peer - 目标端点（必须是已解析的 IP）；
  //       connect_timeout_ms - 连接超时毫秒。
  Connector(EventLoop* loop, const Endpoint& peer, int connect_timeout_ms);
  ~Connector();

  // 发起连接。cb 在成功后或失败/超时后**恰好被调用一次**。线程安全，只能调一次。
  void Start(ConnectResultCallback cb);

  // 取消进行中的连接尝试。若尚未回调，会以 error="cancelled" 回调一次。线程安全。
  void Cancel();

  // 是否已产生结果（成功或失败）。线程安全。
  bool finished() const noexcept;

 private:
  // 在 loop 线程真正发起 connect。
  void StartInLoop();
  // 可写事件：用 SO_ERROR 判断成败。
  void HandleWritable();
  // 超时定时器。
  void HandleTimeout();
  // 统一的结果收敛：清理资源并回调。
  void FinishWithError(const std::string& error);
  void FinishWithSuccess();
  // 关闭 fd 并注销 Channel（幂等）。
  void CloseAndDetach();

  EventLoop* loop_{nullptr};
  Endpoint peer_;
  int connect_timeout_ms_{200};
  int fd_{-1};
  std::unique_ptr<Channel> channel_;
  TimerId timeout_timer_{0};
  ConnectResultCallback cb_;
  bool started_{false};
  bool finished_{false};
};

}  // namespace net
}  // namespace qzrpc
