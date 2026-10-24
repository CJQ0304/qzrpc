// 轻舟 RPC (Qingzhou RPC)
// src/client/connection_pool.cpp
//
// 职责：客户端长连接池 + 连接上的请求多路复用（seq 表）。
//
// ★ 五条必须小心的地方（前三条是团队裁决的硬约束）：
//   1) **seq 槽位在"拿到响应或连接关闭"之前不移除**。调用方超时后，槽位不能立刻删掉：
//      一旦删掉，同一个 seq 可能被后来的请求复用，而迟到的那一帧响应就会交给错误
//      的调用方（"A 的响应交给 B"，这是分布式客户端最阴的一类 bug）。
//      正确做法是把槽位留成**墓碑**（done=true），迟到响应到达时就地丢弃并清槽。
//   2) **墓碑不能无界增长**：超过 256 个就判定这条连接在丢响应，主动关掉让它重建
//      （墓碑同时占着 seq 空间与内存）。
//   3) **连接关闭必须唤醒全部等待者**：否则调用方会一直睡到超时才返回，
//      把"断连"表现成"慢"。
//   4) **连接的所有权在池**：Entry 的 busy 列表持有借用中的强引用，因此
//      ClientConnection 的析构只可能发生在池里；池统一把"最后一个引用"投递到
//      loop 线程释放，避免与事件派发并发（server 端踩过同类 use-after-free）。
//   5) **重试判据不在这里**：本文件只如实分类失败（`AcquireFailure` + Status），
//      "能不能重试 / 要不要换实例"由 channel.cpp 的矩阵与候选循环决定。
//
// ★ 为什么"借不到连接"必须分类（AcquireFailure，见 connection_pool.h 的表格）：
//   旧接口把"池满"和"连不上"都变成 kServiceUnavailable，而上层对它是"可重试"。
//   于是 8 个线程抢一条连接超时 → 全部重试 → 抢得更凶 —— 标准的正反馈拥塞。
//   现在 kPoolFull 直接以 kOverload 返回（让调用方退避，不换实例），
//   kConnectFailed 才允许换实例/重试。
//
// ★ 被动健康检查（per-endpoint 冷却，connect 级别）：
//   一个已经死掉的实例如果继续参与轮询，每次轮到它都要白等一个 connect_timeout，
//   吞吐腰斩、延迟周期性尖刺，而监控上只看到"成功率 50%"——比直接崩掉更难查。
//   所以：建连失败即把该 endpoint 放进冷却（3s 起、连续失败翻倍、上限 30s），
//   冷却期内 Acquire **立即失败**（省掉 TCP 超时），上层据此故障转移到别的实例。
//   **首次建连成功立刻清零**：不清零会让一个只是抖了一下的健康实例在网络恢复后
//   长期背锅（这是"冷却"与"永久拉黑"的分界线）。
//   分层：冷却只回答"这条 endpoint 现在还能不能连"，不改 LoadBalancer 的选择顺序 ——
//   选择器保持无状态，健康状态归池管。
//
// 线程模型：socket 非阻塞，全部 IO 在 Channel 的 loop 线程；Call 在业务线程阻塞等待，
//           靠 seq 多路复用，所以一条连接可以被多个业务线程同时使用。

#include "client/connection_pool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/metrics.h"
#include "common/time_util.h"
#include "net/event_loop.h"
#include "net/socket_util.h"
#include "protocol/codec.h"

