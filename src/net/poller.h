// 轻舟 RPC (Qingzhou RPC)
// src/net/poller.h
//
// 职责：IO 多路复用的抽象接口。Linux 用 epoll，Windows 用 WSAPoll，
//       macOS 走 poll 兼容分支。平台差异**全部**收敛在 poller.cpp 里。
//
// 为什么选 WSAPoll 而不是 select：
//   * select 受 FD_SETSIZE（默认 64，需重编译改）限制，服务端要支撑上万连接直接用不了；
//   * WSAPoll 语义等价于 POSIX poll，无句柄数量上限，也不需要维护 fd_set 位图；
//   * 代价是每次 Wait 都要把 fd 数组整体拷进内核（O(n)），但 v1 的连接规模下可接受，
//     且接口设计已为将来换成 IOCP 留好位置（只替换 Poller 实现，不动上层）。
//
// 线程安全：**非线程安全**。一个 Poller 实例只归属一个 EventLoop 线程，
//           所有 Add/Mod/Del/Wait 必须在该线程调用。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/noncopyable.h"

namespace qzrpc {
namespace net {

// IO 事件多路复用器。
class Poller : private NonCopyable {
 public:
  // 关注的事件位（内部统一表示，与平台常量解耦）。
  enum Event : std::uint32_t {
    kNone = 0,
    kReadable = 0x01,  // 可读（含对端关闭，需在读回调里区分）
    kWritable = 0x02,  // 可写
    kError = 0x04,     // 错误（SO_ERROR 非零）
    kHup = 0x08,       // 对端挂断 / 句柄失效
  };

  // 一次 Wait 的就绪事件。
  struct EventEntry {
    int fd = -1;             // 就绪的文件描述符
    std::uint32_t events = 0;  // 就绪的事件位组合（本文件的 Event 枚举）
  };

  virtual ~Poller();

  // 创建当前平台最优的 Poller 实现。返回非空指针。
  // 调用方：EventLoop 构造函数。
  static std::unique_ptr<Poller> Create();

  // 注册新的 fd 及其关注事件。fd 已存在时返回 false。
  virtual bool Add(int fd, std::uint32_t events) = 0;

  // 修改关注事件。fd 不存在时返回 false。
  virtual bool Mod(int fd, std::uint32_t events) = 0;

  // 注销 fd。fd 不存在时返回 false。
  virtual bool Del(int fd) = 0;

  // 等待事件就绪。
  // 参数：active - 输出，就绪事件列表（会被清空后填充）；timeout_ms - 超时毫秒，
  //       <0 表示永久阻塞，0 表示立即返回。
  // 返回：就绪的 fd 数量；0 表示超时；<0 表示出错（可通过 LastError 排查，EventLoop 应终止）。
  virtual int Wait(std::vector<EventEntry>* active, int timeout_ms) = 0;

  // 当前已注册的 fd 数量（metrics / 调试用）。
  virtual std::size_t Size() const = 0;

  // 平台名（"epoll" / "wsa_poll" / "poll"），用于日志与自检。
  virtual const char* Name() const = 0;
};

}  // namespace net
}  // namespace qzrpc
