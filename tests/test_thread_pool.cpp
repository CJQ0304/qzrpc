// 轻舟 RPC (Qingzhou RPC)
// tests/test_thread_pool.cpp
//
// 有界任务队列线程池的契约测试（T-09）。
//
// 为什么线程池比它看起来更需要测：
//   它是服务端唯一跑业务代码的地方，所以它的每个边界都直接变成线上行为：
//     * 队列满时"拒绝"还是"让 IO 线程自己跑"——后者会把 reactor 拖死；
//     * Stop 时"排空"还是"丢弃"——前者可能让停机卡住，后者可能丢在飞请求；
//     * Start/Stop 的幂等性——重复调用是析构、信号回调、配置热更新都会做的事；
//     * 计数一致性——submitted = completed + queued + rejected 是运维看板唯一的口径。
//   这些错了都不会崩，只会"偶发变慢 / 偶发丢请求 / 指标对不上"。
//
// ★ 曾经的一个真实缺口，现在已经被钉住：任务体抛异常会让工作线程带着异常离开
//   WorkerLoop → std::terminate → 整个进程消失（实测退出码 0xC0000409）。
//   修复前这条**无法在进程内断言**（能断言的只有"进程还在"，而进程已经不在了），
//   所以当时的做法是：留空 + 把实证报告给 src/net 的 owner。
//   try/catch 落地后，缺口变成 TestThrowingTaskIsContained 里的断言：
//   进程活着 + 后续任务照跑 + completed 计数包含抛异常的那一次。
//
// 断言里不用 sleep 等"大概跑完了"，一律用条件变量/计数等待：
// sleep 在共享 CI 机器上会变成偶发红，而偶发红会被当成噪音忽略。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "net/thread_pool.h"
#include "test_main.h"

namespace {

using qzrpc::net::ThreadPool;

// 等某个条件成立，最长等 timeout_ms。返回是否等到。
// 用途：把"任务已经开始执行"这类事件变成确定性等待，而不是 sleep 一个魔法毫秒数。
bool WaitFor(const std::function<bool()>& predicate, int timeout_ms = 5000) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

ThreadPool::Options MakeOptions(std::size_t threads, std::size_t queue) {
  ThreadPool::Options options;
  options.thread_count = threads;
  options.max_queue_size = queue;
  return options;
}

// ===========================================================================
// A. 生命周期
// ===========================================================================

void TestLifecycleAndIdempotency() {
  ThreadPool pool(MakeOptions(2, 16));
  QZ_CHECK_EQ(pool.thread_count(), std::size_t{2});
  // 构造后不启动：running 为 false，Submit 返回 kStopped（而不是悄悄入队后永不执行）。
  QZ_CHECK(!pool.running());
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([] {})),
              static_cast<int>(ThreadPool::SubmitResult::kStopped));

  pool.Start();
  QZ_CHECK(pool.running());

  // 重复 Start 必须幂等：不重复创建线程、不重置计数。
  pool.Start();
  pool.Start();
  QZ_CHECK(pool.running());
  QZ_CHECK_EQ(pool.thread_count(), std::size_t{2});

  // 空任务：契约是直接返回 kStopped（一个空的 std::function 调起来会抛 bad_function_call，
  // 与其在 worker 里炸掉，不如在入口拒绝）。
  QZ_CHECK_EQ(static_cast<int>(pool.Submit(std::function<void()>())),
              static_cast<int>(ThreadPool::SubmitResult::kStopped));

  pool.Stop(true);
  QZ_CHECK(!pool.running());
  QZ_CHECK(pool.stopped());
  // 重复 Stop 幂等，且不能挂住。
  pool.Stop(true);
  pool.Stop(false);
  // 停止后 Submit 必须被拒绝，不能"入队成功但永不执行"。
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([] {})),
              static_cast<int>(ThreadPool::SubmitResult::kStopped));

  // 从未 Start 过的池子 Stop 也不能挂住（析构路径会走这里）。
  ThreadPool never_started(MakeOptions(1, 4));
  never_started.Stop(true);
  QZ_CHECK(never_started.stopped());
}

