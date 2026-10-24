// 轻舟 RPC (Qingzhou RPC)
// src/server/dispatcher.cpp
//
// 职责：method_id → MethodHandler 的**执行期路由表**（热路径）。
//       与 ServiceRegistry（冷路径元信息目录）刻意分开：这里只有一张哈希表，
//       一次查找 = 一次 shared_lock + 一次哈希，不碰字符串、不碰类型名。
//
// 并发模型：
//   * 读（Find/fallback/size/method_ids）取 shared_lock，可并发；
//   * 写（Register/SetFallback/Clear）取 unique_lock；
//   * 注册完成后应视为只读，此时开销与普通 unordered_map 相同。
//
// 指针稳定性：Find 返回 `&entry.handler`（指向 map 内部节点）。
//   unordered_map 的 rehash 不会使元素的指针/引用失效（只失效迭代器），所以
//   返回的指针在"不再 erase/Clear"的前提下始终有效——这也正是头文件
//   "生命周期同 Dispatcher" 的依据。Clear() 会让所有已返回的指针失效。

#include "server/dispatcher.h"

#include <algorithm>
#include <string>
#include <utility>

#include "common/time_util.h"
#include "protocol/frame.h"

namespace qzrpc {
namespace rpc {

namespace {

// "demo.EchoService.Echo" + 8 位十六进制 id，便于在冲突报告里一眼定位。
std::string IdToHex(std::uint32_t method_id) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned>(method_id));
  return std::string(buffer);
}

void SetError(std::string* err, std::string text) {
  if (err != nullptr) *err = std::move(text);
}

}  // namespace

// ---------------------------------------------------------------------------
// ServerContext
// ---------------------------------------------------------------------------

bool ServerContext::deadline_exceeded(std::uint64_t now_ms) const noexcept {
  // 时钟异常（now < start）时不误判：宁可多干一次活，也不要无端回超时。
  if (now_ms < start_ms) return false;
  const std::uint64_t elapsed_ms = now_ms - start_ms;
  if (metadata == nullptr) return false;

  // 优先级：绝对 deadline 优先于相对 timeout（doc 03 §6.3）。
  // 绝对时间只用于"粗粒度判断是否已经没意义"，因此这里用它算剩余额度，
  // 再与**单调时钟**测得的已耗时比较——不把绝对时间当作精确定时依据。
  const std::uint64_t absolute_deadline = metadata->deadline_unix_ms();
  if (absolute_deadline != 0) {
    const std::uint64_t now_unix = TimeUtil::UnixMs();
    if (now_unix >= absolute_deadline) return true;
    const std::uint64_t remaining_ms = absolute_deadline - now_unix;
    return elapsed_ms > remaining_ms;
  }
  if (metadata->has_timeout() && metadata->timeout_ms() != 0) {
    return elapsed_ms > metadata->timeout_ms();
  }
  return false;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

Dispatcher::Dispatcher() = default;

Dispatcher::~Dispatcher() = default;

bool Dispatcher::Register(std::uint32_t method_id, std::string full_name, MethodHandler handler,
                          std::string* err) {
  if (!handler) {
    SetError(err, "dispatcher: null handler for '" + full_name + "'");
    return false;
  }
  // method_id 0 在全框架里是"未指定"的哨兵（帧头里 seq 用 0 表示不关心响应，
  // method_id 也保留 0），不允许注册，否则路由表里会出现一个能被误命中的洞。
  if (method_id == 0) {
    SetError(err, "dispatcher: method_id 0 is reserved, cannot register '" + full_name + "'");
    return false;
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto it = table_.find(method_id);
  if (it != table_.end()) {
    if (it->second.full_name == full_name) {
      // 同一个方法重复注册（多份生成代码重复调用 RegisterXxxService）：
      // 幂等成功，不覆盖已有 handler（避免后注册者悄悄顶掉先注册者）。
      return true;
    }
    SetError(err, "dispatcher: method_id conflict " + IdToHex(method_id) + ": existing '" +
                      it->second.full_name + "' vs new '" + full_name + "'");
    return false;
  }

  Entry entry;
  entry.full_name = std::move(full_name);
  entry.handler = std::move(handler);
  table_.emplace(method_id, std::move(entry));
  return true;
}

const MethodHandler* Dispatcher::Find(std::uint32_t method_id, const char** full_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = table_.find(method_id);
  if (it == table_.end()) {
    // 未命中：*full_name 保持不变（头文件契约），调用方据此判断"没有覆盖旧值"。
    return nullptr;
  }
  if (full_name != nullptr) *full_name = it->second.full_name.c_str();
  return &it->second.handler;
}

void Dispatcher::SetFallback(MethodHandler handler) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  fallback_ = std::move(handler);
}

const MethodHandler* Dispatcher::fallback() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return fallback_ ? &fallback_ : nullptr;
}

std::size_t Dispatcher::size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return table_.size();
}

std::vector<std::uint32_t> Dispatcher::method_ids() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<std::uint32_t> ids;
  ids.reserve(table_.size());
  for (const auto& item : table_) ids.push_back(item.first);
  lock.unlock();  // 排序不需要锁
  std::sort(ids.begin(), ids.end());
  return ids;
}

void Dispatcher::Clear() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  table_.clear();
  fallback_ = nullptr;
}

}  // namespace rpc
}  // namespace qzrpc
