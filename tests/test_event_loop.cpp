// 轻舟 RPC (Qingzhou RPC)
// tests/test_event_loop.cpp
//
// EventLoop 的退出契约（T-02 / L1）。
//
// 为什么这个文件必须存在：这里唯一的一条断言，守的是一个**能让整个进程消失**的缺陷形态。
//
//   旧实现里 `Loop()` 的第一行是 `quit_.store(false)`，看着像"让事件循环可重启"。
//   但 `Loop()` 跑在 **loop 线程**里，而 `Quit()` 由**别的线程**调用（Channel::Stop /
//   Server::Stop）。两者之间有真实窗口：线程已创建、还没被调度到 Loop 的第一行，此时
//   `Quit()` 已经把 quit_ 置 true 并 Wakeup()，随后 Loop() 一进来把标志抹回 false →
//   线程永不退出 → `Stop()` 里的 `join()` 永久挂死，连接池与 Channel 都已拆完，
//   没有任何东西还能叫醒它。
//
//   它在 e2e 里表现为"偶发 60s 超时"，取决于调度，**不是稳定红**；所以必须在单测里钉成
//   确定性的：`Quit()` 一定发生在 `Loop()` 之前，不需要任何时序运气。
//
// ★ 关于"挂死型"用例的写法（这个文件最重要的工程细节）：
//   普通写法 `std::thread t(...); t.join();` 在回归时会**永远不返回**，ctest 只会在 180s
//   后报一句 Timeout —— 它告诉你有东西挂了，但不告诉你是谁，而且整个用例的断言结果全部
//   丢失。所以这里统一走 `RunLoopWithWatchdog()`：
//     1. 先等一个上限；
//     2. 超时后**再 Quit 一次** —— 回归形态下这一刀才叫得醒（标志是被 Loop 抹掉的，
//        第二次 Quit 发生在 Loop 已经在跑之后）；
//     3. 仍然不返回才 detach 并判失败，让进程能干净退出、把前面所有断言打印出来。
//   于是同一个回归有两种可分辨的指纹：kLateQuit（等到第二次 Quit 才停 = 标志被抹掉）与
//   kStuck（彻底叫不醒）。both 都判失败，但看日志就知道是哪一种。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <thread>
#include <utility>

#include "net/event_loop.h"
#include "test_main.h"

