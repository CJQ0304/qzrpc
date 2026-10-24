// 轻舟 RPC (Qingzhou RPC)
// src/net/connection.cpp
//
// 职责：一条 TCP 连接的完整状态机（读缓冲 / 写缓冲 / 回调 / 关闭 / 空闲判定）。
//       它不认识任何 RPC 协议：进来的是字节，出去的也是字节。
//
// ★ 三处必须小心的地方：
//   1) HandleReadable 里的 message_cb_ 完全可能调 ForceClose() → CloseInLoop()。
//      因此本文件所有 Channel 回调的**第一行**都取一个 `auto self = shared_from_this()`，
//      保证回调期间对象不会被上层 erase 掉；回调返回后一律重查 state_，
//      绝不在 kClosed 状态下继续访问 fd_/buffers。
//   2) 关闭只能收敛一次：CloseInLoop 用 state_ 做闸门，fd_ 置 -1，close_cb_ 只触发一次。
//   3) 回调 lambda 一律捕获 weak_ptr：Connection 拥有 Channel，Channel 拥有回调，
//      回调若捕获 shared_ptr 就是引用环 → 连接永不释放。
//
// 线程模型：fd_/input_/output_/channel_/回调 只在 loop 线程访问；
//           state_/last_active_ms_/output_pending_bytes_/pending_ops_ 是原子量，跨线程可读。

#include "net/connection.h"

#include <utility>

#include "common/logger.h"
#include "common/time_util.h"
#include "net/channel.h"
#include "net/event_loop.h"
#include "net/platform.h"
#include "net/socket_util.h"

namespace qzrpc {
namespace net {

namespace {

// 单次 recv 的块大小：64KB 是内核收发缓冲的常见粒度，既能一次读走整帧，
// 又不会因为过大的栈缓冲触发栈探测开销。
constexpr std::size_t kReadChunk = 64 * 1024;

// 单次可读事件里最多做多少次 recv：防止一个极端繁忙的连接把 loop 线程长期占住。
// 用水平触发，所以没读完的数据会立刻再次上报，不会丢。
constexpr int kMaxReadsPerEvent = 16;

}  // namespace

std::shared_ptr<Connection> Connection::Create(EventLoop* loop, int fd,
                                               const std::string& peer_addr) {
  if (loop == nullptr || fd < 0) return nullptr;
  // 构造函数是私有的，所以不能直接用 make_shared。
  std::shared_ptr<Connection> conn(new Connection(loop, fd, peer_addr));
  conn->channel_.reset(new Channel(loop, fd));
  conn->input_.reset(new Buffer());
  conn->output_.reset(new Buffer());

  std::weak_ptr<Connection> weak = conn;
  conn->channel_->SetReadCallback([weak] {
    if (auto self = weak.lock()) self->HandleReadable();
  });
  conn->channel_->SetWriteCallback([weak] {
    if (auto self = weak.lock()) self->HandleWritable();
  });
  conn->channel_->SetErrorCallback([weak] {
    if (auto self = weak.lock()) self->HandleError();
  });
  conn->channel_->SetCloseCallback([weak] {
    if (auto self = weak.lock()) self->HandleHup();
  });

  Endpoint local;
  if (SocketUtil::GetLocalAddress(fd, &local)) conn->local_addr_ = local.ToString();

  conn->last_active_ms_.store(TimeUtil::NowMs(), std::memory_order_relaxed);
  return conn;
}

Connection::Connection(EventLoop* loop, int fd, std::string peer_addr)
    : loop_(loop), fd_(fd), peer_addr_(std::move(peer_addr)) {
  state_.store(static_cast<int>(State::kConnected), std::memory_order_relaxed);
}

Connection::~Connection() {
  // 正常情况下 CloseInLoop 已经把 fd 关掉并置 -1；这里兜底处理"未走关闭流程就被析构"。
  if (channel_) {
    channel_->DisableAll();
    channel_->Remove();
    channel_.reset();
  }
  if (fd_ >= 0) {
    SocketUtil::Close(fd_);
    fd_ = -1;
  }
}

int Connection::fd() const noexcept { return fd_; }

Connection::State Connection::state() const noexcept {
  return static_cast<State>(state_.load(std::memory_order_acquire));
}

bool Connection::connected() const noexcept { return state() == State::kConnected; }

void Connection::SetMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }

void Connection::SetCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }

void Connection::SetWriteCompleteCallback(WriteCompleteCallback cb) {
  write_complete_cb_ = std::move(cb);
}

void Connection::SetHighWaterMarkCallback(std::size_t mark, HighWaterMarkCallback cb) {
  high_water_mark_ = mark;
  high_water_cb_ = std::move(cb);
}

void Connection::SetReadEnabled(bool on) {
  if (!channel_) return;
  if (on) {
    if (state() == State::kConnected || state() == State::kDisconnecting) {
      channel_->EnableReading();
    }
    return;
  }
  channel_->DisableReading();
}

bool Connection::read_enabled() const noexcept {
  return channel_ != nullptr && channel_->reading();
}

void Connection::Send(const void* data, std::size_t len) {
  if (data == nullptr || len == 0) return;
  if (state() == State::kClosed) return;
  std::string copy(static_cast<const char*>(data), len);
  Send(std::move(copy));  // 复用 std::string 重载：非 loop 线程同样会转投并只拷贝这一次
}

void Connection::Send(const std::string& data) {
  if (data.empty()) return;
  if (state() == State::kClosed) return;
  EventLoop* loop = loop_;
  if (loop == nullptr) return;
  std::weak_ptr<Connection> weak = weak_from_this();
  if (loop->IsInLoopThread()) {
    SendInLoop(std::string(data));
    return;
  }
  // 非 loop 线程：把数据整体 move 进闭包（一次拷贝），到 loop 线程再落缓冲。
  loop->RunInLoop([weak, payload = std::string(data)]() mutable {
    if (auto self = weak.lock()) self->SendInLoop(std::move(payload));
  });
}

void Connection::Shutdown() {
  if (state() == State::kClosed) return;
  EventLoop* loop = loop_;
  if (loop == nullptr) return;
  std::weak_ptr<Connection> weak = weak_from_this();
  loop->RunInLoop([weak] {
    if (auto self = weak.lock()) self->ShutdownInLoop();
  });
}

void Connection::ForceClose() {
  if (state() == State::kClosed) return;
  EventLoop* loop = loop_;
  if (loop == nullptr) return;
  std::weak_ptr<Connection> weak = weak_from_this();
  loop->RunInLoop([weak] {
    if (auto self = weak.lock()) self->ForceCloseInLoop();
  });
}

std::uint64_t Connection::last_active_ms() const noexcept {
  return last_active_ms_.load(std::memory_order_relaxed);
}

std::size_t Connection::output_pending_bytes() const noexcept {
  return output_pending_bytes_.load(std::memory_order_relaxed);
}

void Connection::IncrementPendingOps() noexcept {
  pending_ops_.fetch_add(1, std::memory_order_relaxed);
}

void Connection::DecrementPendingOps() noexcept {
  pending_ops_.fetch_sub(1, std::memory_order_relaxed);
}

std::int64_t Connection::pending_ops() const noexcept {
  return pending_ops_.load(std::memory_order_relaxed);
}

void Connection::set_context(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }

std::shared_ptr<void> Connection::context() const { return context_; }

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------

