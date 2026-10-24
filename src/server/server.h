// 轻舟 RPC (Qingzhou RPC)
// src/server/server.h
//
// 职责：服务端顶层门面。把「主 Reactor + N 个从 Reactor + 业务线程池 + 连接管理 +
//       优雅停机 + metrics」组装成一个可直接 Run() 的对象。
//
// 线程模型（一个 Server 实例的线程清单）：
//   * 1 个 acceptor 线程：跑主 EventLoop，只做 accept + 把 fd 分发给从 Reactor；
//   * N 个 io 线程（io_thread_count）：各跑一个 EventLoop，负责读帧 → 派发 → 写回；
//   * M 个 worker 线程（worker_thread_count）：跑业务，统一取自 ThreadPool；
//   * 1 个 housekeeping 线程：空闲连接回收、自适应拒绝比例调节、metrics 上报。
//   合计 1 + N + M + 1 个线程。N 与 M 都可由配置控制。
//
// 为什么用 pimpl：
//   服务端内部有 10+ 个互相引用的组件（Acceptor/EventLoop/ThreadPool/连接表/统计），
//   全部摊在头文件里会让公共头变得不可读，也会把实现细节泄漏给使用者。
//   代价是多一次指针跳转，对 RPC 这种网络密集场景可忽略。
//
// 生命周期：
//   Server(opts) → RegisterService()* → Start() → Run()（阻塞） → Stop() → 析构
//   Stop() 可从任意线程（包括信号处理线程之外的任意线程）调用，Run() 随即返回。
//
// 线程安全：RegisterService 只能在 Start() 之前调用；
//           Start/Stop/Run/Wait/running/port 线程安全。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/noncopyable.h"
#include "server/dispatcher.h"
#include "server/server_options.h"

namespace qzrpc {
namespace rpc {

// 服务端。
class Server : private NonCopyable {
 public:
  // 构造（使用默认配置）。会校验配置合法性。
  Server();

  // 构造。会校验 options 合法性（非法时把原因记到 error_message() 并置 valid=false）。
  explicit Server(const ServerOptions& options);

  ~Server();

  // 配置是否合法（端口范围、线程数、frame 上限等）。
  bool valid() const noexcept;

  // 配置非法时的原因；合法时为空串。
  const std::string& error_message() const noexcept;

  // 生效的配置。
  const ServerOptions& options() const noexcept;

  // 取内部路由表以注册方法。**只能在 Start() 之前调用**。
  // 生成代码里的 RegisterXxxService(Dispatcher*) 会往这里写。
  Dispatcher* mutable_dispatcher();

  // 启动：创建监听、启动全部线程。非阻塞。
  // 参数：err - 可选，失败原因。
  // 返回：true 表示已开始监听。重复调用返回 true 且不重复启动。
  bool Start(std::string* err = nullptr);

  // 阻塞运行，直到 Stop() 被调用。必须在 Start() 成功之后调用。
  // 语义：内部只做 wait，不做轮询；Stop() 会唤醒它。
  void Run();

  // 优雅停机（四步，详见 doc 02 §5）。线程安全，幂等。
  //   1) 主 Reactor 停止 accept；
  //   2) 向所有连接广播"正在停机"语义：新请求直接回 kServiceUnavailable；
  //   3) 等待在飞请求排空，最长 graceful_shutdown_timeout_ms；
  //   4) 关闭全部连接与线程。
  // 注意：Stop() 本身不阻塞等待全部线程退出（那是 Wait() 的事），但会唤醒 Run()。
  void Stop();

  // 等待全部线程真正退出并释放资源。Stop() 之后调用（析构时也会自动调用）。
  // 参数：timeout_ms - 最长等待毫秒，<0 表示一直等。
  // 返回：true 表示已完全停止。
  bool Wait(int timeout_ms = -1);

  // 是否正在运行（Start 之后、Wait 完成之前）。
  bool running() const noexcept;

  // 实际监听端口（options.port 为 0 时用于取系统分配值）。未启动返回 0。
  std::uint16_t port() const noexcept;

  // 已建立连接数（当前）。线程安全。
  std::size_t connection_count() const noexcept;

  // ---- 内建指标 ----

  // 注册框架标准指标（幂等）。
  // 指标名约定（Prometheus 导出名；注册时用同名，点分只出现在文档里）：
  //   qzrpc_server_connection_total         (Counter)    累计 accept 的连接数
  //   qzrpc_server_connection_active        (**gauge**)  当前活跃连接数。
  //       注册表里**没有**这个 Counter——它由 `/metrics` 端点手写成 `# TYPE ... gauge` 行
  //       （值来自一个 atomic 计数）。Counter 只能 Inc()，硬塞一个只涨不跌的"活跃数"
  //       比不报更坏。
  //   qzrpc_server_request_total            (Counter)    累计请求数
  //   qzrpc_server_response_total           (Counter)    累计响应数
  //   qzrpc_server_rejected_total           (Counter)    过载拒绝数
  //   qzrpc_server_error_total              (Counter)    非 ok 响应数
  //   qzrpc_server_request_latency_us       (Histogram)  请求端到端延迟（收到帧→写出响应，微秒）
  //   qzrpc_server_queue_wait_us            (Histogram)  业务排队等待（微秒）
  //   qzrpc_server_worker_busy              (Counter)    **累计**业务执行耗时（微秒，`Inc(us)`），
  //       用来算利用率（`rate(worker_busy)/rate(请求数)`）。名字看起来像瞬时并发数，
  //       实际是累计量——看板配置时按 `rate()` 用，别直接读原始值当"当前忙几个"。
  static void RegisterDefaultMetrics();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rpc
}  // namespace qzrpc