// thread_count = 0 是配置错误（ServerOptions 会拦），但线程池自己也不能崩：
// 实现把它夹到 1。这里钉住"不崩 + 至少一个线程能跑任务"。
void TestZeroThreadCountIsCoerced() {
  ThreadPool pool(MakeOptions(0, 8));
  QZ_CHECK_EQ(pool.thread_count(), std::size_t{1});
  pool.Start();
  std::atomic<int> done{0};
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&done] { done.fetch_add(1); })),
              static_cast<int>(ThreadPool::SubmitResult::kOk));
  QZ_CHECK(WaitFor([&done] { return done.load() == 1; }));
  pool.Stop(true);
}

// ===========================================================================
// B. 有界队列：拒绝 vs 调用者运行
// ===========================================================================

// 队列满时**默认**返回 kRejected。服务端据此回 OVERLOAD 帧，而不是让 IO 线程
// 自己去跑业务（那样 reactor 会在业务耗时里失去响应，一条慢查询能拖死整个连接组）。
void TestQueueFullRejects() {
  // 1 个 worker + 队列容量 2：把 worker 用门闩卡住，队列就能被填满。
  ThreadPool pool(MakeOptions(1, 2));
  pool.Start();

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_open = false;
  std::atomic<bool> worker_entered{false};

  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&] {
                 worker_entered.store(true);
                 std::unique_lock<std::mutex> lock(gate_mutex);
                 gate_cv.wait(lock, [&gate_open] { return gate_open; });
               })),
              static_cast<int>(ThreadPool::SubmitResult::kOk));
  QZ_CHECK(WaitFor([&worker_entered] { return worker_entered.load(); }));

  // 队列容量 2 -> 第 3 个开始必须被拒。
  std::atomic<int> queued_ran{0};
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&queued_ran] { queued_ran.fetch_add(1); })),
              static_cast<int>(ThreadPool::SubmitResult::kOk));
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&queued_ran] { queued_ran.fetch_add(1); })),
              static_cast<int>(ThreadPool::SubmitResult::kOk));
  QZ_CHECK_EQ(pool.queue_size(), std::size_t{2});

  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&queued_ran] { queued_ran.fetch_add(1); })),
              static_cast<int>(ThreadPool::SubmitResult::kRejected));
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&queued_ran] { queued_ran.fetch_add(1); })),
              static_cast<int>(ThreadPool::SubmitResult::kRejected));
  QZ_CHECK_EQ(pool.rejected_total(), std::uint64_t{2});
  // 被拒绝的任务**不能**被执行，也不能留在队列里占位。
  QZ_CHECK_EQ(pool.queue_size(), std::size_t{2});

  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    gate_open = true;
  }
  gate_cv.notify_all();
  QZ_CHECK(WaitFor([&queued_ran] { return queued_ran.load() == 2; }));
  QZ_CHECK(WaitFor([&pool] { return pool.queue_size() == 0; }));

  // 计数口径：submitted 只统计成功入队的；rejected 单独一列。
  QZ_CHECK_EQ(pool.submitted_total(), std::uint64_t{3});   // 门闩任务 + 2 个排队任务
  QZ_CHECK_EQ(pool.completed_total(), std::uint64_t{3});
  QZ_CHECK_EQ(pool.rejected_total(), std::uint64_t{2});
  pool.Stop(true);
}

