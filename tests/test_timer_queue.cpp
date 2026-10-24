// 轻舟 RPC (Qingzhou RPC)
// tests/test_timer_queue.cpp
//
// 最小堆定时器的契约测试（T-07）。
//
// 为什么定时器值得单独测：
//   它的错误形态全是"晚了 / 早了 / 再也不来"——连接空闲回收、请求 deadline、注册中心心跳、
//   自适应限流调节全都挂在这上面。一个"周期性定时器被 Cancel 后还多触发一次"的 bug，
//   在线上表现为"某个后台任务偶发多跑一次"，几乎不可能从日志里看出来。
//
// 两条不变量（实现注释里写明，测试要守住）：
//   1) 回调**绝不在持锁状态**下执行 —— 否则回调里调 Cancel/AddTimer 立刻自死锁；
//   2) 取消是**惰性**的 —— Cancel 只往集合里插 id，条目在"弹出时"被丢弃。
//
// ★ 关于时间：`AddTimer` 内部用 `TimeUtil::NowMs()` 取当前时间，而 `RunExpired(now_ms)`
//   的时间由调用方传入。所以测试里所有"到期时间"都用 `NowMs()` 的相对值表达，
//   并且**只用宽松上界**（"t0+999 一定还没到期"、"t0+2000 一定到期"），
//   不写"正好等于 1000"这种断言 —— 那是在断言调度器精度，不是在断言被测代码。
//
// ★ 关于同一时刻的多个定时器：`RunExpired` 保证"本轮所有已到期的都执行完"，
//   **不保证**它们之间的先后顺序（std::priority_queue 对相等键不稳定，头文件也没承诺）。
//   所以这里断言的是"同一批全部触发、每个恰好一次"，而不是具体顺序；
//   需要顺序的调用方应该用不同的 delay 表达，而不是依赖堆的内部布局。

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/time_util.h"
#include "net/timer_queue.h"
#include "test_main.h"

