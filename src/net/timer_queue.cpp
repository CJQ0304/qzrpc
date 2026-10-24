// 轻舟 RPC (Qingzhou RPC)
// src/net/timer_queue.cpp
//
// 职责：单线程最小堆定时器。
//
// 关键不变量（维护好这两条，本文件就不会出竞态）：
//   1) 回调**永不**在持锁状态下执行（否则回调里调 Cancel/AddTimer 立刻自死锁）；
//   2) 取消是惰性的：Cancel 只往 cancelled_ 插 id；已在堆里的条目在"弹出时"被丢弃。
//
// 关于「NextExpirationMs 跳过已取消项」的实现说明：
//   std::priority_queue 只暴露堆顶，无法在 O(1) 内看到堆顶之后的元素。由于已取消条目的
//   due_ms 一定是堆里最小的（它在堆顶才会挡住真正的堆顶），"不跳过"也不会漏掉任何真实
//   定时器——最多是提前醒来一次，然后由 RunExpired 把它丢掉。因此本实现在 RunExpired
//   开头统一清理"堆顶的已取消条目"，而不是在 NextExpirationMs 里做（那个函数是 const，
//   无法合法地修改堆）。这样既满足语义，也避免了 const_cast 改堆的未定义行为。

#include "net/timer_queue.h"

#include <utility>

#include "common/time_util.h"

namespace qzrpc {
namespace net {

TimerQueue::TimerQueue() = default;

TimerQueue::~TimerQueue() {
  // 清空堆时必须先释放回调（std::function 的析构可能在任意线程做资源释放），
  // 这里不持锁执行：析构期不可能有其它线程合法访问本对象。
  std::priority_queue<Entry, std::vector<Entry>, EntryGreater> empty;
  std::swap(heap_, empty);
}

TimerId TimerQueue::AddTimer(std::uint64_t delay_ms, TimerCallback cb) {
  if (!cb) return 0;
  const TimerId id = next_id_.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t due = TimeUtil::NowMs() + delay_ms;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry entry;
    entry.due_ms = due;
    entry.interval_ms = 0;
    entry.id = id;
    entry.cb = std::move(cb);
    heap_.push(std::move(entry));
  }
  return id;
}

TimerId TimerQueue::AddRepeatingTimer(std::uint64_t interval_ms, TimerCallback cb) {
  if (!cb) return 0;
  // interval 为 0 会退化成"每轮循环触发一次"的忙循环，这里强制至少 1ms。
  const std::uint64_t interval = interval_ms == 0 ? 1 : interval_ms;
  const TimerId id = next_id_.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t due = TimeUtil::NowMs() + interval;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry entry;
    entry.due_ms = due;
    entry.interval_ms = interval;
    entry.id = id;
    entry.cb = std::move(cb);
    heap_.push(std::move(entry));
  }
  return id;
}

bool TimerQueue::Cancel(TimerId id) {
  if (id == 0) return false;
  // 比当前发号器还大的 id 不可能存在，直接判定为无效。
  if (id >= next_id_.load(std::memory_order_relaxed)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  cancelled_.insert(id);
  return true;
}

int TimerQueue::RunExpired(std::uint64_t now_ms) {
  std::vector<Entry> expired;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // 第一段：把堆顶的已取消条目清掉（无论是否到期）。它们挡住真实堆顶时
    // 会让 NextExpirationMs 提前触发，主动清理可以避免这种无意义的唤醒。
    while (!heap_.empty() && cancelled_.count(heap_.top().id) != 0) {
      const TimerId dead_id = heap_.top().id;
      heap_.pop();
      cancelled_.erase(dead_id);
    }
    // 第二段：取走全部到期条目。priority_queue::top() 返回 const 引用，
    // 因此这里显式拷一份再 pop（回调和周期性重新入堆都需要拥有这个 Entry）。
    while (!heap_.empty() && heap_.top().due_ms <= now_ms) {
      Entry entry = heap_.top();
      heap_.pop();
      if (cancelled_.erase(entry.id) != 0) continue;  // 已被取消，丢弃
      expired.push_back(std::move(entry));
    }
  }

  int executed = 0;
  for (Entry& entry : expired) {
    if (entry.cb) {
      entry.cb();
      ++executed;
    }
    if (entry.interval_ms == 0) continue;  // 一次性定时器：回调完即释放
    // 周期性定时器：用同一个 id 重新入堆（这样一次 Cancel 就能停掉整条链）。
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.erase(entry.id) != 0) continue;  // 回调里已经 Cancel 掉了
    entry.due_ms = TimeUtil::NowMs() + entry.interval_ms;
    heap_.push(std::move(entry));
  }
  return executed;
}

int TimerQueue::NextExpirationMs(std::uint64_t now_ms) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (heap_.empty()) return -1;
  const std::uint64_t due = heap_.top().due_ms;
  if (due <= now_ms) return 0;
  const std::uint64_t delta = due - now_ms;
  // 转成 int：超过 INT_MAX 的间隔没有意义（EventLoop 会被唤醒信号打断），做饱和处理。
  const std::uint64_t kMaxWait = 24ULL * 60ULL * 60ULL * 1000ULL;
  if (delta > kMaxWait) return static_cast<int>(kMaxWait);
  return static_cast<int>(delta);
}

std::size_t TimerQueue::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // 头文件已声明这是"上限估计"：已取消但还没被弹出的条目也算在内。
  return heap_.size();
}

}  // namespace net
}  // namespace qzrpc