// allow_caller_run_on_reject = true 时，队列满**不丢任务**，改由提交线程执行。
// 断言两件事：返回值仍是 kOk（调用方视角"任务肯定会被执行"），
// 且任务确实运行在**提交线程**上（不是偷偷塞进队列）。
void TestCallerRunsOnReject() {
  ThreadPool::Options options = MakeOptions(1, 1);
  options.allow_caller_run_on_reject = true;
  ThreadPool pool(options);
  pool.Start();

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_open = false;
  std::atomic<bool> worker_entered{false};
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&] {
                 worker_entered.store(true);
                 std::unique_lock<std::mutex> lock(gate_mutex);
                 gate_cv.wait(lock, [&gate_open] { return gate_open; });
               })),
              static_cast<int>(pool.SubmitResult::kOk));
  QZ_CHECK(WaitFor([&worker_entered] { return worker_entered.load(); }));

  // 把容量 1 的队列填满。
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([] {})),
              static_cast<int>(pool.SubmitResult::kOk));
  QZ_CHECK_EQ(pool.queue_size(), std::size_t{1});

  // 再来一个：策略是"调用者运行"，所以本线程会立刻执行它。
  const std::thread::id caller_id = std::this_thread::get_id();
  std::thread::id ran_on;
  const ThreadPool::SubmitResult result = pool.Submit([&ran_on] {
    ran_on = std::this_thread::get_id();
  });
  QZ_CHECK_EQ(static_cast<int>(result), static_cast<int>(ThreadPool::SubmitResult::kOk));
  QZ_CHECK(ran_on == caller_id);  // 确实在提交线程上跑的

  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    gate_open = true;
  }
  gate_cv.notify_all();
  QZ_CHECK(WaitFor([&pool] { return pool.queue_size() == 0; }));
  pool.Stop(true);
}

// ===========================================================================
// C. Stop 的两种语义
// ===========================================================================

// Stop(drain=true)：队列里已有的任务**必须执行完**。
// 这是停机时"不丢在飞请求"的底座 —— 谁把 drain 改成 false，这条会红。
void TestStopDrainsQueuedTasks() {
  ThreadPool pool(MakeOptions(1, 64));
  pool.Start();

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_open = false;
  std::atomic<bool> worker_entered{false};
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&] {
                 worker_entered.store(true);
                 std::unique_lock<std::mutex> lock(gate_mutex);
                 gate_cv.wait(lock, [&gate_open] { return gate_open; });
               })),
              static_cast<int>(pool.SubmitResult::kOk));
  QZ_CHECK(WaitFor([&worker_entered] { return worker_entered.load(); }));

  std::atomic<int> ran{0};
  constexpr int kTasks = 20;
  for (int i = 0; i < kTasks; ++i) {
    QZ_CHECK_EQ(static_cast<int>(pool.Submit([&ran] { ran.fetch_add(1); })),
                static_cast<int>(pool.SubmitResult::kOk));
  }

  // Stop 会阻塞直到 join 完成，所以必须先放开门闩 —— 用一个独立线程调用 Stop。
  std::thread stopper([&pool] { pool.Stop(true); });
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    gate_open = true;
  }
  gate_cv.notify_all();
  stopper.join();

  QZ_CHECK_EQ(ran.load(), kTasks);  // 20 个排队任务一个不少
  QZ_CHECK_EQ(pool.completed_total(), static_cast<std::uint64_t>(kTasks + 1));
  QZ_CHECK(!pool.running());
}

// Stop(drain=false)：队列里的任务**丢弃**，且丢弃发生在锁外
// （任务闭包析构可能释放业务资源、甚至回调 Submit，持锁析构会自死锁）。
void TestStopDropDiscardsQueuedTasks() {
  ThreadPool pool(MakeOptions(1, 64));
  pool.Start();

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_open = false;
  std::atomic<bool> worker_entered{false};
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([&] {
                 worker_entered.store(true);
                 std::unique_lock<std::mutex> lock(gate_mutex);
                 gate_cv.wait(lock, [&gate_open] { return gate_open; });
               })),
              static_cast<int>(pool.SubmitResult::kOk));
  QZ_CHECK(WaitFor([&worker_entered] { return worker_entered.load(); }));

  std::atomic<int> ran{0};
  for (int i = 0; i < 10; ++i) {
    QZ_CHECK_EQ(static_cast<int>(pool.Submit([&ran] { ran.fetch_add(1); })),
                static_cast<int>(pool.SubmitResult::kOk));
  }

  std::thread stopper([&pool] { pool.Stop(false); });
  // ★ 顺序很关键：**先**等 Stop 真的把队列摘走（stopping_ 与 swap 在同一个临界区里完成，
  //   所以观察到 stopped() == true 就等于队列已经空了），**再**放开门闩。
  //   反过来做（先放门闩再 Stop）会让 worker 醒来后看到队列还满着，把 10 个"本该丢弃"的
  //   任务全跑完 —— 这不是实现 bug，而是测试自己制造了竞态。我第一版就写反了，
  //   ran.load() 是 10 而不是 0，正好是这条注释要提醒的事。
  QZ_CHECK(WaitFor([&pool] { return pool.stopped(); }));
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    gate_open = true;
  }
  gate_cv.notify_all();
  stopper.join();

  QZ_CHECK_EQ(ran.load(), 0);            // 全部丢弃
  QZ_CHECK_EQ(pool.queue_size(), std::size_t{0});
  QZ_CHECK(!pool.running());
}

