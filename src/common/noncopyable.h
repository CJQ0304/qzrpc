// 轻舟 RPC (Qingzhou RPC)
// src/common/noncopyable.h
//
// 职责：为框架内所有"资源持有型"对象提供统一的禁用拷贝基类。
// 约定：继承本类的类必须显式声明拷构造/赋值 = delete 之外的语义（本类已全部 delete）。
#pragma once

namespace qzrpc {

// 禁止拷贝构造与拷贝赋值的标记基类。
// 用法：class Foo : private qzrpc::NonCopyable { ... };
// 注意：需要移动语义的类型请显式声明 = default 的移动构造/赋值，
//       本类只 delete 拷贝，不影响派生类自己定义移动操作。
class NonCopyable {
 public:
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;

 protected:
  NonCopyable() = default;
  ~NonCopyable() = default;
};

// 禁止一切构造（纯静态工具类的标记基类）。
// 用法：class Foo : private qzrpc::StaticOnly { public: static void Bar(); };
class StaticOnly {
 public:
  StaticOnly() = delete;
  StaticOnly(const StaticOnly&) = delete;
  StaticOnly& operator=(const StaticOnly&) = delete;
};

}  // namespace qzrpc
