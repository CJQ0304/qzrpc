// 轻舟 RPC (Qingzhou RPC)
// src/server/service_registry.cpp
//
// 职责：进程级服务/方法元信息目录（冷路径：启动期自检、日志翻译、metrics label）。
//       **不参与热路径路由** —— 那是 Dispatcher 的事。
//
// 三层冲突检测里的 L2（doc 03 §7.4）：
//   注册时重算 MethodIdOfName(full_name) 与传入 id 比对，再查 id 是否已被**不同**全名占用。
//
// 两条判定规则（与 service_registry.h 里 method_id_overridden 的注释逐字对齐）：
//   * method_id_overridden == false（哈希派生，默认）→ 重算结果必须相等，不等即判 codegen
//     bug，记 ERROR 并拒绝注册（这是 L2 最有价值的一条防线：生成常量与名字不一致必然是
//     生成器错了，放任它上线就是"所有调用 404"）；
//   * method_id_overridden == true（IDL 用 `[method_id = 0x...]` 显式指定）→ 跳过重算比对，
//     否则 doc 03 §7.4 承诺的"运维逃生口"永远用不了；但它**仍参与冲突检测**
//     （同 id 不同全名依旧拒绝）—— 逃逸不等于放弃唯一性。

#include "server/service_registry.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "protocol/protocol_const.h"

namespace qzrpc {
namespace rpc {

namespace {

std::string IdToHex(std::uint32_t method_id) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned>(method_id));
  return std::string(buffer);
}

// "demo.EchoService.Echo" → "demo.EchoService"（只用于 service_name 缺省时的兜底）。
std::string ServicePartOf(const std::string& full_name) {
  const std::size_t dot = full_name.rfind('.');
  return dot == std::string::npos ? std::string() : full_name.substr(0, dot);
}

}  // namespace

ServiceRegistry& ServiceRegistry::Instance() {
  // C++11 起函数内 static 的初始化是线程安全的（魔法静态）。
  static ServiceRegistry instance;
  return instance;
}

bool ServiceRegistry::RegisterMethod(const MethodInfo& info, std::string* err) {
  // ---- 入参校验 ----
  if (info.full_name.empty()) {
    if (err != nullptr) *err = "service registry: empty full_name";
    return false;
  }
  if (info.method_id == 0) {
    if (err != nullptr) {
      *err = "service registry: method_id 0 is reserved (full_name='" + info.full_name + "')";
    }
    return false;
  }

  const std::uint32_t computed = protocol::MethodIdOfName(info.full_name);

  std::unique_lock<std::shared_mutex> lock(mutex_);

  // ---- 1) 按全名查：同一方法重复登记（多份生成代码都调 RegisterXxxService） ----
  const auto by_name = by_name_.find(info.full_name);
  if (by_name != by_name_.end()) {
    const std::uint32_t existing_id = by_name->second;
    if (existing_id != info.method_id) {
      if (err != nullptr) {
        *err = "service registry: '" + info.full_name + "' registered with two ids: " +
               IdToHex(existing_id) + " vs " + IdToHex(info.method_id);
      }
      return false;
    }
    // 幂等：用最新的元信息覆盖（请求/响应类型名、幂等标记可能在 codegen 后变化）。
    const auto slot = by_id_.find(existing_id);
    if (slot != by_id_.end()) {
      slot->second = info;
    } else {
      // 索引内部不一致（不该发生）：按新条目重建，保证两个索引都有数据。
      by_id_.emplace(existing_id, info);
    }
    return true;
  }

  // ---- 2) 按 id 查：是否已被**另一个**全名占用（哈希冲突） ----
  const auto by_id = by_id_.find(info.method_id);
  if (by_id != by_id_.end()) {
    if (err != nullptr) {
      *err = "service registry: method_id collision " + IdToHex(info.method_id) + ": '" +
             by_id->second.full_name + "' vs '" + info.full_name +
             "' (use explicit method_id or rename)";
    }
    return false;
  }

  // ---- 3) L2 重算比对：检测 codegen bug（生成常量与名字不匹配） ----
  if (computed != info.method_id) {
    if (!info.method_id_overridden) {
      // 哈希派生却对不上 → 必是 codegen bug，必须拒绝启动（否则线上全员 404）。
      QZ_LOG_ERROR << "service registry: method_id mismatch for '" << info.full_name
                   << "': codegen says " << IdToHex(info.method_id) << ", MethodIdOfName says "
                   << IdToHex(computed) << " (codegen bug: regenerate, or mark it as an "
                   "explicit method_id override)";
      if (err != nullptr) {
        *err = "service registry: method_id mismatch for '" + info.full_name + "': " +
               IdToHex(info.method_id) + " != MethodIdOfName() = " + IdToHex(computed);
      }
      return false;
    }
    // 显式指定：合法，只记一条 INFO 便于读日志时理解"为什么 id 和名字对不上"。
    QZ_LOG_INFO << "service registry: '" << info.full_name << "' uses explicit method_id "
                << IdToHex(info.method_id) << " (hash would be " << IdToHex(computed) << ")";
  }

  // ---- 4) 登记 ----
  by_id_.emplace(info.method_id, info);
  by_name_.emplace(info.full_name, info.method_id);
  return true;
}