// ===========================================================================
// D. 吞吐与计数一致性
// ===========================================================================

// ★ 任务体抛异常必须就地收口。
//
// 这条用例的价值不在"跑通"，而在它**曾经跑不通**：修复前 `WorkerLoop` 里是裸的 `task()`，
// 异常穿出线程函数 → std::terminate → 进程 fail-fast（实测退出码 0xC0000409，
// "任务跑完之后的那行 printf"根本没机会执行）。
// 线上形态是"某个业务 handler 里一句 std::stoi 抛了，整个 server 进程消失"——单请求的错误
// 升级成了进程级事件。修复后这里断言三件事：进程还活着、后续任务照跑、计数口径不破。
void TestThrowingTaskIsContained() {
  ThreadPool pool(MakeOptions(2, 32));
  pool.Start();

  std::atomic<int> after{0};

  // 1) std::exception 子类
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([] { throw std::runtime_error("boom"); })),
              static_cast<int>(ThreadPool::SubmitResult::kOk));
  // 2) 非 std::exception（骨架里最容易漏掉的那一类 catch）
  QZ_CHECK_EQ(static_cast<int>(pool.Submit([] { throw 42; })),
              static_cast<int>(ThreadPool::SubmitResult::kOk));
  // 3) 抛异常之后的正常任务必须照跑（worker 不能因为前面炸过就少一个）
  constexpr int kAfter = 8;
  for (int i = 0; i < kAfter; ++i) {
    QZ_CHECK_EQ(static_cast<int>(pool.Submit([&after] { after.fetch_add(1); })),
                static_cast<int>(ThreadPool::SubmitResult::kOk));
  }

  QZ_CHECK(WaitFor([&after] { return after.load() == kAfter; }, 10000));
  QZ_CHECK_EQ(after.load(), kAfter);

  // 计数口径：抛异常的任务 **也** 算"已执行完"（它确实跑过了），
  // 否则 submitted == completed + queued + rejected 这条运维口径会在异常路径上破掉。
  QZ_CHECK_EQ(pool.submitted_total(), static_cast<std::uint64_t>(2 + kAfter));
  QZ_CHECK(WaitFor([&pool] { return pool.completed_total() == 2 + kAfter; }, 10000));
  QZ_CHECK_EQ(pool.completed_total(), static_cast<std::uint64_t>(2 + kAfter));
  QZ_CHECK_EQ(pool.rejected_total(), std::uint64_t{0});
  pool.Stop(true);
}

void TestManyTasksAllComplete() {
  constexpr std::size_t kThreads = 4;
  constexpr int kTasks = 500;
  ThreadPool pool(MakeOptions(kThreads, 1024));
  pool.Start();

  std::atomic<int> ran{0};
  std::atomic<long long> sum{0};
  for (int i = 0; i < kTasks; ++i) {
    QZ_CHECK_EQ(static_cast<int>(pool.Submit([&ran, &sum, i] {
                   ran.fetch_add(1);
                   sum.fetch_add(i);
                 })),
                static_cast<int>(pool.SubmitResult::kOk));
  }
  QZ_CHECK(WaitFor([&ran] { return ran.load() == kTasks; }, 10000));
  QZ_CHECK_EQ(ran.load(), kTasks);
  QZ_CHECK_EQ(sum.load(), static_cast<long long>(kTasks) * (kTasks - 1) / 2);
  QZ_CHECK_EQ(pool.submitted_total(), static_cast<std::uint64_t>(kTasks));
  QZ_CHECK_EQ(pool.completed_total(), static_cast<std::uint64_t>(kTasks));
  QZ_CHECK_EQ(pool.rejected_total(), std::uint64_t{0});
  QZ_CHECK_EQ(pool.queue_size(), std::size_t{0});
  pool.Stop(true);
}

