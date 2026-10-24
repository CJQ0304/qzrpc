// 轻舟 RPC (Qingzhou RPC)
// src/net/connection.h
//
// 职责：一条 TCP 连接的完整状态机 —— 读缓冲、写缓冲、事件回调、关闭流程、空闲判定。
//       它是 ClientConnection（客户端）与 ServerConnection（服务端）的公共底座，
//       本身**不认识任何 RPC 协议**：进来的是字节，出去的也是字节。
//
// 所有权契约（重要，实现者必须严格遵守）：
//   * Connection 由创建者以 std::shared_ptr 持有（Acceptor 回调 / ConnectionPool）；
//   * 关闭流程：Channel 收到 HUP/错误 → 调 CloseCallback → **由持有者在回调里从容器 erase**；
//     最后一个 shared_ptr 析构时释放 fd 与 Channel；
//   * 回调内部禁止直接 reset 掉自己持有的那个 shared_ptr（会导致 use-after-free），
//     必须先 ForceClose() 再让持有者 erase；
//   * 跨线程调用 Send/Shutdown/ForceClose 是安全的（内部 RunInLoop 转投）。
//
// 线程安全：Send / SendString / Shutdown / ForceClose / state / last_active_ms /
//           pending_ops 系列线程安全；回调设置与 read_enabled 只能在 loop 线程或建连前调用。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/buffer.h"
#include "common/noncopyable.h"

namespace qzrpc {
namespace net {

class Channel;
class EventLoop;

// 一条已建立（或正在建立）的 TCP 连接。
class Connection : public std::enable_shared_from_this<Connection>, private NonCopyable {
 public:
  // 连接状态。
  enum class State : int {
    kConnecting = 0,   // 正在建立（客户端 Connector 尚未完成）
    kConnected = 1,    // 已连接，可读写
    kDisconnecting = 2,  // 已调用 Shutdown，等待写缓冲排空后关闭
    kClosed = 3,       // 已关闭，fd 已释放
  };

  // 数据到达回调（loop 线程执行）。
  // 参数：conn - 自身；input - 读缓冲，回调必须把完整帧从中读走（Retrieve），
  //       未消费的字节会被保留到下次可读事件时继续追加。
  using MessageCallback = std::function<void(const std::shared_ptr<Connection>& conn, Buffer* input)>;

  // 连接关闭回调（loop 线程执行，收到 HUP/错误/ForceClose 后触发一次）。
  using CloseCallback = std::function<void(const std::shared_ptr<Connection>& conn)>;

  // 写缓冲排空回调（loop 线程执行）。连接池在此时认为连接可再次复用。
  using WriteCompleteCallback = std::function<void(const std::shared_ptr<Connection>& conn)>;

  // 写缓冲高水位回调，用于背压观测。
  using HighWaterMarkCallback =
      std::function<void(const std::shared_ptr<Connection>& conn, std::size_t pending_bytes)>;

  // 创建连接对象并接管 fd。
  // 参数：loop - 归属事件循环（非空）；fd - 已连接的非阻塞 fd（所有权移交）；
  //       peer - 对端地址文本（"ip:port"，仅用于日志与注册）。
  // 返回：非空 shared_ptr。此时状态为 kConnected，但**未开始读**（需 SetReadEnabled(true)）。
  static std::shared_ptr<Connection> Create(EventLoop* loop, int fd, const std::string& peer_addr);

  ~Connection();

  int fd() const noexcept;
  EventLoop* loop() const noexcept { return loop_; }

  // "1.2.3.4:8080"，建连时确定，不再变化。
  const std::string& peer_addr() const noexcept { return peer_addr_; }

  // 本地地址文本（Accept 后填充；客户端连接为本地随机端口）。
  const std::string& local_addr() const noexcept { return local_addr_; }

  // 设置本地地址文本（由创建者填充，仅建连期调用）。
  void set_local_addr(std::string addr) { local_addr_ = std::move(addr); }