const MethodInfo* ServiceRegistry::FindMethod(std::uint32_t method_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = by_id_.find(method_id);
  return it == by_id_.end() ? nullptr : &it->second;
}

const MethodInfo* ServiceRegistry::FindByName(const std::string& full_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = by_name_.find(full_name);
  if (it == by_name_.end()) return nullptr;
  const auto slot = by_id_.find(it->second);
  return slot == by_id_.end() ? nullptr : &slot->second;
}

std::string ServiceRegistry::DescribeMethod(std::uint32_t method_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = by_id_.find(method_id);
  if (it != by_id_.end()) return it->second.full_name;
  // 未登记时的稳定格式（运维脚本与日志解析依赖它，不要改）。
  return "unknown#" + IdToHex(method_id);
}

std::vector<MethodInfo> ServiceRegistry::AllMethods() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<MethodInfo> methods;
  methods.reserve(by_id_.size());
  for (const auto& item : by_id_) methods.push_back(item.second);
  lock.unlock();
  std::sort(methods.begin(), methods.end(),
            [](const MethodInfo& a, const MethodInfo& b) { return a.method_id < b.method_id; });
  return methods;
}

std::vector<std::string> ServiceRegistry::ServiceNames() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::set<std::string> names;
  for (const auto& item : by_id_) {
    if (!item.second.service_name.empty()) {
      names.insert(item.second.service_name);
      continue;
    }
    // service_name 没填时从 full_name 推导，避免调用方拿到空名字。
    std::string derived = ServicePartOf(item.second.full_name);
    if (!derived.empty()) names.insert(std::move(derived));
  }
  lock.unlock();
  return std::vector<std::string>(names.begin(), names.end());
}

std::size_t ServiceRegistry::size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return by_id_.size();
}

std::vector<std::string> ServiceRegistry::SelfCheck() const {
  std::vector<std::string> problems;
  std::shared_lock<std::shared_mutex> lock(mutex_);

  for (const auto& item : by_id_) {
    const MethodInfo& info = item.second;
    // (a) 重算比对：显式覆盖是**合法**的，不算问题（否则每用一次逃生口就多一条噪音，
    //     真问题反而被淹）；哈希派生却对不上才是 codegen bug。
    const std::uint32_t computed = protocol::MethodIdOfName(info.full_name);
    if (computed != info.method_id && !info.method_id_overridden) {
      problems.push_back("method_id mismatch: '" + info.full_name + "' registered as " +
                         IdToHex(info.method_id) + " but MethodIdOfName() = " +
                         IdToHex(computed) + " (codegen bug)");
    }
    // (b) 反向索引必须指回自己。
    const auto by_name = by_name_.find(info.full_name);
    if (by_name == by_name_.end()) {
      problems.push_back("missing name index for '" + info.full_name + "'");
    } else if (by_name->second != info.method_id) {
      problems.push_back("name index for '" + info.full_name + "' points to " +
                         IdToHex(by_name->second) + " but registered as " +
                         IdToHex(info.method_id));
    }
    // (c) 必填元信息缺失（不影响运行，但会让日志/metrics 变成一锅粥）。
    if (info.service_name.empty()) {
      problems.push_back("empty service_name for '" + info.full_name + "'");
    }
    if (info.request_type.empty() || info.response_type.empty()) {
      problems.push_back("empty request_type/response_type for '" + info.full_name + "'");
    }
  }

  // (d) 两个索引的元素数必须一致（不一致说明登记过程被打断）。
  if (by_id_.size() != by_name_.size()) {
    problems.push_back("index size mismatch: by_id=" + std::to_string(by_id_.size()) +
                       " by_name=" + std::to_string(by_name_.size()));
  }

  std::sort(problems.begin(), problems.end());
  return problems;
}

void ServiceRegistry::Clear() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  by_id_.clear();
  by_name_.clear();
}

}  // namespace rpc
}  // namespace qzrpc