void Connection::HandleReadable() {
  // 保命：message_cb_ 里可能 ForceClose 并让持有者 erase 掉本对象。
  const std::shared_ptr<Connection> self = shared_from_this();
  if (state() == State::kClosed) return;

  bool peer_closed = false;
  int read_error = 0;
  for (int round = 0; round < kMaxReadsPerEvent; ++round) {
    input_->EnsureWritable(kReadChunk);
    const long n = SocketUtil::Recv(fd_, input_->WriteBegin(), input_->WritableBytes());
    if (n > 0) {
      input_->HasWritten(static_cast<std::size_t>(n));
      last_active_ms_.store(TimeUtil::NowMs(), std::memory_order_relaxed);
      continue;  // 继续读，直到 EAGAIN（水平触发要求读干净）
    }
    if (n == 0) {
      peer_closed = true;  // 对端有序关闭（FIN）
      break;
    }
    const int err = SocketUtil::LastError();
    if (SocketUtil::WouldBlock(err)) break;  // 正常：没有更多数据
    read_error = err;
    break;
  }

  if (read_error != 0) {
    QZ_LOG_DEBUG << "connection[" << peer_addr_ << "]: recv error: "
                 << SocketUtil::ErrorString(read_error);
    CloseInLoop();
    return;
  }

  // 先把已经读到的数据处理掉，再关闭：否则最后一批请求会被丢掉。
  if (input_->ReadableBytes() > 0 && message_cb_) {
    message_cb_(self, input_.get());
    // ★ 回调可能关闭了连接，必须重查状态，且此后不得再访问 input_/output_/fd_。
    if (state() == State::kClosed) return;
  }

  if (peer_closed) {
    CloseInLoop();  // 对端已 FIN：本端也关闭（不做半关闭长连接，v1 无此需求）
    return;
  }

  if (state() == State::kDisconnecting && output_->ReadableBytes() == 0) {
    CloseInLoop();
  }
}

// ---------------------------------------------------------------------------
// 写
// ---------------------------------------------------------------------------

void Connection::HandleWritable() {
  const std::shared_ptr<Connection> self = shared_from_this();
  if (state() == State::kClosed) return;

  int write_error = 0;
  while (output_->ReadableBytes() > 0) {
    const long n = SocketUtil::Send(fd_, output_->Peek(), output_->ReadableBytes());
    if (n > 0) {
      output_->Retrieve(static_cast<std::size_t>(n));
      last_active_ms_.store(TimeUtil::NowMs(), std::memory_order_relaxed);
      continue;
    }
    if (n == 0) break;
    const int err = SocketUtil::LastError();
    if (SocketUtil::WouldBlock(err)) break;
    write_error = err;
    break;
  }
  output_pending_bytes_.store(output_->ReadableBytes(), std::memory_order_relaxed);

  if (write_error != 0) {
    QZ_LOG_DEBUG << "connection[" << peer_addr_ << "]: send error: "
                 << SocketUtil::ErrorString(write_error);
    CloseInLoop();
    return;
  }

  if (output_->ReadableBytes() > 0) return;  // 还没发完，等下一次可写事件

  if (channel_) channel_->DisableWriting();  // 必须关掉，否则会 busy loop
  high_water_notified_.store(false, std::memory_order_relaxed);

  if (write_complete_cb_) {
    write_complete_cb_(self);
    if (state() == State::kClosed) return;
  }
  if (state() == State::kDisconnecting) CloseInLoop();
}

void Connection::HandleError() {
  if (state() == State::kClosed) return;
  int so_error = 0;
#ifdef _WIN32
  int len = static_cast<int>(sizeof(so_error));
#else
  socklen_t len = static_cast<socklen_t>(sizeof(so_error));
#endif
  if (::getsockopt(static_cast<SocketHandle>(fd_), SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&so_error), &len) == 0 &&
      so_error != 0) {
    QZ_LOG_DEBUG << "connection[" << peer_addr_ << "]: socket error: "
                 << SocketUtil::ErrorString(so_error);
  }
  CloseInLoop();
}

void Connection::HandleHup() {
  if (state() == State::kClosed) return;
  CloseInLoop();
}

// ---------------------------------------------------------------------------
// loop 线程内的动作
// ---------------------------------------------------------------------------

