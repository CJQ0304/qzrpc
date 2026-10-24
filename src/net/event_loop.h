// 轻舟 RPC (Qingzhou RPC)
// src/net/event_loop.h
//
// 职责：一个 EventLoop = 一个线程的运行环境 = 一个 Poller + 一个 TimerQueue + 一个任务队列。
//
// 线程模型（关键契约）：
//   * 每个 EventLoop 只能由**一个线程**调用 Loop()，该线程称为"loop 线程"；
//   * Channel 的所有操作（EnableReading/EnableWriting/Remove）必须在 loop 线程执行；
//   * 其他线程若要操作 Channel，必须通过 RunInLoop/QueueInLoop 把闭包转投到 loop 线程；
//   * 只有 RunInLoop/QueueInLoop/RunAfter/RunEvery/CancelTimer/Quit 是线程安全的。
//
// 唤醒机制：内部使用一对自连接的 UDP socket（127.0.0.1 随机端口）实现。
//   选它的原因：POSIX 的 socketpair/eventfd 与 Windows 的 CreateEvent 互不通用，
//   而"UDP 自连接 + 写 1 字节"在两端都用同一套代码，且能被 poll/WSAPoll 统一监听。
//
// 线程安全：见上文逐方法标注。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/noncopyable.h"
#include "net/poller.h"
#include "net/timer_queue.h"

namespace qzrpc {
namespace net {

class Channel;

// 事件循环。
class EventLoop : private NonCopyable {
 public:
  // 构造一个未运行的事件循环。构造期间会完成 Poller 与唤醒 socket 的创建。
  EventLoop();
  ~EventLoop();

  // 启动循环，当前线程成为 loop 线程，本函数阻塞直到 Quit() 被调用。
  // 约束：只能调用一次；不可重入。
  void Loop();

  // 请求退出循环（线程安全，可从任意线程调用）。
  // 语义：立即停止等待新事件，执行完当前一轮已就绪的回调后返回。
  void Quit();

  // 循环是否正在运行（Loop() 已进入且未返回）。线程安全。
  bool running() const noexcept;

  // 是否已收到 Quit 请求。线程安全。
  bool quit_requested() const noexcept;

  // 在 loop 线程执行 cb：若调用方就是 loop 线程则**立即同步执行**，
  // 否则等价于 QueueInLoop。线程安全。
  void RunInLoop(std::function<void()> cb);

  // 把 cb 放入任务队列，等本轮 IO 处理完再执行（保证不在调用栈里重入）。线程安全。
  void QueueInLoop(std::function<void()> cb);

  // 唤醒 loop 线程，使其立刻从 Wait 返回以处理任务队列。线程安全，可重复调用。
  void Wakeup();

  // 在 delay_ms 之后执行一次 cb。返回定时器句柄。线程安全。
  TimerId RunAfter(std::uint64_t delay_ms, TimerCallback cb);

  // 每 interval_ms 执行一次 cb，直到 CancelTimer。线程安全。
  TimerId RunEvery(std::uint64_t interval_ms, TimerCallback cb);

  // 取消定时器。线程安全。
  void CancelTimer(TimerId id);

  // 当前线程是否是本 loop 的 loop 线程。
  // 注意：Loop() 运行前该值尚未绑定，此时恒返回 false。
  bool IsInLoopThread() const noexcept;

  // loop 线程的 id（Loop() 运行前为空）。
  std::thread::id loop_thread_id() const noexcept;

  // 尚未执行的任务数（背压观测用）。线程安全。
  std::size_t pending_task_count() const noexcept;

  // 当前注册在 Poller 里的 fd 数量（调试用）。**只能在 loop 线程调用。**
  std::size_t registered_fd_count() const;

  // 唤醒次数统计（自检用，防止唤醒机制失效导致任务饥饿）。线程安全。
  std::uint64_t wakeup_count() const noexcept;

 private:
  friend class Channel;

  // Channel 注册/更新/注销事件。只能在 loop 线程调用（由 Channel 保证）。
  bool UpdateChannel(Channel* channel);
  void RemoveChannel(Channel* channel);

  // 执行任务队列里的全部任务。只在 loop 线程调用。
  void DoPendingTasks();

  // 处理唤醒 socket 上到达的字节并清空。
  void HandleWakeupRead();

  // 创建/销毁内部的唤醒 socket 对。构造函数与析构函数调用。
  bool CreateWakeupFds();
  void CloseWakeupFds() noexcept;

  std::unique_ptr<Poller> poller_;
  std::unique_ptr<TimerQueue> timer_queue_;

  // fd -> Channel 的映射，仅 loop 线程访问。Channel 析构或 Remove 时摘除。
  std::unordered_map<int, Channel*> channels_;

  // 内部唤醒用 fd 对：wakeup_send_fd_ 用于跨线程写，wakeup_recv_fd_ 注册在 Poller 上。
  int wakeup_send_fd_{-1};
  int wakeup_recv_fd_{-1};

  std::atomic<bool> quit_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> wakeup_count_{0};

  mutable std::mutex mutex_;
  std::vector<std::function<void()>> pending_tasks_;
  bool calling_pending_tasks_{false};

  std::thread::id loop_thread_id_{};
  std::atomic<bool> thread_bound_{false};
};

}  // namespace net
}  // namespace qzrpc