namespace {

using qzrpc::TimeUtil;
using qzrpc::net::TimerId;
using qzrpc::net::TimerQueue;

// ===========================================================================
// A. 一次性定时器的基本语义
// ===========================================================================

void TestOneShotFiresOnceAtDeadline() {
  TimerQueue queue;
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
  QZ_CHECK_EQ(queue.NextExpirationMs(TimeUtil::NowMs()), -1);

  const std::uint64_t t0 = TimeUtil::NowMs();
  std::atomic<int> fired{0};
  const TimerId id = queue.AddTimer(1000, [&fired] { fired.fetch_add(1); });
  QZ_CHECK(id != 0);
  QZ_CHECK_EQ(queue.Size(), std::size_t{1});

  // AddTimer 内部取的是"调用那一刻"的 NowMs，所以 due 落在 [t0+1000, t0+1000+ε]。
  const int wait = queue.NextExpirationMs(t0);
  QZ_CHECK(wait >= 1000);
  QZ_CHECK(wait <= 1005);  // ε 只允许几毫秒的调度抖动

  // 提前 1ms：绝不能触发（"早了"是最容易写错的方向，因为 <= 和 < 只差一个字符）。
  QZ_CHECK_EQ(queue.RunExpired(t0 + 999), 0);
  QZ_CHECK_EQ(fired.load(), 0);
  QZ_CHECK_EQ(queue.Size(), std::size_t{1});  // 还在堆里

  // 到期：触发且只触发一次。
  QZ_CHECK_EQ(queue.RunExpired(t0 + 2000), 1);
  QZ_CHECK_EQ(fired.load(), 1);
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
  QZ_CHECK_EQ(queue.NextExpirationMs(t0 + 2000), -1);

  // 再跑一次不能重复触发（堆里已经没有了）。
  QZ_CHECK_EQ(queue.RunExpired(t0 + 3000), 0);
  QZ_CHECK_EQ(fired.load(), 1);
}

// 回调按 due 升序执行。这是堆的契约，也是"多个 deadline 同时存在"时的唯一可依赖顺序。
void TestCallbacksRunInDeadlineOrder() {
  TimerQueue queue;
  const std::uint64_t t0 = TimeUtil::NowMs();

  std::vector<std::string> order;
  // 故意乱序添加。
  queue.AddTimer(30, [&order] { order.push_back("30"); });
  queue.AddTimer(10, [&order] { order.push_back("10"); });
  queue.AddTimer(20, [&order] { order.push_back("20"); });

  QZ_CHECK_EQ(queue.RunExpired(t0 + 100), 3);
  QZ_CHECK_EQ(order.size(), std::size_t{3});
  if (order.size() == 3) {
    QZ_CHECK_EQ(order[0], std::string("10"));
    QZ_CHECK_EQ(order[1], std::string("20"));
    QZ_CHECK_EQ(order[2], std::string("30"));
  }
}

// 同一批到期的定时器：**全部**在本次 RunExpired 里执行，每个恰好一次。
// 顺序不做断言（见文件头注释）。
void TestSameDeadlineAllFireInOneBatch() {
  TimerQueue queue;
  const std::uint64_t t0 = TimeUtil::NowMs();

  constexpr int kTimers = 20;
  std::atomic<int> fired{0};
  std::vector<int> hit(kTimers, 0);
  for (int i = 0; i < kTimers; ++i) {
    // delay = 0：due 就是"添加那一刻"，全部落在同一批。
    const TimerId id = queue.AddTimer(0, [&fired, &hit, i] {
      fired.fetch_add(1);
      hit[static_cast<std::size_t>(i)] += 1;
    });
    QZ_CHECK(id != 0);
  }
  QZ_CHECK_EQ(queue.Size(), static_cast<std::size_t>(kTimers));

  // 一次 RunExpired 必须把它们全部取走 —— 如果实现只取一个，这里会返回 1。
  QZ_CHECK_EQ(queue.RunExpired(t0 + 100), kTimers);
  QZ_CHECK_EQ(fired.load(), kTimers);
  for (int i = 0; i < kTimers; ++i) {
    QZ_CHECK_EQ(hit[static_cast<std::size_t>(i)], 1);  // 每个恰好一次
  }
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
  QZ_CHECK_EQ(queue.RunExpired(t0 + 200), 0);  // 没有残留
}

// 空任务必须被拒绝（返回无效 id 0），而不是塞进堆里等到期时抛 bad_function_call。
void TestNullCallbackRejected() {
  TimerQueue queue;
  // 空 std::function 必须被拒绝（返回无效 id 0），而不是塞进堆里等到期时抛
  // bad_function_call —— 那会把异常带到事件循环线程里。
  QZ_CHECK_EQ(queue.AddTimer(10, std::function<void()>()), TimerId{0});
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
  QZ_CHECK_EQ(queue.NextExpirationMs(TimeUtil::NowMs()), -1);

  QZ_CHECK_EQ(queue.AddRepeatingTimer(10, std::function<void()>()), TimerId{0});
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
}

// ===========================================================================
// B. 取消
// ===========================================================================

void TestCancelBeforeFire() {
  TimerQueue queue;
  const std::uint64_t t0 = TimeUtil::NowMs();

  std::atomic<int> fired{0};
  const TimerId id = queue.AddTimer(1000, [&fired] { fired.fetch_add(1); });
  QZ_CHECK(id != 0);

  QZ_CHECK(queue.Cancel(id));
  QZ_CHECK_EQ(queue.RunExpired(t0 + 5000), 0);
  QZ_CHECK_EQ(fired.load(), 0);
  // 惰性删除：Cancel 不立即出堆，但下一次 RunExpired 之后必须消失（不能永远占着）。
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});

  // 重复取消同一个 id：返回 true（id 曾经有效）但不产生任何副作用。
  QZ_CHECK(queue.Cancel(id));
  QZ_CHECK_EQ(queue.RunExpired(t0 + 6000), 0);

  // 取消非法 id：0 与"还没发出去的号"都必须返回 false，且不能崩。
  QZ_CHECK(!queue.Cancel(0));
  QZ_CHECK(!queue.Cancel(99999999));
}

