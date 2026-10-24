// 轻舟 RPC (Qingzhou RPC)
// src/net/thread_pool.h
//
// 职责：固定线程数 + **有界**任务队列的线程池。服务端业务线程池的唯一实现。
//
// 与 phxrpc 的区别：
//   * phxrpc 的 DataFlow 队列是"无界 push + 上层预判 CanPushRequest"两段式，
//     判断与 push 之间存在竞态，队列仍可能超限；
//   * 本实现把"入队"与"容量检查"放进同一把锁，Submit 的返回值就是最终结论，
//     不会出现"检查通过但入队失败"的窗口；
//   * 过载时返回 kRejected 而**不是**关连接，服务端据此回 OVERLOAD 帧。
//
// 线程安全：Start/Stop/Submit 与全部观测方法线程安全。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/noncopyable.h"

namespace qzrpc {
namespace net {

// 固定大小业务线程池。
class ThreadPool : private NonCopyable {
 public:
  // 线程池配置。
  struct Options {
    // 工作线程数。必须 >= 1。
    std::size_t thread_count = 4;
    // 有界队列容量上限。0 表示无界（**不推荐**，会退化成内存堆爆）。
    std::size_t max_queue_size = 10000;
    // 线程名前缀（Windows 上用于调试器显示，POSIX 上设为 pthread name）。
    std::string name = "qzrpc-worker";
    // 队列满时是否让提交线程自己执行任务（"调用者运行"背压策略）。
    // true：牺牲该线程的响应性换取不丢任务；false：返回 kRejected 让上层回 OVERLOAD。
    // 默认 false，因为服务端 IO 线程绝不能被业务逻辑占用。
    bool allow_caller_run_on_reject = false;
  };

  // 提交结果。
  enum class SubmitResult {
    kOk = 0,       // 已入队
    kRejected = 1, // 队列已满，被拒绝
    kStopped = 2,  // 线程池已停止
  };

  // 构造。不启动线程，需显式 Start()。
  explicit ThreadPool(const Options& options);
  ~ThreadPool();

  // 启动全部工作线程。重复调用是幂等的。
  void Start();

  // 停止。
  // 参数：drain - true 表示把队列里已有任务执行完再退出；false 表示立即丢弃并退出。
  // 语义：阻塞直到所有工作线程 join 完成。重复调用幂等。
  void Stop(bool drain = true);

  // 提交一个任务。**线程安全**。
  // 参数：task - 无参无返回值闭包；为空时直接返回 kStopped。
  // 返回：见 SubmitResult。
  SubmitResult Submit(std::function<void()> task);

  // 当前排队等待执行的任务数。线程安全。
  std::size_t queue_size() const;

  // 配置的工作线程数。
  std::size_t thread_count() const noexcept { return options_.thread_count; }

  // 是否处于运行中（Start 之后、Stop 完成之前）。
  bool running() const noexcept;

  // 是否已停止。
  bool stopped() const noexcept;

  // 累计提交/完成/拒绝数（metrics 用）。线程安全。
  std::uint64_t submitted_total() const noexcept;
  std::uint64_t completed_total() const noexcept;
  std::uint64_t rejected_total() const noexcept;

 private:
  // 工作线程主循环。
  void WorkerLoop(std::size_t index);

  Options options_;

  mutable std::mutex mutex_;
  std::condition_variable task_cv_;   // 有新任务 / 收到停止信号
  std::condition_variable idle_cv_;   // 队列空或全部线程退出

  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> threads_;

  bool started_{false};
  bool stopping_{false};
  bool drain_on_stop_{true};
  std::size_t active_workers_{0};

  std::atomic<std::uint64_t> submitted_total_{0};
  std::atomic<std::uint64_t> completed_total_{0};
  std::atomic<std::uint64_t> rejected_total_{0};
};

}  // namespace net
}  // namespace qzrpc
