// 轻舟 RPC (Qingzhou RPC)
// src/net/event_loop.cpp
//
// 职责：一个线程的运行环境 = Poller + TimerQueue + 任务队列 + 唤醒对。
//
// 唤醒机制（doc 00 §3.1 第 1 条，顺序绝对不能颠倒）：
//   1) 创建一个 DGRAM socket，bind 到 127.0.0.1:0（端口由系统分配）；
//   2) **必须**用 getsockname 取回系统实际分配的端口；
//   3) 再创建第二个 DGRAM socket 并 connect 到 (127.0.0.1, 该端口)。
//   如果先 connect 后 bind，或直接用 bind 前的端口去 connect，Windows 会返回
//   WSAEADDRNOTAVAIL / WSAEINVAL，循环从此再也醒不过来（表现为跨线程任务饥饿）。
//
// 关于唤醒 fd 没有走 Channel：
//   EventLoop 头文件（已冻结）没有为唤醒 Channel 预留成员，而 channels_ 只存裸指针，
//   无法托管所有权。因此唤醒 fd 直接注册在 Poller 上，由 Loop() 在派发前特判，
//   既不需要额外成员，也不引入"new 一个 Channel 却没人 delete"的泄漏。
//   这一处偏离已记录在交付报告的「契约缺口」中。

#include "net/event_loop.h"

#include <cstring>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/time_util.h"
#include "net/channel.h"
#include "net/platform.h"
#include "net/socket_util.h"