// 已触发的一次性定时器再 Cancel：契约写的是"安全的空操作"，必须有界且不影响别人。
void TestCancelAfterFireIsHarmless() {
  TimerQueue queue;
  const std::uint64_t t0 = TimeUtil::NowMs();

  std::atomic<int> first{0};
  const TimerId fired_id = queue.AddTimer(10, [&first] { first.fetch_add(1); });
  std::atomic<int> second{0};
  const TimerId live_id = queue.AddTimer(20, [&second] { second.fetch_add(1); });

  QZ_CHECK_EQ(queue.RunExpired(t0 + 500), 2);
  QZ_CHECK_EQ(first.load(), 1);
  QZ_CHECK_EQ(second.load(), 1);

  // 两个都已经触发过。对它们 Cancel 不能影响任何东西，也不能抛。
  queue.Cancel(fired_id);
  queue.Cancel(live_id);
  QZ_CHECK_EQ(queue.RunExpired(t0 + 1000), 0);
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
}

// 回调在**不持锁**状态下执行 —— 这条不变量如果破了，下面这段会自死锁（表现为用例超时）。
// 事件循环里最常见的写法就是"定时器回调里再排一个定时器"，所以这不是极端场景。
void TestCallbackMayCallBackIntoQueue() {
  TimerQueue queue;
  const std::uint64_t t0 = TimeUtil::NowMs();

  std::atomic<int> fired{0};
  std::atomic<int> chained{0};
  TimerId victim = 0;
  victim = queue.AddTimer(10, [&] {
    fired.fetch_add(1);
    queue.Cancel(victim);                                  // 回调里取消自己
    queue.AddTimer(10, [&chained] { chained.fetch_add(1); });  // 回调里再排一个
    (void)queue.Size();                                    // 回调里读状态
  });
  QZ_CHECK(victim != 0);

  QZ_CHECK_EQ(queue.RunExpired(t0 + 100), 1);   // 本次只执行原来那个
  QZ_CHECK_EQ(fired.load(), 1);
  QZ_CHECK_EQ(chained.load(), 0);               // 新排的还没到期
  QZ_CHECK_EQ(queue.Size(), std::size_t{1});

  QZ_CHECK_EQ(queue.RunExpired(t0 + 200), 1);
  QZ_CHECK_EQ(chained.load(), 1);
}

// ===========================================================================
// C. 周期性定时器
// ===========================================================================

void TestRepeatingFiresRepeatedly() {
  TimerQueue queue;
  std::atomic<int> fired{0};
  const TimerId id = queue.AddRepeatingTimer(1000, [&fired] { fired.fetch_add(1); });
  QZ_CHECK(id != 0);

  // 每轮用"当前真实时间 + 余量"驱动：周期性定时器重新入堆时用的是 NowMs()，
  // 所以传一个足够大的 now_ms 才能保证每轮都到期。用真实时间而不是伪造的基准，
  // 是因为实现内部已经绑定了 NowMs —— 伪造基准只会测出一个永远不会到期的队列。
  int saw_fire = 0;
  for (int round = 0; round < 3; ++round) {
    const std::uint64_t now = TimeUtil::NowMs() + 1100;
    const int executed = queue.RunExpired(now);
    if (executed > 0) ++saw_fire;
    QZ_CHECK(executed <= 1);  // 每轮最多触发一次（不因为时间跨度大就补触发多次）
  }
  QZ_CHECK_EQ(saw_fire, 3);
  QZ_CHECK_EQ(fired.load(), 3);
  QZ_CHECK_EQ(queue.Size(), std::size_t{1});  // 仍在堆里等下一轮

  // 一次 Cancel 必须停掉整条链（因为重新入堆用的是同一个 id）。
  QZ_CHECK(queue.Cancel(id));
  QZ_CHECK_EQ(queue.RunExpired(TimeUtil::NowMs() + 5000), 0);
  QZ_CHECK_EQ(fired.load(), 3);
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
}

// 在回调里取消自己：这是"心跳定时器只跑一次就停"的标准写法，必须干净退出，
// 而**不是**再多跑一次（多跑一次的表现是"停服了还在打心跳"）。
void TestRepeatingSelfCancelStopsChain() {
  TimerQueue queue;
  std::atomic<int> fired{0};
  TimerId id = 0;
  id = queue.AddRepeatingTimer(1000, [&] {
    fired.fetch_add(1);
    queue.Cancel(id);
  });
  QZ_CHECK(id != 0);

  for (int round = 0; round < 4; ++round) {
    queue.RunExpired(TimeUtil::NowMs() + 1100);
  }
  QZ_CHECK_EQ(fired.load(), 1);
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
}