namespace {

using qzrpc::net::EventLoop;

// Loop() 的三种退出形态。
enum class LoopExit {
  kOk,       // 在第一次等待内自行返回（正确）
  kLateQuit, // 只有补了第二次 Quit 才返回 → quit 标志被 Loop 抹掉过（回归指纹）
  kStuck,    // 怎么都不返回 → 线程被 detach，判失败
};

const char* ToString(LoopExit value) {
  switch (value) {
    case LoopExit::kOk: return "kOk";
    case LoopExit::kLateQuit: return "kLateQuit";
    case LoopExit::kStuck: return "kStuck";
  }
  return "?";
}

// 起一个线程跑 Loop()，带两级看门狗。见文件头的说明——这里的存在意义不是"多等一会儿"，
// 而是"回归时仍能把断言结果打出来"。
LoopExit RunLoopWithWatchdog(EventLoop& loop, int first_wait_ms, int second_wait_ms) {
  std::packaged_task<void()> task([&loop] { loop.Loop(); });
  std::future<void> finished = task.get_future();
  std::thread worker(std::move(task));

  if (finished.wait_for(std::chrono::milliseconds(first_wait_ms)) == std::future_status::ready) {
    worker.join();
    return LoopExit::kOk;
  }

  loop.Quit();
  if (finished.wait_for(std::chrono::milliseconds(second_wait_ms)) == std::future_status::ready) {
    worker.join();
    return LoopExit::kLateQuit;
  }

  worker.detach();
  return LoopExit::kStuck;
}

// 轮询等待（毫秒级），只在"另一线程应完成某事"的地方用。
// 注意：本文件刻意没有引入通用的 WaitUntil 帮助函数 —— 用不上（每条用例都靠 Quit 收尾，
// 不靠等待），而 /W4 会把未引用的匿名命名空间函数报成 C4505，在 /WX 下直接变红。

// ===========================================================================
// A. Quit 早于 Loop 调度：必须能停下来（唯一一条进程级回归锁）
// ===========================================================================

void TestQuitBeforeLoopStillStops() {
  EventLoop loop;

  // Loop 还没跑：running/quit 都是干净的，线程 id 还没绑定。
  QZ_CHECK(!loop.running());
  QZ_CHECK(!loop.quit_requested());
  QZ_CHECK(!loop.IsInLoopThread());
  QZ_CHECK(loop.loop_thread_id() == std::thread::id());

  // ★ 回归的现场条件：线程还不存在。
  loop.Quit();
  QZ_CHECK(loop.quit_requested());
  // Quit 幂等地唤醒（唤醒廉价，漏一次就是挂死），但**不**改变 running()。
  QZ_CHECK(loop.wakeup_count() > 0);
  QZ_CHECK(!loop.running());

  // 修复前：Loop() 一进来把标志抹成 false → 只能靠第二次 Quit 叫醒（kLateQuit）。
  const LoopExit outcome = RunLoopWithWatchdog(loop, 10000, 5000);
  if (outcome != LoopExit::kOk) {
    QZ_CHECK_EQ(std::string("Loop() did not stop on its own: ") + ToString(outcome), std::string("kOk"));
  } else {
    QZ_EXPECT(true);
  }
  QZ_CHECK(!loop.running());
  // quit 请求是**持久**的：Loop 返回后仍然是 true。这正是"停止不可重启"的来源。
  QZ_CHECK(loop.quit_requested());
  // Loop 返回后 loop 线程解绑：从别的线程看不再是 loop 线程。
  QZ_CHECK(!loop.IsInLoopThread());
  // 但 loop_thread_id 保留下来（调试用：知道是哪个线程跑过）。
  QZ_CHECK(loop.loop_thread_id() != std::thread::id());
}

// ===========================================================================
// B. 定时器在 Loop 之前排好：到点触发，并在回调里 Quit
// ===========================================================================

void TestRunAfterFiresBeforeLoopAndStops() {
  EventLoop loop;
  std::atomic<int> fired{0};

  // delay=0：下一次 Wait 的超时就是 0，立刻到期。
  // 这条同时钉住 RunAfter 的实现注意点——TimerQueue 是纯堆，没有 timerfd，所以"加定时器"
  // 必须显式 Wakeup()，否则 loop 可能正阻塞在一个按旧堆顶算出来的超时上（静默不触发）。
  loop.RunAfter(0, [&loop, &fired] {
    fired.fetch_add(1, std::memory_order_relaxed);
    loop.Quit();
  });
  QZ_CHECK(loop.wakeup_count() > 0);

  const LoopExit outcome = RunLoopWithWatchdog(loop, 10000, 5000);
  QZ_CHECK_EQ(std::string(ToString(outcome)), std::string("kOk"));
  QZ_CHECK_EQ(fired.load(), 1);
}

// ===========================================================================
// C. RunInLoop 在 loop 线程里是**同步内联**执行
// ===========================================================================

void TestRunInLoopExecutesInlineInLoopThread() {
  EventLoop loop;
  std::atomic<int> depth{0};
  std::atomic<int> inner_depth{0};
  std::atomic<bool> stayed_in_loop_thread{false};
  std::atomic<std::thread::id> outer_id{std::thread::id()};
  std::atomic<std::thread::id> inner_id{std::thread::id()};

  loop.RunAfter(0, [&] {
    depth.fetch_add(1);
    outer_id = std::this_thread::get_id();
    // 在 loop 线程里调 RunInLoop：契约是"立即同步执行"，所以内层的 depth 必须是 2
    // （外层还没返回），且仍在同一个线程。
    loop.RunInLoop([&] {
      depth.fetch_add(1);
      inner_depth = depth.load();
      inner_id = std::this_thread::get_id();
      stayed_in_loop_thread = loop.IsInLoopThread();
    });
    loop.Quit();
  });

  const LoopExit outcome = RunLoopWithWatchdog(loop, 10000, 5000);
  QZ_CHECK_EQ(std::string(ToString(outcome)), std::string("kOk"));
  QZ_CHECK_EQ(depth.load(), 2);
  QZ_CHECK_EQ(inner_depth.load(), 2);  // 同步内联：内层看到 depth 已是 2
  QZ_CHECK(inner_id.load() == outer_id.load());
  QZ_CHECK(outer_id.load() == loop.loop_thread_id());
  QZ_CHECK(stayed_in_loop_thread.load());
}

// ===========================================================================
// D. QueueInLoop 在 loop 线程里是**延迟**执行（不在调用栈里重入）
// ===========================================================================

void TestQueueInLoopDoesNotReenterCallStack() {
  EventLoop loop;
  bool first_returned = false;
  std::atomic<bool> second_saw_first_returned{false};
  std::atomic<int> order{0};

  loop.RunAfter(0, [&] {
    loop.QueueInLoop([&] {
      // 这个回调必须在本轮 IO/定时器处理完之后才跑，此时 first 早已返回。
      second_saw_first_returned = first_returned;
      order.fetch_add(1);
      loop.Quit();
    });
    QZ_EXPECT(order.load() == 0);  // 排队时没有立刻执行
    first_returned = true;
    order.fetch_add(1);            // first 先完成
  });

  const LoopExit outcome = RunLoopWithWatchdog(loop, 10000, 5000);
  QZ_CHECK_EQ(std::string(ToString(outcome)), std::string("kOk"));
  QZ_CHECK(second_saw_first_returned.load());
  QZ_CHECK_EQ(order.load(), 2);
}

// ===========================================================================
// E. 跨线程 QueueInLoop：任务在 loop 线程执行，并且唤醒确实发生了
// ===========================================================================

void TestQueueInLoopFromOtherThread() {
  EventLoop loop;
  std::atomic<bool> ran{false};
  std::atomic<bool> ran_in_loop_thread{false};
  std::atomic<std::thread::id> ran_on{std::thread::id()};

  const std::uint64_t wakeup_before = loop.wakeup_count();
  loop.QueueInLoop([&] {
    ran_on = std::this_thread::get_id();
    ran_in_loop_thread = loop.IsInLoopThread();
    ran = true;
    loop.Quit();
  });
  // 调用方不是 loop 线程 → 必须唤醒（loop 可能正阻塞在 Wait 上）。
  QZ_CHECK(loop.wakeup_count() > wakeup_before);

  const LoopExit outcome = RunLoopWithWatchdog(loop, 10000, 5000);
  QZ_CHECK_EQ(std::string(ToString(outcome)), std::string("kOk"));
  QZ_CHECK(ran.load());
  QZ_CHECK(ran_in_loop_thread.load());
  QZ_CHECK(ran_on.load() == loop.loop_thread_id());
  QZ_CHECK(ran_on.load() != std::this_thread::get_id());
}

// ===========================================================================
// F. 退出前把任务队列排空：Quit 早到也不能丢已排队的任务
// ===========================================================================

void TestPendingTasksAreDrainedOnExit() {
  EventLoop loop;
  std::atomic<int> ran{0};

  for (int i = 0; i < 3; ++i) {
    loop.QueueInLoop([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
  }
  QZ_CHECK_EQ(loop.pending_task_count(), static_cast<std::size_t>(3));

  // 先 Quit 再进 Loop：循环体一轮都不执行，走的是"退出前排空"那条路径。
  loop.Quit();
  const LoopExit outcome = RunLoopWithWatchdog(loop, 10000, 5000);
  QZ_CHECK_EQ(std::string(ToString(outcome)), std::string("kOk"));

  // 为什么这条重要：框架里大量"延迟析构 / 延迟回调"依赖这个队列（Connection 的 close
  // 回调、Connector 的 Channel 延迟释放、Server 的关连接任务）。留给 ~EventLoop 去跑，
  // 那些任务引用的对象可能已经析构 —— use-after-free。
  QZ_CHECK_EQ(ran.load(), 3);
  QZ_CHECK_EQ(loop.pending_task_count(), static_cast<std::size_t>(0));
}

// ===========================================================================
// G. 停止不可重启：Quit 之后第二次 Loop() 立即返回，不会"复活"
// ===========================================================================

void TestLoopCannotBeRestarted() {
  EventLoop loop;
  loop.Quit();
  QZ_CHECK_EQ(std::string(ToString(RunLoopWithWatchdog(loop, 10000, 5000))), std::string("kOk"));
  QZ_CHECK(!loop.running());

  // 第二次 Loop：quit_ 仍然是 true（没人清它），所以进循环体之前就退出。
  // 这条钉的是语义而不是实现：EventLoop 与 Channel / ConnectionPool 一致 —— 停止不可重启。
  // 如果将来有人想让 Loop 可重启，他必须先给 quit_ 找一个"清零时机"，
  // 而上面 A 那条用例会立刻变红 —— 这就是这两条用例配对存在的原因。
  const auto begin = std::chrono::steady_clock::now();
  const LoopExit second = RunLoopWithWatchdog(loop, 10000, 5000);
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin)
          .count();
  QZ_CHECK_EQ(std::string(ToString(second)), std::string("kOk"));
  QZ_CHECK(loop.quit_requested());
  QZ_CHECK(!loop.running());
  QZ_CHECK(elapsed_ms < 5000);  // 立即返回，不是等超时
}

// ===========================================================================
// H. 空回调、未启动时的状态查询：不该崩也不该改变状态
// ===========================================================================

void TestEmptyCallbacksAndIdleState() {
  EventLoop loop;
  // 空 function 一律忽略（不判空会在 WorkerLoop / DoPendingTasks 里直接 terminate）。
  loop.RunInLoop(std::function<void()>());
  loop.QueueInLoop(std::function<void()>());
  QZ_CHECK_EQ(loop.pending_task_count(), static_cast<std::size_t>(0));
  QZ_CHECK(!loop.running());
  QZ_CHECK(!loop.quit_requested());

  // 未启动时 Queued 任务不执行，但 Loop 一跑就会执行（不依赖任何外部条件）。
  std::atomic<int> ran{0};
  loop.QueueInLoop([&ran] { ran.fetch_add(1); });
  QZ_CHECK_EQ(ran.load(), 0);
  loop.Quit();
  QZ_CHECK_EQ(std::string(ToString(RunLoopWithWatchdog(loop, 10000, 5000))), std::string("kOk"));
  QZ_CHECK_EQ(ran.load(), 1);
}

}  // namespace

int main() {
  // 每条用例前置打印并刷新：真挂住时最后一行就是凶手（ctest 的超时只给一个数字）。
  std::printf("== TestQuitBeforeLoopStillStops\n");
  std::fflush(stdout);
  TestQuitBeforeLoopStillStops();

  std::printf("== TestRunAfterFiresBeforeLoopAndStops\n");
  std::fflush(stdout);
  TestRunAfterFiresBeforeLoopAndStops();

  std::printf("== TestRunInLoopExecutesInlineInLoopThread\n");
  std::fflush(stdout);
  TestRunInLoopExecutesInlineInLoopThread();

  std::printf("== TestQueueInLoopDoesNotReenterCallStack\n");
  std::fflush(stdout);
  TestQueueInLoopDoesNotReenterCallStack();

  std::printf("== TestQueueInLoopFromOtherThread\n");
  std::fflush(stdout);
  TestQueueInLoopFromOtherThread();

  std::printf("== TestPendingTasksAreDrainedOnExit\n");
  std::fflush(stdout);
  TestPendingTasksAreDrainedOnExit();

  std::printf("== TestLoopCannotBeRestarted\n");
  std::fflush(stdout);
  TestLoopCannotBeRestarted();

  std::printf("== TestEmptyCallbacksAndIdleState\n");
  std::fflush(stdout);
  TestEmptyCallbacksAndIdleState();

  return qztest::Summary("test_event_loop");
}