namespace qzrpc {
namespace net {

namespace {

#ifdef _WIN32
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

}  // namespace

EventLoop::EventLoop()
    : poller_(Poller::Create()), timer_queue_(new TimerQueue()) {
  CreateWakeupFds();
}

EventLoop::~EventLoop() {
  // 先请求退出（若仍在跑，说明用户没调 Quit，属于调用方的编排错误），再关唤醒 fd。
  quit_.store(true, std::memory_order_relaxed);
  // 把残留任务跑一遍：它们可能持有 Connection / Channel 的所有权（延迟析构就是这么实现的），
  // 必须在 poller_ 与 channels_ 还活着的时候释放，否则 ~Connection→Channel::Remove()
  // 会碰到已经析构的 Poller。
  DoPendingTasks();
  CloseWakeupFds();
  // 注意：此处不析构 channels_ 里残留的 Channel —— Channel 的生命周期归其持有者
  // （Acceptor/Connection），本类只保存裸指针。
  channels_.clear();
}

bool EventLoop::CreateWakeupFds() {
  PlatformInit::EnsureInit();

  const SocketHandle recv_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (recv_sock == kInvalidSocket) {
    QZ_LOG_ERROR << "event loop: create wakeup recv socket failed: "
                 << SocketUtil::ErrorString(SocketUtil::LastError());
    return false;
  }
  const int recv_fd = static_cast<int>(recv_sock);
  wakeup_recv_fd_ = recv_fd;
  SocketUtil::SetNonBlocking(recv_fd, true);

  sockaddr_in bind_addr;
  std::memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = static_cast<unsigned short>(AF_INET);
  bind_addr.sin_port = 0;
  bind_addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

  // 步骤 1：bind 到回环地址的随机端口。
  if (::bind(recv_sock, reinterpret_cast<sockaddr*>(&bind_addr),
             static_cast<SockLen>(sizeof(bind_addr))) != 0) {
    QZ_LOG_ERROR << "event loop: bind wakeup socket failed: "
                 << SocketUtil::ErrorString(SocketUtil::LastError());
    CloseWakeupFds();
    return false;
  }
  // 步骤 2：取回系统分配的端口（不能直接用 bind 时传入的 0）。
  sockaddr_in bound_addr;
  std::memset(&bound_addr, 0, sizeof(bound_addr));
  SockLen bound_len = static_cast<SockLen>(sizeof(bound_addr));
  if (::getsockname(recv_sock, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) != 0) {
    QZ_LOG_ERROR << "event loop: getsockname on wakeup socket failed: "
                 << SocketUtil::ErrorString(SocketUtil::LastError());
    CloseWakeupFds();
    return false;
  }

  const SocketHandle send_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (send_sock == kInvalidSocket) {
    QZ_LOG_ERROR << "event loop: create wakeup send socket failed: "
                 << SocketUtil::ErrorString(SocketUtil::LastError());
    CloseWakeupFds();
    return false;
  }
  const int send_fd = static_cast<int>(send_sock);
  wakeup_send_fd_ = send_fd;
  // 必须非阻塞：UDP 发送缓冲满时若阻塞，会卡住调用 Wakeup() 的那个线程（可能是业务线程）。
  SocketUtil::SetNonBlocking(send_fd, true);

  // 步骤 3：connect 到刚刚取回的真实端口。
  if (::connect(send_sock, reinterpret_cast<const sockaddr*>(&bound_addr), bound_len) != 0) {
    QZ_LOG_ERROR << "event loop: connect wakeup socket failed: "
                 << SocketUtil::ErrorString(SocketUtil::LastError());
    CloseWakeupFds();
    return false;
  }

  if (!poller_->Add(wakeup_recv_fd_, Poller::kReadable)) {
    QZ_LOG_ERROR << "event loop: register wakeup fd to poller(" << poller_->Name() << ") failed";
    CloseWakeupFds();
    return false;
  }
  return true;
}

void EventLoop::CloseWakeupFds() noexcept {
  if (wakeup_recv_fd_ >= 0) {
    if (poller_) poller_->Del(wakeup_recv_fd_);
    SocketUtil::Close(wakeup_recv_fd_);
    wakeup_recv_fd_ = -1;
  }
  if (wakeup_send_fd_ >= 0) {
    SocketUtil::Close(wakeup_send_fd_);
    wakeup_send_fd_ = -1;
  }
}

void EventLoop::Loop() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    QZ_LOG_ERROR << "event loop: Loop() called twice or re-entered, ignored";
    return;
  }
  // ★ 这里**绝不能** quit_.store(false)。
  //
  // 曾经的写法是"进 Loop 先清零退出标志"，看起来是"让本对象可重启"，实际上是丢唤醒：
  //   Loop() 是在 **loop 线程**里执行的，而 Quit() 由**别的线程**（Channel::Stop / Server::Stop）
  //   调用。两者之间存在真实窗口：线程已创建（std::thread 构造返回）、还没被调度到 Loop 的
  //   第一行，此时 Quit() 已经 quit_=true 并 Wakeup()，随后 Loop() 一进来就把标志抹成 false
  //   → 线程永远不退出 → Stop() 里的 join() 永久挂死（连接池/Channel 都已拆完，没有任何
  //   东西能再叫醒它）。
  //
  //   实测（tests/test_e2e_echo 的 F 段：起 Channel → 调一次 → Stop → 重新建 Channel）：
  //   加打印前 8 次运行挂 5 次；抓到的现场就是 "QUIT requested" 打印在 "START" 之前，
  //   随后该线程 iter#1 打出 quit=0 并一直跑下去。改成"quit_ 只由构造函数置 false"后
  //   连跑 20 次全过。
  //
  // 语义依据：EventLoop 与 Channel / ConnectionPool 一致 —— **停止不可重启**。
  // 全树没有任何调用方对同一个 EventLoop 调用两次 Loop()（调用点只有 channel.cpp:348、
  // server.cpp 的 io/metrics 线程，都是先 new 再起线程），所以这个清零从来没有生产者。
  loop_thread_id_ = std::this_thread::get_id();
  thread_bound_.store(true, std::memory_order_release);