namespace qzrpc {
namespace rpc {

namespace {

// 每条连接允许存在的墓碑（调用方已放弃、等迟到响应的槽位）上限。
constexpr std::size_t kMaxAbandonedSlots = 256;

// seq 分配的最大探测次数。正常情况一次就中；只有 2^32 回绕后的极端撞车才需要多试。
constexpr std::uint32_t kSeqProbeLimit = 65536;

// 空闲回收定时器的最短周期（毫秒）：太密的定时器纯粹浪费 loop 线程。
constexpr int kMinReapIntervalMs = 500;

// ---- 端点冷却（connect 级熔断）----
// 首次建连失败的冷却时长。3s 的量级是刻意的：比"一次调用超时"长得多（否则死实例
// 仍会被反复试探），又比"服务重启 + 重新注册"短（否则恢复得太慢）。
constexpr std::uint64_t kCooldownBaseMs = 3000;
// 连续失败时的退避上限。再长就等于永久拉黑，而我们要的是"自动恢复"。
constexpr std::uint64_t kCooldownMaxMs = 30000;

// 打出"某 endpoint 进入冷却"的日志/计数的间隔下限，避免死实例刷日志。
// （计数器本身照常累加；这个只约束日志行。）
constexpr std::uint64_t kCooldownLogIntervalMs = 10000;

void SetErr(std::string* err, std::string msg) {
  if (err != nullptr) *err = std::move(msg);
}

void SetFailure(AcquireFailure* failure, AcquireFailure value) {
  if (failure != nullptr) *failure = value;
}

// 端点冷却计数。按 label 取指针：endpoint 数量很少且这里是冷路径（失败才走），
// 不做缓存，避免为一个"只增不减"的指针表付内存。
// 迟到响应（调用方已放弃，响应才到）计数。
// channel.h 的指标清单里承诺过 qzrpc_client_late_response_total，之前只有注释、代码从未 Inc ——
// 这里把它接成真指标：这个值一旦持续增长，说明 request_timeout_ms 相对服务端 P99 太小，
// 或者链路上有丢包重传，是最省事的诊断信号之一。
void IncLateResponseCounter() {
  MetricsRegistry::Instance().GetCounter("qzrpc_client_late_response_total")->Inc();
}

void IncCooldownCounter(const std::string& endpoint) {
  const std::string name =
      std::string("qzrpc_client_endpoint_cooldown_total{endpoint=\"") + endpoint + "\"}";
  MetricsRegistry::Instance().GetCounter(name)->Inc();
}

// 回调与 ClientConnection 之间的弱引用桥。
//   * Connection 归 ClientConnection 持有；回调若捕获 shared_ptr<ClientConnection>
//     就是环（Connection → 回调 → ClientConnection → Connection），连接永不释放；
//   * 头文件里 ClientConnection 又不是 enable_shared_from_this，拿不到 weak_from_this；
//   * 于是把 weak_ptr<void> 放进 Connection 的 user context：context 归 Connection 持有，
//     弱引用不增加计数，环不存在，且回调能在对象已销毁时安全地变成 no-op。
struct OwnerBridge {
  std::weak_ptr<void> owner;
};

// 把「关闭一条连接」与「释放它」拆开：
//   * Close() 是线程安全的，会 ForceClose 并唤醒等待者；
//   * 释放必须回到 loop 线程，否则可能与正在派发的事件竞争。
// 用一个模板+lambda 封装，是为了让"最后一个引用"在 loop 线程的队列里被销毁。
void RetireLater(net::EventLoop* loop, const std::shared_ptr<void>& keep_alive) {
  if (!keep_alive) return;
  if (loop != nullptr && loop->running()) {
    // 闭包按值持有 shared_ptr：任务销毁时才真正释放，且一定在 loop 线程上发生。
    loop->QueueInLoop([keep_alive] { (void)keep_alive; });
  }
  // loop 已停：此时不可能有事件在派发，交给调用方就地释放（shared_ptr 析构）。
}

// 唤醒并清空等待表。模板化是为了在不暴露 ClientConnection 私有嵌套类型的前提下复用。
// 锁序约定：**先 pending_mutex_（表），再 PendingCall::mu（条目）**，全局唯一，不能反过来。
template <typename Map, typename Mutex>
void WakeAllWaiters(Map& pending, Mutex& mutex, const Status& status) {
  std::vector<typename Map::mapped_type> waiters;  // 元素本身就是 shared_ptr<PendingCall>
  {
    std::lock_guard<Mutex> lk(mutex);
    waiters.reserve(pending.size());
    for (auto& kv : pending) waiters.push_back(kv.second);
    pending.clear();
  }
  for (auto& waiter : waiters) {
    std::lock_guard<std::mutex> plk(waiter->mu);
    if (waiter->done) continue;  // 已经拿到结果的（含墓碑）不覆盖
    waiter->status = status;
    waiter->done = true;
    waiter->cv.notify_all();
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// ClientConnection
// ---------------------------------------------------------------------------

ClientConnection::ClientConnection(net::EventLoop* loop, const net::Endpoint& peer,
                                   std::uint32_t max_frame_size)
    : peer_(peer),
      max_frame_size_(max_frame_size == 0 ? protocol::kDefaultMaxFrameSize : max_frame_size) {
  (void)loop;  // loop 从 conn_ 上取（头文件没有留 loop_ 成员）；此处只为对齐签名语义。
  created_ms_ = TimeUtil::NowMs();
  last_used_ms_.store(created_ms_, std::memory_order_relaxed);
}

ClientConnection::Ptr ClientConnection::Create(net::EventLoop* loop, int fd,
                                               const net::Endpoint& peer,
                                               std::uint32_t max_frame_size) {
  if (loop == nullptr || fd < 0) {
    if (fd >= 0) net::SocketUtil::Close(fd);
    return nullptr;
  }

  Ptr conn(new ClientConnection(loop, peer, max_frame_size));

  // Connection 必须在这里建（构造 Attach 时 loop 是从 conn_ 反查的，先有鸡才能有蛋）。
  std::shared_ptr<net::Connection> raw =
      net::Connection::Create(loop, fd, peer.ToString());
  if (!raw) {
    net::SocketUtil::Close(fd);
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(conn->conn_mutex_);
    conn->conn_ = raw;
  }
  // 桥必须在 Create 里搭好（这里才有 shared_ptr<ClientConnection>），Attach 只消费它。
  std::shared_ptr<OwnerBridge> bridge = std::make_shared<OwnerBridge>();
  bridge->owner = conn;
  raw->set_context(bridge);

  if (!conn->Attach(fd)) {
    conn->Close();
    // 注意：这里不能只 return nullptr —— 让 conn 立刻析构会把 fd 泄漏给池外。
    // Close() 已经把 fd 交给 Connection 关闭，conn 析构时会 ForceClose 兜底。
    return nullptr;
  }
  conn->healthy_.store(true, std::memory_order_relaxed);
  return conn;
}

ClientConnection::~ClientConnection() {
  std::shared_ptr<net::Connection> raw;
  {
    std::lock_guard<std::mutex> lk(conn_mutex_);
    raw.swap(conn_);
  }
  if (raw) {
    closed_.store(true, std::memory_order_release);
    net::EventLoop* lp = raw->loop();
    if (lp != nullptr && lp->running()) {
      // 先摘回调再关，且**延迟一轮**执行：如果此刻正处在某个回调的调用栈里，
      // 立即销毁 std::function 就是 use-after-free。
      lp->QueueInLoop([raw] {
        raw->SetMessageCallback(nullptr);
        raw->SetCloseCallback(nullptr);
        raw->ForceClose();
      });
    } else {
      raw->SetMessageCallback(nullptr);
      raw->SetCloseCallback(nullptr);
      raw->ForceClose();
    }
  }
  WakeAllWaiters(pending_, pending_mutex_, Status::Cancelled("client connection destroyed"));
}

bool ClientConnection::Attach(int fd) {
  std::shared_ptr<net::Connection> raw;
  {
    std::lock_guard<std::mutex> lk(conn_mutex_);
    raw = conn_;
  }
  if (!raw || fd < 0) return false;

  // 回调不能捕获 shared_ptr<ClientConnection>（环，见 OwnerBridge 的注释）。
  // 桥由 Create 放在 Connection 的 user context 里，这里取出来用。
  std::shared_ptr<OwnerBridge> bridge = std::static_pointer_cast<OwnerBridge>(raw->context());
  if (!bridge || bridge->owner.expired()) {
    QZ_LOG_ERROR << "[Client] attach failed: owner bridge missing for " << peer_.ToString();
    return false;
  }

  raw->SetMessageCallback([bridge](const std::shared_ptr<net::Connection>& conn, Buffer* input) {
    if (std::shared_ptr<void> owner = bridge->owner.lock()) {
      std::static_pointer_cast<ClientConnection>(owner)->HandleBytes(conn, input);
    }
  });
  raw->SetCloseCallback([bridge](const std::shared_ptr<net::Connection>& conn) {
    if (std::shared_ptr<void> owner = bridge->owner.lock()) {
      std::static_pointer_cast<ClientConnection>(owner)->HandleClosed(conn);
    }
  });

  // 开始读。SetReadEnabled 只能在 loop 线程调用 → 从建连线程投递过去。
  net::EventLoop* lp = raw->loop();
  if (lp == nullptr) return false;
  std::shared_ptr<net::Connection> keep = raw;
  lp->RunInLoop([keep] { keep->SetReadEnabled(true); });
  return true;
}

void ClientConnection::HandleBytes(const std::shared_ptr<net::Connection>& conn, Buffer* input) {
  (void)conn;
  while (input->ReadableBytes() >= protocol::kFrameHeaderSize) {
    protocol::Frame frame;
    std::string err;
    const protocol::Codec::Result result =
        protocol::Codec::Decode(input, &frame, max_frame_size_, &err);
    if (result == protocol::Codec::Result::kNeedMore) return;  // 半包，等下一次可读

    if (result == protocol::Codec::Result::kError) {
      // 协议错误不可重同步（codec.h 的契约），只能断开。
      QZ_LOG_WARN << "[Client] protocol error from " << peer_.ToString() << ": " << err;
      Close();
      return;
    }

    if (!frame.header.is_response()) {
      QZ_LOG_WARN << "[Client] unexpected non-response frame from " << peer_.ToString()
                  << " seq=" << frame.header.seq << ", dropped";
      continue;
    }

    // ★ 响应按 seq 配对，与到达顺序无关。这里**不得**假设"下一个响应就是最早那个请求"：
    //   服务端 worker 池并发执行，谁先算完谁先回，乱序是正常现象。
    std::shared_ptr<PendingCall> waiter;
    {
      std::lock_guard<std::mutex> lk(pending_mutex_);
      auto it = pending_.find(frame.header.seq);
      if (it == pending_.end()) {
        QZ_LOG_DEBUG << "[Client] response for unknown seq=" << frame.header.seq << " ("
                     << peer_.ToString() << "), dropped";
        continue;
      }
      waiter = it->second;
      // 槽位的生命周期在这一刻结束（正常响应或迟到响应都走这里）。
      pending_.erase(it);
    }

    const Status status(static_cast<StatusCode>(frame.header.status), std::string());
    {
      std::lock_guard<std::mutex> lk(waiter->mu);
      if (waiter->done) {
        // 墓碑：调用方早已超时放弃，这一帧迟到响应就地丢弃。
        IncLateResponseCounter();
        QZ_LOG_DEBUG << "[Client] late response for abandoned seq=" << frame.header.seq
                     << " dropped (" << peer_.ToString() << ")";
        continue;
      }
      waiter->status = status;
      waiter->payload = std::move(frame.payload);
      waiter->metadata = std::move(frame.metadata);
      waiter->done = true;
      waiter->cv.notify_all();
    }

    // 收到合法响应说明链路是通的：清掉连续失败计数，恢复健康标记。
    consecutive_failures_.store(0, std::memory_order_relaxed);
    healthy_.store(true, std::memory_order_relaxed);
  }
}

void ClientConnection::HandleClosed(const std::shared_ptr<net::Connection>& conn) {
  (void)conn;
  healthy_.store(false, std::memory_order_relaxed);
  consecutive_failures_.fetch_add(1, std::memory_order_relaxed);
  closed_.store(true, std::memory_order_release);
  // ★ 唤醒全部等待者：否则它们会一路睡到自己的 deadline，把"断连"表现成"慢"。
  WakeAllWaiters(pending_, pending_mutex_, Status::Cancelled("connection closed by peer"));
  QZ_LOG_DEBUG << "[Client] connection closed: " << peer_.ToString();
}

Status ClientConnection::Call(std::uint32_t method_id, const std::string& payload,
                              const protocol::Metadata& meta, int timeout_ms,
                              std::string* resp_payload, protocol::Metadata* resp_metadata) {
  if (resp_payload != nullptr) resp_payload->clear();
  if (resp_metadata != nullptr) resp_metadata->Clear();
  if (timeout_ms <= 0) return Status::InvalidArgument("timeout_ms must be > 0");
  if (closed_.load(std::memory_order_acquire)) {
    return Status::Cancelled("connection already closed");
  }

  // ---- 1) 先占槽位，再发字节。顺序反了就会丢"极快响应"。----
  std::shared_ptr<PendingCall> waiter = std::make_shared<PendingCall>();
  std::uint32_t seq = 0;
  {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    for (std::uint32_t probe = 0; probe < kSeqProbeLimit; ++probe) {
      const std::uint32_t candidate = next_seq_.fetch_add(1, std::memory_order_relaxed);
      if (candidate == 0) continue;  // 0 保留（表示"不关心响应"）
      if (pending_.find(candidate) != pending_.end()) continue;  // 2^32 回绕后的撞车
      seq = candidate;
      break;
    }
    if (seq == 0) return Status::Overload("seq space exhausted on this connection");
    pending_[seq] = waiter;
    // 在表锁内复查连接状态：关闭路径也在表锁内唤醒等待者，
    // 因此"先登记再判关闭"可以保证不会出现无人唤醒的槽位。
    if (closed_.load(std::memory_order_acquire)) {
      pending_.erase(seq);
      return Status::Cancelled("connection closed before send");
    }
  }

  // ---- 2) 编码。失败说明一个字节都还没发出去（before-send，调用方可安全重试）。----
  protocol::Frame frame;
  frame.Reset(seq, method_id, true);
  frame.header.flags =
      static_cast<std::uint8_t>(protocol::kFlagRequest | protocol::kFlagKeepAlive);
  frame.metadata = meta;
  frame.payload = payload;

  Buffer wire;
  if (!protocol::Codec::Encode(&frame, &wire, max_frame_size_)) {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    pending_.erase(seq);
    return Status::SerializeError("encode request frame failed (seq=" + std::to_string(seq) +
                                  ", payload=" + std::to_string(payload.size()) + "B)");
  }

  std::shared_ptr<net::Connection> raw;
  {
    std::lock_guard<std::mutex> lk(conn_mutex_);
    raw = conn_;
  }
  if (!raw) {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    pending_.erase(seq);
    return Status::Cancelled("connection not attached");
  }

  // ---- 3) 发送（线程安全，内部会转到 loop 线程并做一次拷贝）----
  raw->Send(wire.Peek(), wire.ReadableBytes());

  // ---- 4) 阻塞等待。超时只影响**本次等待**，不影响槽位（见下文）。----
  {
    std::unique_lock<std::mutex> lk(waiter->mu);
    waiter->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&waiter] { return waiter->done; });
    if (waiter->done) {
      const Status status = waiter->status;
      if (status.ok()) {
        if (resp_payload != nullptr) *resp_payload = std::move(waiter->payload);
        if (resp_metadata != nullptr) *resp_metadata = std::move(waiter->metadata);
      }
      lk.unlock();
      if (!status.ok()) {
        // 失败时清空输出，避免调用方误用半成品。
        if (resp_payload != nullptr) resp_payload->clear();
        if (resp_metadata != nullptr) resp_metadata->Clear();
        if (status.code() == StatusCode::kTimeout ||
            status.code() == StatusCode::kCancelled) {
          healthy_.store(false, std::memory_order_relaxed);
          consecutive_failures_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      return status;
    }

    // 超时：**不移除槽位**，就地把它变成墓碑（done=true），等迟到响应来丢弃。
    waiter->status =
        Status::Timeout("no response within " + std::to_string(timeout_ms) + "ms from " +
                        peer_.ToString());
    waiter->done = true;
    lk.unlock();
  }

  healthy_.store(false, std::memory_order_relaxed);
  consecutive_failures_.fetch_add(1, std::memory_order_relaxed);

  // 墓碑过多 → 这条连接在丢响应，主动关掉重建（顺带清空所有墓碑）。
  std::size_t abandoned = 0;
  bool over_limit = false;
  {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    for (auto& kv : pending_) {
      std::lock_guard<std::mutex> plk(kv.second->mu);
      if (kv.second->done) ++abandoned;
    }
    over_limit = abandoned > kMaxAbandonedSlots;
  }
  if (over_limit) {
    QZ_LOG_WARN << "[Client] " << abandoned << " abandoned seq slot(s) on " << peer_.ToString()
                << ", closing connection to rebuild";
    Close();
  }

  return Status::Timeout("no response within " + std::to_string(timeout_ms) + "ms from " +
                         peer_.ToString());
}

void ClientConnection::Close() {
  if (closed_.exchange(true, std::memory_order_acq_rel)) return;  // 幂等
  healthy_.store(false, std::memory_order_relaxed);

  std::shared_ptr<net::Connection> raw;
  {
    std::lock_guard<std::mutex> lk(conn_mutex_);
    raw = conn_;
  }
  if (raw) raw->ForceClose();  // loop 线程会回调 HandleClosed（正常路径）

  // 兜底：若 loop 已停或 ForceClose 被丢弃，回调不会来，这里直接唤醒等待者。
  WakeAllWaiters(pending_, pending_mutex_, Status::Cancelled("connection closed"));
}

net::EventLoop* ClientConnection::loop() const noexcept {
  // conn_mutex_ 在头文件里已声明为 mutable：只读查询也要加锁，但加锁不改变可观察状态。
  std::lock_guard<std::mutex> lk(conn_mutex_);
  return conn_ ? conn_->loop() : nullptr;
}

std::uint64_t ClientConnection::last_used_ms() const noexcept {
  return last_used_ms_.load(std::memory_order_relaxed);
}

void ClientConnection::Touch(std::uint64_t now_ms) noexcept {
  last_used_ms_.store(now_ms, std::memory_order_relaxed);
}

std::size_t ClientConnection::inflight() const noexcept {
  // 只数"真正在等"的槽位：墓碑不算在飞（它们的调用方已经走了）。
  std::lock_guard<std::mutex> lk(pending_mutex_);
  std::size_t count = 0;
  for (const auto& kv : pending_) {
    std::lock_guard<std::mutex> plk(kv.second->mu);
    if (!kv.second->done) ++count;
  }
  return count;
}

bool ClientConnection::healthy() const noexcept {
  return healthy_.load(std::memory_order_relaxed) && !closed_.load(std::memory_order_relaxed);
}

void ClientConnection::MarkHealthy() noexcept {
  healthy_.store(true, std::memory_order_relaxed);
  consecutive_failures_.store(0, std::memory_order_relaxed);
}

void ClientConnection::MarkUnhealthy() noexcept {
  healthy_.store(false, std::memory_order_relaxed);
}

std::uint32_t ClientConnection::consecutive_failures() const noexcept {
  return consecutive_failures_.load(std::memory_order_relaxed);
}

bool ClientConnection::closed() const noexcept { return closed_.load(std::memory_order_acquire); }

// ---------------------------------------------------------------------------
// ConnectionPool
// ---------------------------------------------------------------------------

struct ConnectionPool::Impl {
  // 一个 endpoint 的连接账本。
  //   idle   —— 可立即借出
  //   busy   —— 借出中（池持有强引用，所以调用方忘了归还会体现为"池满且借不到"，
  //             而不是静默的野指针；配合 RAII Lease 实际不会发生）
  //   connecting —— 正在建连的数量（参与上限计数，避免并发建连冲破上限）
  struct Entry {
    std::vector<ClientConnection::Ptr> idle;
    std::vector<ClientConnection::Ptr> busy;
    std::size_t connecting{0};
    std::size_t total() const noexcept { return idle.size() + busy.size() + connecting; }

    // ---- 被动健康检查（connect 级熔断）----
    // cooldown_until_ms == 0 表示"不在冷却"。用单调时钟（TimeUtil::NowMs），
    // 不受系统时间调整影响（否则一次校时可能让冷却变成几小时）。
    std::uint64_t cooldown_until_ms{0};
    // 当前退避长度（毫秒）。连续失败翻倍，成功清零。
    std::uint64_t cooldown_backoff_ms{0};
    // 连续建连失败次数（仅用于日志与分级告警）。
    std::uint32_t connect_failures{0};
    // 上次打冷却日志的时间（限流日志，不影响计数）。
    std::uint64_t last_cooldown_log_ms{0};
  };

  Impl(net::EventLoop* event_loop, const ClientOptions& opts)
      : loop(event_loop), options(opts) {}

  // 取（必要时创建）某 endpoint 的账本。**条目永不 erase**：其它线程可能正持有
  // Entry& 在 cv 上等待，erase 会让它们拿到野引用。
  Entry& EntryFor(const std::string& key) {
    auto it = entries.find(key);
    if (it == entries.end()) it = entries.emplace(key, Entry()).first;
    return it->second;
  }

  // 建连失败 → 打开/延长冷却。必须在持有 mutex 时调用（读写 Entry）。
  // 返回本次冷却时长（毫秒），供日志使用。
  std::uint64_t MarkConnectFailure(Entry& entry, const std::string& key) {
    ++entry.connect_failures;
    // 3s → 6s → 12s → 24s → 30s（封顶）。首次是 3s，不是 2*3s。
    entry.cooldown_backoff_ms =
        entry.cooldown_backoff_ms == 0 ? kCooldownBaseMs : entry.cooldown_backoff_ms * 2;
    if (entry.cooldown_backoff_ms > kCooldownMaxMs) entry.cooldown_backoff_ms = kCooldownMaxMs;
    const std::uint64_t now = TimeUtil::NowMs();
    entry.cooldown_until_ms = now + entry.cooldown_backoff_ms;
    const bool log_now = entry.last_cooldown_log_ms == 0 ||
                         (now >= entry.last_cooldown_log_ms &&
                          now - entry.last_cooldown_log_ms >= kCooldownLogIntervalMs);
    if (log_now) {
      entry.last_cooldown_log_ms = now;
      QZ_LOG_WARN << "[ClientPool] endpoint " << key << " entered cooldown for "
                  << entry.cooldown_backoff_ms << "ms after " << entry.connect_failures
                  << " consecutive connect failure(s)";
    }
    // 计数在锁内取名字，代价可忽略（失败路径，且 endpoint 数量是个位数）。
    IncCooldownCounter(key);
    return entry.cooldown_backoff_ms;
  }

  // 连接成功（新建成功或健康连接归还）→ 立刻清零冷却。
  // 这一条是"冷却"与"永久拉黑"的分界：不清零，网络抖动恢复后健康实例会长期背锅。
  void MarkEndpointHealthy(Entry& entry) {
    entry.cooldown_until_ms = 0;
    entry.cooldown_backoff_ms = 0;
    entry.connect_failures = 0;
  }

  // 把一条连接从"池的账本"里摘下来并安全销毁。
  void Retire(ClientConnection::Ptr conn) {
    if (!conn) return;
    conn->Close();
    // 关键：最后一个引用必须在 loop 线程释放（可能与事件派发并发）。
    const std::shared_ptr<void> keep = conn;
    conn.reset();
    RetireLater(loop, keep);
  }

  // 空闲连接回收（loop 线程定时器回调）。
  void ReapIdle() {
    if (options.idle_connection_timeout_ms <= 0) return;
    const std::uint64_t now = TimeUtil::NowMs();
    const std::uint64_t limit = static_cast<std::uint64_t>(options.idle_connection_timeout_ms);
    std::vector<ClientConnection::Ptr> dead;
    {
      std::lock_guard<std::mutex> lk(mutex);
      if (stopped) return;
      for (auto& kv : entries) {
        std::vector<ClientConnection::Ptr>& idle = kv.second.idle;
        for (auto it = idle.begin(); it != idle.end();) {
          const ClientConnection::Ptr& cand = *it;
          const std::uint64_t last = cand ? cand->last_used_ms() : 0;
          const bool stale = (now >= last) && (now - last) > limit;
          if (!cand || cand->closed() || !cand->healthy() || stale) {
            dead.push_back(std::move(*it));
            it = idle.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    for (auto& one : dead) Retire(std::move(one));
    if (!dead.empty()) {
      QZ_LOG_DEBUG << "[ClientPool] reaped " << dead.size() << " idle connection(s)";
    }
  }

  net::EventLoop* loop{nullptr};
  ClientOptions options;

  mutable std::mutex mutex;
  std::condition_variable cv;

  // key = endpoint 文本（"ip:port"）。**条目永不 erase**（只清空内容）：
  // 其它线程可能正持有 Entry& 在 cv 上等待，erase 会让它们拿到野引用。
  std::unordered_map<std::string, Entry> entries;

  std::size_t pending_acquire{0};
  bool started{false};
  bool stopped{false};
  net::TimerId idle_timer{0};
};

const char* AcquireFailureName(AcquireFailure failure) noexcept {
  // 名字是 metrics label / 日志字段的一部分：改了就是破坏看板，别随手改。
  switch (failure) {
    case AcquireFailure::kNone:
      return "none";
    case AcquireFailure::kTimeout:
      return "timeout";
    case AcquireFailure::kPoolFull:
      return "pool_full";
    case AcquireFailure::kConnectFailed:
      return "connect_failed";
    case AcquireFailure::kStopped:
      return "stopped";
  }
  return "unknown";
}

ConnectionPool::ConnectionPool(net::EventLoop* loop, const ClientOptions& options)
    : impl_(new Impl(loop, options)) {
  if (loop == nullptr) {
    QZ_LOG_ERROR << "[ClientPool] constructed with a null event loop; Acquire will fail";
  }
}

ConnectionPool::~ConnectionPool() { Stop(); }

bool ConnectionPool::Start(std::string* err) {
  Impl& im = *impl_;
  net::EventLoop* loop = nullptr;
  int reap_interval_ms = 0;
  {
    std::lock_guard<std::mutex> lk(im.mutex);
    if (im.started) return true;
    if (im.stopped) {
      SetErr(err, "connection pool has been stopped");
      return false;
    }
    if (im.loop == nullptr) {
      SetErr(err, "connection pool requires a non-null event loop");
      return false;
    }
    im.started = true;
    loop = im.loop;
    if (im.options.idle_connection_timeout_ms > 0) {
      reap_interval_ms =
          std::max(kMinReapIntervalMs, std::min(im.options.idle_connection_timeout_ms, 10000));
    }
  }

  if (reap_interval_ms > 0) {
    Impl* raw = impl_.get();
    const net::TimerId id = loop->RunEvery(static_cast<std::uint64_t>(reap_interval_ms),
                                           [raw] { raw->ReapIdle(); });
    std::lock_guard<std::mutex> lk(im.mutex);
    im.idle_timer = id;
  }
  return true;
}

void ConnectionPool::Stop() {
  Impl& im = *impl_;
  std::vector<ClientConnection::Ptr> all;
  net::EventLoop* loop = nullptr;
  net::TimerId timer = 0;
  {
    std::lock_guard<std::mutex> lk(im.mutex);
    if (im.stopped) return;
    im.stopped = true;
    loop = im.loop;
    timer = im.idle_timer;
    for (auto& kv : im.entries) {
      for (auto& c : kv.second.idle) all.push_back(std::move(c));
      for (auto& c : kv.second.busy) all.push_back(std::move(c));
      kv.second.idle.clear();
      kv.second.busy.clear();
      kv.second.connecting = 0;
    }
    im.entries.clear();
  }

  // 唤醒所有还在等连接的调用方（它们在 cv 上，靠 stopped 标记退出）。
  im.cv.notify_all();
  if (timer != 0 && loop != nullptr) loop->CancelTimer(timer);

  for (auto& conn : all) {
    if (!conn) continue;
    conn->Close();  // 唤醒阻塞在 ClientConnection::Call 里的线程（kCancelled）
    const std::shared_ptr<void> keep = conn;
    conn.reset();
    RetireLater(loop, keep);
  }
  if (!all.empty()) QZ_LOG_INFO << "[ClientPool] stopped, closed " << all.size() << " connection(s)";
}

ClientConnection::Ptr ConnectionPool::Acquire(const net::Endpoint& ep, int timeout_ms,
                                             std::string* err, AcquireFailure* failure) {
  Impl& im = *impl_;
  SetFailure(failure, AcquireFailure::kNone);
  const std::string key = ep.ToString();
  const std::size_t limit =
      im.options.connections_per_endpoint == 0 ? 1 : im.options.connections_per_endpoint;
  const std::uint64_t deadline =
      TimeUtil::NowMs() + static_cast<std::uint64_t>(timeout_ms > 0 ? timeout_ms : 1);

  // 判断"这次失败该归咎于 endpoint 吗"的门槛：预算是被上层 deadline 截断过的，
  // 只剩几毫秒时连一个健康实例也连不上。没有公平机会就不进冷却，
  // 否则一次临期调用会把好实例拉黑 3 秒，故障转移反而制造故障。
  constexpr std::uint64_t kMinFairConnectBudgetMs = 50;

  std::unique_lock<std::mutex> lk(im.mutex);
  for (;;) {
    if (im.stopped) {
      SetErr(err, "connection pool stopped");
      SetFailure(failure, AcquireFailure::kStopped);
      return nullptr;
    }
    if (im.loop == nullptr) {
      SetErr(err, "connection pool has no event loop");
      SetFailure(failure, AcquireFailure::kStopped);
      return nullptr;
    }

    Impl::Entry& entry = im.EntryFor(key);

    // 0) 冷却中：**立即失败**，一个字节都不发、一次 TCP 连接都不发起。
    //    这是"死实例不再拖慢每次调用"的全部关键 —— 省掉的正是 connect_timeout 那一整段。
    //    上层（channel.cpp）据此换一个 endpoint，而不是在这里干等。
    const std::uint64_t now0 = TimeUtil::NowMs();
    if (entry.cooldown_until_ms > now0) {
      const std::uint64_t remain = entry.cooldown_until_ms - now0;
      SetErr(err, "endpoint in cooldown, retry after " + std::to_string(remain) + "ms");
      // 归类仍然算 connect 失败：对上层而言语义就是"这条 endpoint 现在连不上，换一条"。
      SetFailure(failure, AcquireFailure::kConnectFailed);
      return nullptr;
    }

    // 1) 优先复用空闲连接。
    bool need_refetch = false;
    while (!entry.idle.empty()) {
      ClientConnection::Ptr cand = std::move(entry.idle.back());
      entry.idle.pop_back();
      const std::uint64_t now = TimeUtil::NowMs();
      const std::uint64_t last = cand ? cand->last_used_ms() : 0;
      const bool stale = im.options.idle_connection_timeout_ms > 0 && now >= last &&
                         (now - last) >
                             static_cast<std::uint64_t>(im.options.idle_connection_timeout_ms);
      if (!cand || cand->closed() || !cand->healthy() || stale) {
        // 死连接直接换掉，不打断本次借用；Retire 会 Close 并在 loop 线程释放。
        ClientConnection::Ptr dead = std::move(cand);
        lk.unlock();
        im.Retire(std::move(dead));
        lk.lock();
        // Retire 期间 entries 可能被 Stop() 清空 → `entry` 引用可能已失效，
        // 必须回到外层循环重新取一次（这条比"少一次重试"重要得多）。
        need_refetch = true;
        break;
      }
      cand->Touch(now);
      entry.busy.push_back(cand);
      return cand;
    }
    if (need_refetch) continue;

    // 2) 未达上限 → 建新连接。**建连必须在锁外做**（会阻塞几十到几百毫秒）。
    if (entry.total() < limit) {
      ++entry.connecting;
      const std::uint64_t now = TimeUtil::NowMs();
      std::uint64_t budget = deadline > now ? deadline - now : 1;
      if (im.options.connect_timeout_ms > 0 &&
          budget > static_cast<std::uint64_t>(im.options.connect_timeout_ms)) {
        budget = static_cast<std::uint64_t>(im.options.connect_timeout_ms);
      }
      lk.unlock();

      const std::uint64_t connect_start_ms = TimeUtil::NowMs();
      std::string connect_err;
      const int fd = net::SocketUtil::ConnectWithTimeout(
          ep, static_cast<int>(budget > 60000 ? 60000 : budget), &connect_err);
      ClientConnection::Ptr fresh;
      if (fd >= 0) {
        fresh = ClientConnection::Create(im.loop, fd, ep, im.options.max_frame_size);
      }
      const std::uint64_t connect_used_ms = TimeUtil::NowMs() - connect_start_ms;

      lk.lock();
      auto entry_it = im.entries.find(key);
      if (entry_it == im.entries.end()) {
        // Stop() 在建连期间清过表：这条连接不要了。
        if (fresh) fresh->Close();
        SetErr(err, "connection pool stopped during connect");
        SetFailure(failure, AcquireFailure::kStopped);
        return nullptr;
      }
      Impl::Entry& back = entry_it->second;
      if (back.connecting > 0) --back.connecting;
      if (!fresh) {
        if (connect_err.empty()) connect_err = "connect to " + key + " failed";
        // 预算是不是被这次建连吃光了？吃光了才是 kTimeout（"预算用尽"），
        // 否则是 kConnectFailed（"对端拒绝/不可达"）。语义差别见 connection_pool.h 的表。
        const bool budget_exhausted = connect_used_ms + 1 >= budget;
        // 只有"给了公平机会"的失败才归咎 endpoint 并打开冷却；
        // 预算本来就被上层 deadline 削到只剩几毫秒时，连不上不能怪它。
        if (budget >= kMinFairConnectBudgetMs) {
          im.MarkConnectFailure(back, key);
        }
        im.cv.notify_all();
        SetErr(err, connect_err + " (after " + std::to_string(connect_used_ms) + "ms, budget " +
                        std::to_string(budget) + "ms)");
        SetFailure(failure,
                   budget_exhausted ? AcquireFailure::kTimeout : AcquireFailure::kConnectFailed);
        return nullptr;
      }
      if (im.stopped) {
        ClientConnection::Ptr drop = fresh;
        lk.unlock();
        im.Retire(std::move(drop));
        SetErr(err, "connection pool stopped during connect");
        SetFailure(failure, AcquireFailure::kStopped);
        return nullptr;
      }
      // ★ 建连成功：立刻清零冷却与退避（"冷却"与"永久拉黑"的分界线）。
      im.MarkEndpointHealthy(back);
      back.busy.push_back(fresh);
      return fresh;
    }

    // 3) 达到上限：等别人归还，直到 deadline。**这是 cv 等待，不是忙等。**
    const std::uint64_t now = TimeUtil::NowMs();
    if (now >= deadline) {
      SetErr(err, "acquire timeout after " + std::to_string(timeout_ms) + "ms (endpoint " + key +
                      ", limit " + std::to_string(limit) + " connection(s) all busy)");
      // 池满 ≠ 下游不可用：上限是本进程自己的配置，重试只会把争抢放大。
      // 上层据此映射成 kOverload（让调用方退避），而不是去换实例。
      SetFailure(failure, AcquireFailure::kPoolFull);
      return nullptr;
    }
    const std::uint64_t wait_ms = deadline - now;
    ++im.pending_acquire;
    im.cv.wait_for(lk, std::chrono::milliseconds(wait_ms), [&im, &key, limit] {
      if (im.stopped) return true;
      auto found = im.entries.find(key);
      if (found == im.entries.end()) return false;
      const Impl::Entry& e = found->second;
      return !e.idle.empty() || e.total() < limit;
    });
    --im.pending_acquire;
  }
}

void ConnectionPool::Release(ClientConnection::Ptr conn) {
  if (!conn) return;
  Impl& im = *impl_;
  ClientConnection::Ptr retire;
  {
    std::lock_guard<std::mutex> lk(im.mutex);
    const std::string key = conn->peer().ToString();
    // ★ 必须先确认这条连接真的在 busy 列表里。
    //   以前"找不到也照样 idle.push_back"：一次重复 Release 就会让同一条连接在 idle 里出现两次，
    //   随后两个线程各拿到一个 shared_ptr 指向**同一个** ClientConnection —— 症状是
    //   "我收到了别人的响应"这类最难查的串话。找不到就丢弃（并吵一声），别静默入池。
    bool found = false;
    auto it = im.entries.find(key);
    if (it != im.entries.end()) {
      std::vector<ClientConnection::Ptr>& busy = it->second.busy;
      for (auto bit = busy.begin(); bit != busy.end(); ++bit) {
        if (bit->get() == conn.get()) {
          busy.erase(bit);
          found = true;
          break;
        }
      }
    }
    if (!found) {
      QZ_LOG_ERROR << "[ClientPool] Release() of a connection that is not borrowed from this pool: "
                   << key << " (double release? dropped to avoid two owners)";
      retire = std::move(conn);  // 锁外 Retire
    }

    // 三条"不该回到空闲队列"的判据：
    //   a. 池已停；
    //   b. 连接已关闭 / 不健康（可能还有半截响应在路上）；
    //   c. **还有在飞请求**（多路复用的连接上，别人可能正在等回包 ——
    //      这一条最容易漏，漏了就会出现"另一个线程拿到连接却收到别人的响应"）。
    if (found && (im.stopped || conn->closed() || !conn->healthy() || conn->inflight() > 0)) {
      retire = std::move(conn);
    } else if (found) {
      // 一条连接能健康归还，就是"这条 endpoint 现在可用"最硬的证据 → 清掉冷却。
      // （建连成功时也清一次；这里覆盖"建连成功后又被抖动""心跳式恢复"的情形。）
      auto idle_it = im.entries.find(key);
      if (idle_it == im.entries.end()) idle_it = im.entries.emplace(key, Impl::Entry()).first;
      im.MarkEndpointHealthy(idle_it->second);
      idle_it->second.idle.push_back(std::move(conn));
    }
    im.cv.notify_all();
  }
  if (retire) impl_->Retire(std::move(retire));
}

void ConnectionPool::Invalidate(ClientConnection::Ptr conn) {
  if (!conn) return;
  Impl& im = *impl_;
  {
    std::lock_guard<std::mutex> lk(im.mutex);
    const std::string key = conn->peer().ToString();
    auto it = im.entries.find(key);
    if (it != im.entries.end()) {
      std::vector<ClientConnection::Ptr>& busy = it->second.busy;
      for (auto bit = busy.begin(); bit != busy.end(); ++bit) {
        if (bit->get() == conn.get()) {
          busy.erase(bit);
          break;
        }
      }
    }
  }
  // 找不到 busy 记录也照样 Retire：Invalidate 的语义是"这条别用了"，
  // 多余调用只会多关一次（Close 是幂等的），不会造成两个 owner。
  impl_->Retire(std::move(conn));  // 关闭 + 在 loop 线程释放，不进空闲队列
  impl_->cv.notify_all();
}

void ConnectionPool::RemoveEndpoint(const net::Endpoint& ep) {
  Impl& im = *impl_;
  std::vector<ClientConnection::Ptr> dead;
  {
    std::lock_guard<std::mutex> lk(im.mutex);
    auto it = im.entries.find(ep.ToString());
    if (it == im.entries.end()) return;
    // 空闲连接全部关掉；**借用中的不动**（那会把正在进行的调用变成 kCancelled）。
    for (auto& c : it->second.idle) dead.push_back(std::move(c));
    it->second.idle.clear();
  }
  for (auto& conn : dead) impl_->Retire(std::move(conn));
  QZ_LOG_INFO << "[ClientPool] endpoint removed: " << ep.ToString() << " (" << dead.size()
              << " idle connection(s) closed)";
}

std::size_t ConnectionPool::idle_count(const net::Endpoint& ep) const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  auto it = impl_->entries.find(ep.ToString());
  return it == impl_->entries.end() ? 0 : it->second.idle.size();
}

std::size_t ConnectionPool::total_count() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  std::size_t total = 0;
  for (const auto& kv : impl_->entries) total += kv.second.total();
  return total;
}

std::size_t ConnectionPool::pending_acquire_count() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->pending_acquire;
}

}  // namespace rpc
}  // namespace qzrpc