  // 当前状态。线程安全。
  State state() const noexcept;

  // 是否可读写（kConnected）。线程安全。
  bool connected() const noexcept;

  // ---- 回调注册（建连后、SetReadEnabled(true) 之前完成） ----

  void SetMessageCallback(MessageCallback cb);
  void SetCloseCallback(CloseCallback cb);
  void SetWriteCompleteCallback(WriteCompleteCallback cb);

  // 设置高水位阈值与回调。pending_bytes 超过 mark 时触发一次（降到 mark 以下再超过才重新触发）。
  void SetHighWaterMarkCallback(std::size_t mark, HighWaterMarkCallback cb);

  // ---- 读写控制 ----

  // 开关读事件。false 时停止关注可读，内核接收窗口自然收窄形成背压。
  // 约束：只能在 loop 线程调用（或建连前）。
  void SetReadEnabled(bool on);

  // 当前是否关注可读。约束：只能在 loop 线程读取。
  bool read_enabled() const noexcept;

  // 发送数据。**线程安全**：非 loop 线程调用会被转投到 loop 线程并拷贝一次 data。
  // 参数：data/len - 待发送字节；调用返回即表示"已入队"，不代表已发出。
  void Send(const void* data, std::size_t len);

  // 发送字符串（同上，会拷贝）。
  void Send(const std::string& data);

  // 优雅关闭：写完当前输出缓冲后关闭。线程安全。
  void Shutdown();

  // 立即关闭：丢弃输出缓冲并关闭 fd。线程安全，幂等。
  void ForceClose();

  // ---- 观测 ----

  // 最近一次读或写活动的时间（单调时钟毫秒），用于空闲连接回收。线程安全。
  std::uint64_t last_active_ms() const noexcept;

  // 输出缓冲当前积压字节数（背压/过载观测）。线程安全。
  std::size_t output_pending_bytes() const noexcept;

  // 在飞业务数量（服务端：已派发未回包的请求数；客户端：已发出未收到响应的请求数）。
  // 由使用方维护，框架只在优雅停机时读取。线程安全。
  void IncrementPendingOps() noexcept;
  void DecrementPendingOps() noexcept;
  std::int64_t pending_ops() const noexcept;

  // 用户态上下文（服务端放 ConnState，客户端放 ClientConnection 需要的数据）。
  // 约束：只能在 loop 线程 set/get；不要在此存放需要析构期反注册的对象。
  void set_context(std::shared_ptr<void> ctx);
  std::shared_ptr<void> context() const;

 private:
  Connection(EventLoop* loop, int fd, std::string peer_addr);

  // Channel 回调（均在 loop 线程执行）。
  void HandleReadable();
  void HandleWritable();
  void HandleError();
  void HandleHup();

  // 在 loop 线程执行的实际动作。
  void SendInLoop(std::string data);
  void ShutdownInLoop();
  void ForceCloseInLoop();
  // 统一关闭：注销 Channel、关闭 fd、置状态、触发 CloseCallback。
  void CloseInLoop();

  EventLoop* loop_{nullptr};
  int fd_{-1};
  std::string peer_addr_;
  std::string local_addr_;

  std::unique_ptr<Channel> channel_;
  std::unique_ptr<Buffer> input_;
  std::unique_ptr<Buffer> output_;

  std::atomic<int> state_{static_cast<int>(State::kConnected)};
  std::atomic<std::uint64_t> last_active_ms_{0};
  std::atomic<std::size_t> output_pending_bytes_{0};
  std::atomic<std::int64_t> pending_ops_{0};
  std::atomic<bool> high_water_notified_{false};

  MessageCallback message_cb_;
  CloseCallback close_cb_;
  WriteCompleteCallback write_complete_cb_;
  HighWaterMarkCallback high_water_cb_;
  std::size_t high_water_mark_{0};

  std::shared_ptr<void> context_;
};

}  // namespace net
}  // namespace qzrpc