  std::vector<Poller::EventEntry> active;
  while (!quit_.load(std::memory_order_relaxed)) {
    const std::uint64_t now_ms = TimeUtil::NowMs();
    const int timeout_ms = timer_queue_->NextExpirationMs(now_ms);

    active.clear();
    const int ready = poller_->Wait(&active, timeout_ms);
    if (ready < 0) {
      const int err = SocketUtil::LastError();
      // EINTR 是"被信号打断"，不是错误；其余错误继续跑下去只会无限刷日志。
      if (SocketUtil::WouldBlock(err)) continue;
      QZ_LOG_ERROR << "event loop: poller(" << poller_->Name() << ") wait failed: "
                   << SocketUtil::ErrorString(err);
      break;
    }

    for (const Poller::EventEntry& entry : active) {
      if (entry.fd == wakeup_recv_fd_) {
        HandleWakeupRead();
        continue;
      }
      const auto it = channels_.find(entry.fd);
      if (it == channels_.end() || it->second == nullptr) {
        // fd 已在上一个回调里被摘除：直接跳过，不做任何事。
        continue;
      }
      it->second->HandleEvent(entry.events);
    }

    timer_queue_->RunExpired(TimeUtil::NowMs());
    DoPendingTasks();
  }

  // ★ 退出前把任务队列彻底排空（在 loop 线程里做）。
  //   框架里有大量"延迟析构 / 延迟回调"依赖这个队列：Connection 的 close 回调、
  //   Connector 的 Channel 延迟释放、Server 的关连接任务……如果留到 ~EventLoop 才跑，
  //   那些任务引用的对象（连接表、用户状态）可能已经被析构，变成 use-after-free。
  //   这里给出**最后一轮机会**，并且反复几轮以容纳"任务里又排任务"的情况。
  for (int round = 0; round < 64; ++round) {
    if (pending_task_count() == 0) break;
    DoPendingTasks();
  }

  thread_bound_.store(false, std::memory_order_release);
  running_.store(false, std::memory_order_release);
}

void EventLoop::Quit() {
  quit_.store(true, std::memory_order_relaxed);
  // 即使已经收到过 Quit 也照样唤醒：唤醒是幂等且廉价的，漏掉一次就会挂死。
  Wakeup();
}

bool EventLoop::running() const noexcept { return running_.load(std::memory_order_acquire); }

bool EventLoop::quit_requested() const noexcept { return quit_.load(std::memory_order_relaxed); }

void EventLoop::RunInLoop(std::function<void()> cb) {
  if (!cb) return;
  if (IsInLoopThread()) {
    cb();
    return;
  }
  QueueInLoop(std::move(cb));
}

void EventLoop::QueueInLoop(std::function<void()> cb) {
  if (!cb) return;
  bool need_wakeup = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_tasks_.push_back(std::move(cb));
    // 两种情况必须唤醒：
    //   * 调用方不是 loop 线程（loop 可能正阻塞在 Wait 上）；
    //   * 调用方是 loop 线程，但当前不在 DoPendingTasks 里（例如在某个 IO 回调里排队）。
    need_wakeup = !calling_pending_tasks_ || !IsInLoopThread();
  }
  if (need_wakeup) Wakeup();
}

void EventLoop::Wakeup() {
  wakeup_count_.fetch_add(1, std::memory_order_relaxed);
  if (wakeup_send_fd_ < 0) return;
  const char kByte = 1;
  // 写失败只有两种原因：缓冲满（说明已有待处理唤醒）或 fd 已关（析构期）。
  // 两种情况都不需要处理，因此忽略返回值。
  SocketUtil::Send(wakeup_send_fd_, &kByte, 1);
}

