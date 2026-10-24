// 轻舟 RPC (Qingzhou RPC)
// src/net/timer_queue.h
//
// 职责：单线程定时器。用最小堆（std::priority_queue）实现，支持一次性与周期性定时器、按 id 取消。
//
// 设计要点：
//   * 时间基准是单调时钟毫秒（steady clock），不受墙钟调整影响；
//   * 取消不真正从堆里删除（惰性删除）：Cancel 只往取消集合里加 id，弹出时跳过。
//     这样 Cancel 的复杂度是 O(1)，且不会破坏堆结构；
//   * 周期性定时器触发后**用同一个 id 重新入堆**，因此一次 Cancel 能停掉整条链；
//   * 回调在**不持锁**的状态下执行（否则回调里调 Cancel 会自死锁）。
//
// 线程安全：AddTimer/Cancel/Size 线程安全（内部加锁）；
//           RunExpired/NextExpirationMs 只能在 EventLoop 线程调用。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>

namespace qzrpc {
namespace net {

// 定时器句柄。0 表示无效。
using TimerId = std::uint64_t;

// 定时器到期回调。禁止在回调里做阻塞操作（会卡住整个 EventLoop）。
using TimerCallback = std::function<void()>;

// 最小堆定时器队列。
class TimerQueue {
 public:
  TimerQueue();
  ~TimerQueue();

  // 添加一次性定时器。
  // 参数：delay_ms - 延迟毫秒（0 表示下一轮循环立刻触发）；cb - 到期回调，不能为空。
  // 返回：定时器句柄，可用于 Cancel。线程安全。
  TimerId AddTimer(std::uint64_t delay_ms, TimerCallback cb);

  // 添加周期性定时器。首次触发在 interval_ms 之后。
  // 返回：定时器句柄。线程安全。
  TimerId AddRepeatingTimer(std::uint64_t interval_ms, TimerCallback cb);

  // 取消定时器。对已触发的一次性定时器或已取消的 id 调用是安全的空操作。
  // 返回：true 表示该 id 此前是活跃的。线程安全。
  bool Cancel(TimerId id);

  // 取出并执行所有到期定时器。
  // 参数：now_ms - 当前单调毫秒（由事件循环统一传入，避免一次循环里取多次时钟）。
  // 返回：本次真正执行的回调数量。
  // 约束：只能在 EventLoop 线程调用。
  int RunExpired(std::uint64_t now_ms);

  // 距离下一个定时器到期还有多少毫秒。
  // 返回：>=0 的毫秒数；-1 表示没有定时器（此时 Wait 应使用永久阻塞或外部指定的超时）。
  int NextExpirationMs(std::uint64_t now_ms) const;

  // 活跃（未被取消且未触发）定时器数量上限估计。
  std::size_t Size() const;

 private:
  // 堆元素。due_ms 为绝对到期时间。
  struct Entry {
    std::uint64_t due_ms = 0;
    std::uint64_t interval_ms = 0;  // 0 表示一次性
    TimerId id = 0;
    TimerCallback cb;
  };

  // 小顶堆比较器（std::priority_queue 默认是大顶堆）。
  struct EntryGreater {
    bool operator()(const Entry& a, const Entry& b) const noexcept { return a.due_ms > b.due_ms; }
  };

  mutable std::mutex mutex_;
  std::priority_queue<Entry, std::vector<Entry>, EntryGreater> heap_;
  std::unordered_set<TimerId> cancelled_;
  std::atomic<TimerId> next_id_{1};
};

}  // namespace net
}  // namespace qzrpc
