// 轻舟 RPC (Qingzhou RPC)
// src/net/poller.cpp
//
// 职责：IO 多路复用的三种实现，平台差异全部收敛在本文件。
//   * Linux  → epoll（边缘语义用水平触发 LT，配合 Connection 读干净到 EAGAIN）
//   * Windows → WSAPoll（自维护 vector<WSAPOLLFD> + fd→index 表）
//   * 其它   → poll（POSIX 兼容分支）
//
// WSAPoll 的两个坑（本文件已处理）：
//   1) revents **不会**被 WSAPoll 清零，必须在每次 Wait 前手动清零，否则会一直
//      上报同一批"幽灵事件"；
//   2) POLLERR / POLLHUP / POLLNVAL 的语义与 epoll 不同：对端正常关闭（FIN）时
//      WSAPoll 只置 POLLRDNORM 而不置 POLLHUP，所以连接关闭要靠 recv 返回 0 判定
//      （见 Connection::HandleReadable）。POLLNVAL 表示句柄已失效，这里按 kError|kHup
//      上报，让上层走关闭流程而不是死等。

#include "net/poller.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/platform.h"

#if defined(__linux__)
#include <sys/epoll.h>
#endif

namespace qzrpc {
namespace net {

Poller::~Poller() = default;

#if defined(__linux__)

namespace {

// 内部事件位 <-> epoll 事件位。
// 注意：EPOLLRDHUP 需要 _GNU_SOURCE，glibc 默认定义；它表示对端半关闭（FIN），
// 必须映射到 kHup，否则 Connection 只能等到 recv 返回 0 的下一轮可读事件。
std::uint32_t ToEpollEvents(std::uint32_t events) {
  std::uint32_t out = 0;
  if ((events & Poller::kReadable) != 0) out |= EPOLLIN;
  if ((events & Poller::kWritable) != 0) out |= EPOLLOUT;
  if (out != 0) out |= EPOLLRDHUP;
  return out;
}

std::uint32_t FromEpollEvents(std::uint32_t events) {
  std::uint32_t out = 0;
  if ((events & (EPOLLIN | EPOLLPRI)) != 0) out |= Poller::kReadable;
  if ((events & EPOLLOUT) != 0) out |= Poller::kWritable;
  if ((events & EPOLLERR) != 0) out |= Poller::kError;
  if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0) out |= Poller::kHup;
  return out;
}

}  // namespace

class EpollPoller final : public Poller {
 public:
  EpollPoller() { epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC); }

  ~EpollPoller() override {
    if (epoll_fd_ >= 0) {
      ::close(epoll_fd_);
      epoll_fd_ = -1;
    }
  }

  bool Add(int fd, std::uint32_t events) override {
    if (epoll_fd_ < 0 || fd < 0) return false;
    if (registered_.count(fd) != 0) return false;
    epoll_event ev;
    ZeroEvent(&ev);
    ev.events = ToEpollEvents(events);
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
    registered_.insert(fd);
    return true;
  }

  bool Mod(int fd, std::uint32_t events) override {
    if (epoll_fd_ < 0 || fd < 0) return false;
    if (registered_.count(fd) == 0) return false;
    epoll_event ev;
    ZeroEvent(&ev);
    ev.events = ToEpollEvents(events);
    ev.data.fd = fd;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
  }

  bool Del(int fd) override {
    if (epoll_fd_ < 0 || fd < 0) return false;
    const auto it = registered_.find(fd);
    if (it == registered_.end()) return false;
    // 内核侧 EPOLL_CTL_DEL 失败也要把本地表清掉：fd 可能已被 close（此时内核自动摘除）。
    epoll_event ev;
    ZeroEvent(&ev);
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev);
    registered_.erase(it);
    return true;
  }

  int Wait(std::vector<EventEntry>* active, int timeout_ms) override {
    if (active == nullptr) return -1;
    active->clear();
    if (epoll_fd_ < 0) return -1;
    if (registered_.empty()) {
      // 没有注册任何 fd：语义上等价于"睡到超时"。epoll_wait 本身支持这样用，
      // 但 timeout < 0 时会永久阻塞且没有任何事件能唤醒它，极易变成静默挂死，
      // 因此这里退化为一次短睡并返回 0（EventLoop 会重算超时）。
      if (timeout_ms < 0) {
        ::poll(nullptr, 0, 50);
      } else if (timeout_ms > 0) {
        ::poll(nullptr, 0, timeout_ms);
      }
      return 0;
    }
    epoll_event events[kMaxEventsPerWait];
    const int n = ::epoll_wait(epoll_fd_, events, kMaxEventsPerWait, timeout_ms);
    if (n < 0) return -1;  // EINTR / EBADF 由 EventLoop 判断
    active->reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      EventEntry entry;
      entry.fd = events[i].data.fd;
      entry.events = FromEpollEvents(events[i].events);
      if (entry.events == Poller::kNone) continue;
      active->push_back(entry);
    }
    return static_cast<int>(active->size());
  }

  std::size_t Size() const override { return registered_.size(); }

  const char* Name() const override { return "epoll"; }

 private:
  static constexpr int kMaxEventsPerWait = 256;

  static void ZeroEvent(epoll_event* ev) {
    std::memset(ev, 0, sizeof(*ev));
    ev->data.fd = -1;
  }

  int epoll_fd_{-1};
  std::unordered_set<int> registered_;
};

#else  // 非 Linux：WSAPoll（Windows）或 poll（其它 POSIX）