TimerId EventLoop::RunAfter(std::uint64_t delay_ms, TimerCallback cb) {
  const TimerId id = timer_queue_->AddTimer(delay_ms, std::move(cb));
  // ★ 必须唤醒 loop：本框架的 TimerQueue 是纯堆（没有 timerfd 那种能自动进内核等待集
  //   的描述符），loop 可能正阻塞在一个"按旧堆顶算出来的超时"上（甚至是永久阻塞）。
  //   不唤醒就会出现"定时器加了但永远不触发"的静默故障。
  //   若当前就是 loop 线程则不需要唤醒（本轮结束会重新计算 NextExpirationMs）。
  if (!IsInLoopThread()) Wakeup();
  return id;
}

TimerId EventLoop::RunEvery(std::uint64_t interval_ms, TimerCallback cb) {
  const TimerId id = timer_queue_->AddRepeatingTimer(interval_ms, std::move(cb));
  if (!IsInLoopThread()) Wakeup();  // 同上
  return id;
}

void EventLoop::CancelTimer(TimerId id) {
  // 取消不需要唤醒：被取消的条目只可能让 loop **提前**醒来（它的 due 更早），
  // 不会造成任何定时器延迟触发，因此省掉一次跨线程唤醒。
  timer_queue_->Cancel(id);
}

bool EventLoop::IsInLoopThread() const noexcept {
  if (!thread_bound_.load(std::memory_order_acquire)) return false;
  return std::this_thread::get_id() == loop_thread_id_;
}

std::thread::id EventLoop::loop_thread_id() const noexcept { return loop_thread_id_; }

std::size_t EventLoop::pending_task_count() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_tasks_.size();
}

std::size_t EventLoop::registered_fd_count() const { return poller_->Size(); }

std::uint64_t EventLoop::wakeup_count() const noexcept {
  return wakeup_count_.load(std::memory_order_relaxed);
}

bool EventLoop::UpdateChannel(Channel* channel) {
  if (channel == nullptr) return false;
  const int fd = channel->fd();
  const auto it = channels_.find(fd);
  if (it == channels_.end()) {
    if (!poller_->Add(fd, channel->events())) return false;
    channels_[fd] = channel;
    channel->attached_ = true;
    return true;
  }
  // 已登记：可能是同一个 Channel 改事件，也可能是同一个 fd 换了新 Channel（fd 复用）。
  // 后者必须先 Del 再 Add，否则 Poller 会认为 fd 已存在而拒绝。
  if (it->second != channel) {
    poller_->Del(fd);
    it->second = channel;
  }
  channel->attached_ = true;
  return poller_->Mod(fd, channel->events());
}

void EventLoop::RemoveChannel(Channel* channel) {
  if (channel == nullptr) return;
  const int fd = channel->fd();
  const auto it = channels_.find(fd);
  if (it != channels_.end() && it->second == channel) {
    channels_.erase(it);
    poller_->Del(fd);
  }
  channel->attached_ = false;
}

void EventLoop::DoPendingTasks() {
  std::vector<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_tasks_.empty()) return;
    // 用 swap 取走队列：绝不能持锁执行回调（回调里 QueueInLoop 会立刻自死锁）。
    tasks.swap(pending_tasks_);
    calling_pending_tasks_ = true;
  }
  for (std::function<void()>& task : tasks) {
    if (task) task();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    calling_pending_tasks_ = false;
  }
}

void EventLoop::HandleWakeupRead() {
  char buf[128];
  for (;;) {
    const long n = SocketUtil::Recv(wakeup_recv_fd_, buf, sizeof(buf));
    if (n > 0) continue;  // 可能攒了多个字节/多个数据报，全部排空
    if (n < 0) {
      const int err = SocketUtil::LastError();
      if (SocketUtil::WouldBlock(err)) return;  // 正常：排空完毕
      // 其它错误（对端 ICMP 不可达等）不影响后续唤醒，记录后返回即可。
      QZ_LOG_DEBUG << "event loop: wakeup recv error: " << SocketUtil::ErrorString(err);
    }
    // n == 0：UDP 上是零长度数据报，没有更多信息，直接返回。
    return;
  }
}

}  // namespace net
}  // namespace qzrpc