// max_queue_size = 0 表示无界。这条行为容易在"把 0 当成非法值"的改动里被破坏，
// 而它恰恰是 example 之外所有人都用它做批量任务的原语。
void TestUnboundedQueueAcceptsEverything() {
  ThreadPool pool(MakeOptions(2, 0));
  pool.Start();

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_open = false;
  std::atomic<int> workers_entered{0};

  auto gate_task = [&] {
    workers_entered.fetch_add(1);
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate_cv.wait(lock, [&gate_open] { return gate_open; });
  };
  // 两个 worker 都卡住，队列才能真正堆积。
  QZ_CHECK_EQ(static_cast<int>(pool.Submit(gate_task)),
              static_cast<int>(pool.SubmitResult::kOk));
  QZ_CHECK_EQ(static_cast<int>(pool.Submit(gate_task)),
              static_cast<int>(pool.SubmitResult::kOk));
  QZ_CHECK(WaitFor([&workers_entered] { return workers_entered.load() == 2; }));

  constexpr int kTasks = 200;  // 远超任何合理的容量上限
  std::atomic<int> ran{0};
  for (int i = 0; i < kTasks; ++i) {
    QZ_CHECK_EQ(static_cast<int>(pool.Submit([&ran] { ran.fetch_add(1); })),
                static_cast<int>(pool.SubmitResult::kOk));
  }
  QZ_CHECK_EQ(pool.rejected_total(), std::uint64_t{0});
  QZ_CHECK_EQ(pool.queue_size(), static_cast<std::size_t>(kTasks));

  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    gate_open = true;
  }
  gate_cv.notify_all();
  QZ_CHECK(WaitFor([&ran] { return ran.load() == kTasks; }, 10000));
  QZ_CHECK_EQ(ran.load(), kTasks);
  pool.Stop(true);
}

// 销毁时不得执行残留任务：业务闭包可能捕获已经析构的对象，
// 所以析构走 Stop(false)（见 .cpp）。这里用一个"对象已析构后再执行会读到毒值"的场景验证。
void TestDestructorDoesNotRunPendingTasks() {
  std::atomic<int> ran{0};
  {
    ThreadPool pool(MakeOptions(1, 8));
    pool.Start();
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool gate_open = false;
    std::atomic<bool> worker_entered{false};
    QZ_CHECK_EQ(static_cast<int>(pool.Submit([&] {
                   worker_entered.store(true);
                   std::unique_lock<std::mutex> lock(gate_mutex);
                   gate_cv.wait(lock, [&gate_open] { return gate_open; });
                 })),
                static_cast<int>(pool.SubmitResult::kOk));
    QZ_CHECK(WaitFor([&worker_entered] { return worker_entered.load(); }));

    // 3 个排队任务：析构时必须被丢弃。
    for (int i = 0; i < 3; ++i) {
      QZ_CHECK_EQ(static_cast<int>(pool.Submit([&ran] { ran.fetch_add(1); })),
                  static_cast<int>(pool.SubmitResult::kOk));
    }
    {
      std::lock_guard<std::mutex> lock(gate_mutex);
      gate_open = true;
    }
    gate_cv.notify_all();
    // 不等它们跑完就析构：析构里的 Stop(false) 会先清队列再 join。
  }
  QZ_CHECK(ran.load() < 3);
}

}  // namespace

int main() {
  // A. 生命周期
  TestLifecycleAndIdempotency();
  TestZeroThreadCountIsCoerced();

  // B. 有界队列
  TestQueueFullRejects();
  TestCallerRunsOnReject();

  // C. Stop 语义
  TestStopDrainsQueuedTasks();
  TestStopDropDiscardsQueuedTasks();

  // D. 吞吐与计数
  TestThrowingTaskIsContained();
  TestManyTasksAllComplete();
  TestUnboundedQueueAcceptsEverything();
  TestDestructorDoesNotRunPendingTasks();

  return qztest::Summary("test_thread_pool");
}