namespace {

#ifdef _WIN32
using PollFd = WSAPOLLFD;
#else
using PollFd = pollfd;
#endif

std::uint32_t ToPollEvents(std::uint32_t events) {
  short out = 0;
#ifdef _WIN32
  if ((events & Poller::kReadable) != 0) out |= POLLRDNORM;
  if ((events & Poller::kWritable) != 0) out |= POLLWRNORM;
#else
  if ((events & Poller::kReadable) != 0) out |= POLLIN;
  if ((events & Poller::kWritable) != 0) out |= POLLOUT;
#endif
  return static_cast<std::uint32_t>(out);
}

std::uint32_t FromPollEvents(short revents) {
  std::uint32_t out = 0;
#ifdef _WIN32
  if ((revents & POLLRDNORM) != 0) out |= Poller::kReadable;
  if ((revents & POLLWRNORM) != 0) out |= Poller::kWritable;
  if ((revents & POLLNVAL) != 0) out |= Poller::kError | Poller::kHup;
#else
  if ((revents & POLLIN) != 0) out |= Poller::kReadable;
  if ((revents & POLLOUT) != 0) out |= Poller::kWritable;
  if ((revents & POLLNVAL) != 0) out |= Poller::kError | Poller::kHup;
#endif
  if ((revents & POLLERR) != 0) out |= Poller::kError;
  if ((revents & POLLHUP) != 0) out |= Poller::kHup;
  return out;
}

void SetPollFd(PollFd* entry, int fd, std::uint32_t events) {
#ifdef _WIN32
  entry->fd = static_cast<SOCKET>(fd);
#else
  entry->fd = fd;
#endif
  entry->events = static_cast<short>(ToPollEvents(events));
  entry->revents = 0;
}

}  // namespace

// 自维护 poll 数组 + fd→index 表。
// Del 用"与末尾交换"实现 O(1)，因此必须同步修正被搬动元素的 index。
class PollArrayPoller final : public Poller {
 public:
  bool Add(int fd, std::uint32_t events) override {
    if (fd < 0) return false;
    if (index_.count(fd) != 0) return false;
    PollFd entry;
    SetPollFd(&entry, fd, events);
    index_[fd] = fds_.size();
    fds_.push_back(entry);
    return true;
  }

  bool Mod(int fd, std::uint32_t events) override {
    const auto it = index_.find(fd);
    if (it == index_.end()) return false;
    fds_[it->second].events = static_cast<short>(ToPollEvents(events));
    fds_[it->second].revents = 0;
    return true;
  }

  bool Del(int fd) override {
    const auto it = index_.find(fd);
    if (it == index_.end()) return false;
    const std::size_t pos = it->second;
    if (pos + 1 != fds_.size()) {
      PollFd moved = fds_.back();
      fds_[pos] = moved;
#ifdef _WIN32
      const int moved_fd = static_cast<int>(moved.fd);
#else
      const int moved_fd = moved.fd;
#endif
      index_[moved_fd] = pos;
    }
    fds_.pop_back();
    index_.erase(fd);
    return true;
  }

  int Wait(std::vector<EventEntry>* active, int timeout_ms) override {
    if (active == nullptr) return -1;
    active->clear();
    if (fds_.empty()) {
      // 没有注册任何 fd：见 epoll 分支的同款说明，绝不永久阻塞。
      if (timeout_ms < 0) {
#ifdef _WIN32
        ::Sleep(50);
#else
        ::poll(nullptr, 0, 50);
#endif
      } else if (timeout_ms > 0) {
#ifdef _WIN32
        ::Sleep(static_cast<DWORD>(timeout_ms));
#else
        ::poll(nullptr, 0, timeout_ms);
#endif
      }
      return 0;
    }
    // WSAPoll 不清 revents，必须自己清；poll 虽然会覆写，清零同样无害且更明确。
    for (PollFd& entry : fds_) entry.revents = 0;

#ifdef _WIN32
    const int n = ::WSAPoll(fds_.data(), static_cast<ULONG>(fds_.size()), timeout_ms);
    if (n == SOCKET_ERROR) return -1;
#else
    const int n = ::poll(fds_.data(), static_cast<nfds_t>(fds_.size()), timeout_ms);
    if (n < 0) return -1;
#endif
    if (n == 0) return 0;

    for (const PollFd& entry : fds_) {
      if (entry.revents == 0) continue;
      EventEntry ready;
#ifdef _WIN32
      ready.fd = static_cast<int>(entry.fd);
#else
      ready.fd = entry.fd;
#endif
      ready.events = FromPollEvents(entry.revents);
      if (ready.events == Poller::kNone) continue;
      active->push_back(ready);
    }
    return static_cast<int>(active->size());
  }

  std::size_t Size() const override { return fds_.size(); }

  const char* Name() const override {
#ifdef _WIN32
    return "wsa_poll";
#else
    return "poll";
#endif
  }

 private:
  std::vector<PollFd> fds_;
  std::unordered_map<int, std::size_t> index_;
};

#endif  // __linux__

std::unique_ptr<Poller> Poller::Create() {
#if defined(__linux__)
  return std::unique_ptr<Poller>(new EpollPoller());
#else
  // Windows 与其它 POSIX 共用 PollArrayPoller：内部用 WSAPoll / poll，
  // 只差在 fd 字段类型与事件常量，已由 PollFd 与宏分支隔离。
  return std::unique_ptr<Poller>(new PollArrayPoller());
#endif
}

}  // namespace net
}  // namespace qzrpc
