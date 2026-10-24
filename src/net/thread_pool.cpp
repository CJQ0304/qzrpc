// 轻舟 RPC (Qingzhou RPC)
// src/net/thread_pool.cpp
//
// 职责：固定线程数 + 有界队列的业务线程池。
//
// 设计要点：
//   * "容量检查"与"入队"在同一把锁内完成，因此 Submit 的返回值就是最终结论，
//     不存在"检查通过但入队失败"的窗口（这是相对 phxrpc 两段式判断的改进）；
//   * 队列满时默认返回 kRejected（不阻塞 IO 线程），上层据此回 OVERLOAD 帧；
//   * Stop(drain=false) 会清空队列，保证停机不会被积压任务拖住。
//
// 语义定义（写在这里避免误用）：
//   * started_：线程已经启动；stopping_：已请求停止（running() = started_ && !stopping_）；
//   * active_workers_：**正在执行任务**的线程数（不含阻塞在条件变量上的）；
//   * idle_cv_：在 active_workers_ == 0 且 queue_ 为空时广播，供 Stop 等待收敛。

#include "net/thread_pool.h"

#include <utility>

namespace qzrpc {
namespace net {

namespace {

// 给工作线程起名（便于调试器/性能分析器识别）。
//
// 实现说明：Windows 上唯一"不依赖新 API"的起名方式是向调试器抛 0x406D1388 异常，
// 但那需要 __try/__except。MSVC 在 /EHsc 下禁止"需要对象展开的函数里使用 SEH"
// （编译错误 C2712），而本函数带 std::string 参数，必然触发。为了不引入 /EHa 这种
// 全局妥协，这里保留空实现：线程名只是观测便利，不影响任何正确性。
// （后续若需要，可改用 LoadLibrary("Kernel32") + SetThreadDescription 动态解析。）
void SetCurrentThreadName(const std::string& name) { (void)name; }

}  // namespace

ThreadPool::ThreadPool(const Options& options) : options_(options) {
  if (options_.thread_count == 0) options_.thread_count = 1;
}

ThreadPool::~ThreadPool() {
  // drain = false：析构时不再执行业务任务（业务闭包可能捕获已经析构的对象）。
  Stop(false);
}

void ThreadPool::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) return;
  started_ = true;
  stopping_ = false;
  drain_on_stop_ = true;
  threads_.reserve(options_.thread_count);
  for (std::size_t i = 0; i < options_.thread_count; ++i) {
    threads_.emplace_back([this, i] { WorkerLoop(i); });
  }
}

void ThreadPool::Stop(bool drain) {
  std::vector<std::thread> threads_to_join;
  // dropped 声明在锁外：队列里的 std::function 析构可能释放业务闭包持有的资源，
  // 绝不能在持锁状态下做（也可能回调回 ThreadPool::Submit 造成自死锁）。
  std::deque<std::function<void()>> dropped;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) {
      // 从未启动：只把状态置成 stopped，避免后续 Submit 再入队。
      stopping_ = true;
      return;
    }
    if (stopping_ && threads_.empty()) return;  // 幂等
    stopping_ = true;
    drain_on_stop_ = drain;
    if (!drain) dropped.swap(queue_);
    threads_to_join.swap(threads_);
  }
  task_cv_.notify_all();
  for (std::thread& worker : threads_to_join) {
    if (worker.joinable()) worker.join();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
  }
}

void ThreadPool::WorkerLoop(std::size_t index) {
  (void)index;
  SetCurrentThreadName(options_.name);
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      task_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        // 只有 stopping_ 能让"队列为空且被唤醒"成立 → 退出。
        // 退出前广播一次，让正在等"全部空闲"的 Stop 观察到收敛。
        idle_cv_.notify_all();
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
      ++active_workers_;
    }
    if (task) task();
    completed_total_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_workers_;
      if (active_workers_ == 0 && queue_.empty()) idle_cv_.notify_all();
    }
  }
}

ThreadPool::SubmitResult ThreadPool::Submit(std::function<void()> task) {
  if (!task) return SubmitResult::kStopped;
  bool run_on_caller = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopping_) return SubmitResult::kStopped;
    if (options_.max_queue_size > 0 && queue_.size() >= options_.max_queue_size) {
      rejected_total_.fetch_add(1, std::memory_order_relaxed);
      if (!options_.allow_caller_run_on_reject) return SubmitResult::kRejected;
      run_on_caller = true;
    } else {
      queue_.push_back(std::move(task));
      submitted_total_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (run_on_caller) {
    // "调用者运行"策略：用提交线程的响应性换"绝不丢任务"。
    // 服务端默认关闭该策略（IO 线程绝不能被业务逻辑占用）。
    task();
    completed_total_.fetch_add(1, std::memory_order_relaxed);
    return SubmitResult::kOk;
  }
  task_cv_.notify_one();
  return SubmitResult::kOk;
}

std::size_t ThreadPool::queue_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

bool ThreadPool::running() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return started_ && !stopping_;
}

bool ThreadPool::stopped() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return stopping_;
}

std::uint64_t ThreadPool::submitted_total() const noexcept {
  return submitted_total_.load(std::memory_order_relaxed);
}

std::uint64_t ThreadPool::completed_total() const noexcept {
  return completed_total_.load(std::memory_order_relaxed);
}

std::uint64_t ThreadPool::rejected_total() const noexcept {
  return rejected_total_.load(std::memory_order_relaxed);
}

}  // namespace net
}  // namespace qzrpc