void Connection::SendInLoop(std::string data) {
  if (state() == State::kClosed) return;
  if (!data.empty()) {
    output_->Append(data.data(), data.size());
    output_pending_bytes_.store(output_->ReadableBytes(), std::memory_order_relaxed);
    last_active_ms_.store(TimeUtil::NowMs(), std::memory_order_relaxed);
  }

  // 高水位：只在"跨过阈值"的那一刻回调一次，回落到阈值以下才重新武装。
  if (high_water_mark_ > 0 && output_->ReadableBytes() > high_water_mark_) {
    const bool already = high_water_notified_.exchange(true, std::memory_order_relaxed);
    if (!already && high_water_cb_) {
      high_water_cb_(shared_from_this(), output_->ReadableBytes());
      if (state() == State::kClosed) return;
    }
  }

  // 先就地尝试发送：小包通常一次 send 就能发完，省掉一整轮事件循环往返。
  int write_error = 0;
  while (output_->ReadableBytes() > 0) {
    const long n = SocketUtil::Send(fd_, output_->Peek(), output_->ReadableBytes());
    if (n > 0) {
      output_->Retrieve(static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) break;
    const int err = SocketUtil::LastError();
    if (SocketUtil::WouldBlock(err)) break;
    write_error = err;
    break;
  }
  output_pending_bytes_.store(output_->ReadableBytes(), std::memory_order_relaxed);

  if (write_error != 0) {
    QZ_LOG_DEBUG << "connection[" << peer_addr_ << "]: send error: "
                 << SocketUtil::ErrorString(write_error);
    CloseInLoop();
    return;
  }

  if (output_->ReadableBytes() > 0) {
    // 发不完 → 关注可写，剩余部分由 HandleWritable 继续发。
    if (channel_) channel_->EnableWriting();
    return;
  }

  if (channel_ && channel_->writing()) channel_->DisableWriting();
  high_water_notified_.store(false, std::memory_order_relaxed);

  if (write_complete_cb_ && !data.empty()) {
    write_complete_cb_(shared_from_this());
    if (state() == State::kClosed) return;
  }
  if (state() == State::kDisconnecting) CloseInLoop();
}

void Connection::ShutdownInLoop() {
  if (state() == State::kClosed) return;
  if (state() == State::kDisconnecting) return;
  state_.store(static_cast<int>(State::kDisconnecting), std::memory_order_release);
  // 语义：写完当前输出缓冲后关闭。缓冲已空就是立即关。
  if (output_->ReadableBytes() == 0) CloseInLoop();
}

void Connection::ForceCloseInLoop() { CloseInLoop(); }

void Connection::CloseInLoop() {
  if (state() == State::kClosed) return;  // 单次收敛闸门
  state_.store(static_cast<int>(State::kClosed), std::memory_order_release);

  if (channel_) {
    channel_->DisableAll();  // 先从 Poller 摘除：fd 关闭后不该再有任何事件上报
    channel_->Remove();
  }
  if (fd_ >= 0) {
    SocketUtil::Close(fd_);
    fd_ = -1;
  }
  if (output_) {
    output_->RetrieveAll();
    output_pending_bytes_.store(0, std::memory_order_relaxed);
  }

  if (!close_cb_) return;
  // 保命：close_cb_ 里持有者会从容器 erase，可能释放掉最后一个强引用。
  const std::shared_ptr<Connection> self = shared_from_this();
  const CloseCallback cb = close_cb_;
  EventLoop* loop = loop_;
  if (loop != nullptr) {
    // ★ 必须延迟到本次事件派发**之后**再回调。
    //   本函数极可能正跑在 Channel::HandleEvent 的调用栈里（HUP/错误/ForceClose 路径），
    //   同步回调会让持有者 erase 掉最后一个 shared_ptr → ~Connection → ~Channel，
    //   而 Channel::HandleEvent 返回时还要写 handling_event_，
    //   于是变成"写已释放的 Channel"（ASAN 实测 heap-use-after-free, WRITE of size 1）。
    //   延迟后：Connection（连同它的 Channel）由这个任务持有，直到派发栈退出才析构。
    loop->QueueInLoop([cb, self] {
      if (cb) cb(self);
    });
    return;
  }
  if (cb) cb(self);
}

}  // namespace net
}  // namespace qzrpc