// interval == 0 会让"每轮循环都触发"变成忙循环，实现把它夹到 1ms。
// 这里只断言"能工作且不崩"，不断言具体频率。
void TestRepeatingZeroIntervalIsCoerced() {
  TimerQueue queue;
  std::atomic<int> fired{0};
  QZ_CHECK(queue.AddRepeatingTimer(0, [&fired] { fired.fetch_add(1); }) != 0);
  QZ_CHECK_EQ(queue.Size(), std::size_t{1});
  QZ_CHECK(queue.RunExpired(TimeUtil::NowMs() + 1000) == 1);
  QZ_CHECK_EQ(fired.load(), 1);
}

// ===========================================================================
// D. NextExpirationMs
// ===========================================================================

void TestNextExpirationMs() {
  TimerQueue queue;
  const std::uint64_t t0 = TimeUtil::NowMs();
  QZ_CHECK_EQ(queue.NextExpirationMs(t0), -1);  // 空队列 -> -1（调用方据此永久阻塞）

  queue.AddTimer(1000, [] {});
  queue.AddTimer(3000, [] {});
  // 返回**最近**那个的剩余时间，不是最后一个加进去的。
  const int wait = queue.NextExpirationMs(t0);
  QZ_CHECK(wait >= 1000);
  QZ_CHECK(wait <= 1005);

  // 已经过期（传一个很晚的 now）-> 0，表示"别睡了立刻跑"。
  QZ_CHECK_EQ(queue.NextExpirationMs(t0 + 5000), 0);

  // 清空后回到 -1。
  QZ_CHECK_EQ(queue.RunExpired(t0 + 5000), 2);
  QZ_CHECK_EQ(queue.NextExpirationMs(t0 + 5000), -1);
}

// 析构时堆里还有定时器：不得执行回调（闭包可能捕获已析构的对象），也不得崩。
void TestDestructorDropsPendingTimers() {
  std::atomic<int> fired{0};
  {
    TimerQueue queue;
    queue.AddTimer(10, [&fired] { fired.fetch_add(1); });
    queue.AddRepeatingTimer(10, [&fired] { fired.fetch_add(1); });
    QZ_CHECK_EQ(queue.Size(), std::size_t{2});
    // 不 RunExpired，直接离开作用域。
  }
  QZ_CHECK_EQ(fired.load(), 0);
}

// Size() 是"上限估计"：已取消但还没被弹出的条目也算在内（头文件明确写了）。
// 把这条语义钉住，避免有人把它当精确值写进 metrics 或判断逻辑。
void TestSizeIsUpperBoundEstimate() {
  TimerQueue queue;
  const std::uint64_t t0 = TimeUtil::NowMs();

  const TimerId a = queue.AddTimer(1000, [] {});
  const TimerId b = queue.AddTimer(2000, [] {});
  QZ_CHECK_EQ(queue.Size(), std::size_t{2});

  QZ_CHECK(queue.Cancel(a));
  // 惰性删除：Cancel 之后 Size 仍包含它（这是契约，不是 bug）。
  QZ_CHECK_EQ(queue.Size(), std::size_t{2});

  // RunExpired 会先清掉堆顶的已取消项，所以运行之后它就消失了。
  QZ_CHECK_EQ(queue.RunExpired(t0 + 500), 0);
  QZ_CHECK_EQ(queue.Size(), std::size_t{1});  // b 还在

  queue.Cancel(b);
  QZ_CHECK_EQ(queue.RunExpired(t0 + 5000), 0);
  QZ_CHECK_EQ(queue.Size(), std::size_t{0});
}

}  // namespace

int main() {
  // A. 一次性
  TestOneShotFiresOnceAtDeadline();
  TestCallbacksRunInDeadlineOrder();
  TestSameDeadlineAllFireInOneBatch();
  TestNullCallbackRejected();

  // B. 取消
  TestCancelBeforeFire();
  TestCancelAfterFireIsHarmless();
  TestCallbackMayCallBackIntoQueue();

  // C. 周期性
  TestRepeatingFiresRepeatedly();
  TestRepeatingSelfCancelStopsChain();
  TestRepeatingZeroIntervalIsCoerced();

  // D. 查询与生命周期
  TestNextExpirationMs();
  TestDestructorDropsPendingTimers();
  TestSizeIsUpperBoundEstimate();

  return qztest::Summary("test_timer_queue");
}
