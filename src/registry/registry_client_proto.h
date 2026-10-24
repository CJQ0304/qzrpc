// 轻舟 RPC (Qingzhou RPC)
// src/registry/registry_client_proto.h
//
// 职责：注册中心的协议消息定义。
//
// 这份文件是**手写的**，但它严格遵循 codegen 的输出形态（见 doc 04 §4.4），
// 因此它同时承担两个作用：
//   1) 提供注册中心实际使用的消息类型；
//   2) 作为「生成代码长什么样」的可运行参照物 —— 任何人想确认 codegen 该产出什么，
//      读这个文件即可。
//
// 与业务 IDL 的关系：
//   idl/registry.qz 的语义与这里完全一致；如果 codegen 正常工作，
//   `qzcodegen registry.qz` 的产物应该与本文件除注释外的内容几乎逐行对应。
//   这条"自举一致性"是本项目质量控制的关键手段。
//
// 线格式：与 protobuf 完全兼容，字段号见各字段注释。
// 线程安全：普通值对象，非线程安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "protocol/protocol_const.h"
#include "serialize/message.h"

namespace qzrpc {
namespace registry {

// 服务名全名（注册中心自身的 RPC 服务）。
inline constexpr const char* kRegistryServiceFullName = "qzrpc.registry.RegistryService";

// 注册中心的方法 id（由 full_name 经 FNV-1a 64 取低 32 位得到）。
// 说明：这里用函数而不是常量，避免静态初始化顺序问题；开销可忽略（每 3 秒调一次）。
inline std::uint32_t MethodIdRegister() noexcept {
  return protocol::MethodIdOfName("qzrpc.registry.RegistryService.Register");
}
inline std::uint32_t MethodIdHeartbeat() noexcept {
  return protocol::MethodIdOfName("qzrpc.registry.RegistryService.Heartbeat");
}
inline std::uint32_t MethodIdFetch() noexcept {
  return protocol::MethodIdOfName("qzrpc.registry.RegistryService.Fetch");
}
inline std::uint32_t MethodIdDeregister() noexcept {
  return protocol::MethodIdOfName("qzrpc.registry.RegistryService.Deregister");
}

// 一个服务实例。
// .qz 定义：
//   message ServiceInstance {
//     string service_name = 1;
//     string host         = 2;
//     uint32 port         = 3;
//     uint32 weight       = 4;
//     string instance_id  = 5;
//     uint32 ttl_ms       = 6;
//   }
class ServiceInstance final : public serde::Message {
 public:
  ServiceInstance() = default;
  ~ServiceInstance() override = default;

  // "qzrpc.registry.ServiceInstance"
  const char* TypeName() const override { return "qzrpc.registry.ServiceInstance"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  // 字段访问。
  const std::string& service_name() const noexcept { return service_name_; }
  void set_service_name(std::string v) { service_name_ = std::move(v); }

  const std::string& host() const noexcept { return host_; }
  void set_host(std::string v) { host_ = std::move(v); }

  std::uint32_t port() const noexcept { return port_; }
  void set_port(std::uint32_t v) noexcept { port_ = v; }

  std::uint32_t weight() const noexcept { return weight_; }
  void set_weight(std::uint32_t v) noexcept { weight_ = v; }

  const std::string& instance_id() const noexcept { return instance_id_; }
  void set_instance_id(std::string v) { instance_id_ = std::move(v); }

  std::uint32_t ttl_ms() const noexcept { return ttl_ms_; }
  void set_ttl_ms(std::uint32_t v) noexcept { ttl_ms_ = v; }

 private:
  std::string service_name_;
  std::string host_;
  std::uint32_t port_{0};
  std::uint32_t weight_{1};
  std::string instance_id_;
  std::uint32_t ttl_ms_{0};
};

// 注册请求。
class RegisterRequest final : public serde::Message {
 public:
  RegisterRequest() = default;
  ~RegisterRequest() override = default;

  const char* TypeName() const override { return "qzrpc.registry.RegisterRequest"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  const ServiceInstance& instance() const noexcept { return instance_; }
  ServiceInstance* mutable_instance() noexcept { return &instance_; }
  void set_instance(const ServiceInstance& v) { instance_ = v; }

  // 租约时长（毫秒），超时未心跳则被摘除。
  std::uint32_t lease_ttl_ms() const noexcept { return lease_ttl_ms_; }
  void set_lease_ttl_ms(std::uint32_t v) noexcept { lease_ttl_ms_ = v; }

 private:
  ServiceInstance instance_;
  std::uint32_t lease_ttl_ms_{15000};
};

// 注册响应。
class RegisterResponse final : public serde::Message {
 public:
  RegisterResponse() = default;
  ~RegisterResponse() override = default;

  const char* TypeName() const override { return "qzrpc.registry.RegisterResponse"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  bool ok() const noexcept { return ok_; }
  void set_ok(bool v) noexcept { ok_ = v; }

  const std::string& error() const noexcept { return error_; }
  void set_error(std::string v) { error_ = std::move(v); }

  // 服务端分配/确认的实例 id（客户端请求里没带 instance_id 时由服务端生成）。
  const std::string& assigned_instance_id() const noexcept { return assigned_instance_id_; }
  void set_assigned_instance_id(std::string v) { assigned_instance_id_ = std::move(v); }

 private:
  bool ok_{false};
  std::string error_;
  std::string assigned_instance_id_;
};

// 心跳请求。
class HeartbeatRequest final : public serde::Message {
 public:
  HeartbeatRequest() = default;
  ~HeartbeatRequest() override = default;

  const char* TypeName() const override { return "qzrpc.registry.HeartbeatRequest"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  const std::string& service_name() const noexcept { return service_name_; }
  void set_service_name(std::string v) { service_name_ = std::move(v); }

  const std::string& instance_id() const noexcept { return instance_id_; }
  void set_instance_id(std::string v) { instance_id_ = std::move(v); }

 private:
  std::string service_name_;
  std::string instance_id_;
};

// 心跳响应。
class HeartbeatResponse final : public serde::Message {
 public:
  HeartbeatResponse() = default;
  ~HeartbeatResponse() override = default;

  const char* TypeName() const override { return "qzrpc.registry.HeartbeatResponse"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  bool ok() const noexcept { return ok_; }
  void set_ok(bool v) noexcept { ok_ = v; }

  // 服务端不认识该实例（已被 TTL 摘除），客户端应重新 Register。
  bool not_found() const noexcept { return not_found_; }
  void set_not_found(bool v) noexcept { not_found_ = v; }

 private:
  bool ok_{false};
  bool not_found_{false};
};

// 拉取实例列表请求。
class FetchRequest final : public serde::Message {
 public:
  FetchRequest() = default;
  ~FetchRequest() override = default;

  const char* TypeName() const override { return "qzrpc.registry.FetchRequest"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  const std::string& service_name() const noexcept { return service_name_; }
  void set_service_name(std::string v) { service_name_ = std::move(v); }

 private:
  std::string service_name_;
};

// 拉取实例列表响应。
class FetchResponse final : public serde::Message {
 public:
  FetchResponse() = default;
  ~FetchResponse() override = default;

  const char* TypeName() const override { return "qzrpc.registry.FetchResponse"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  std::size_t instances_size() const noexcept { return instances_.size(); }
  const ServiceInstance& instances(std::size_t i) const { return instances_[i]; }
  ServiceInstance* add_instances();
  const std::vector<ServiceInstance>& instances() const noexcept { return instances_; }

  // 版本号（单调递增）。客户端可据此跳过未变化的刷新。
  std::uint64_t version() const noexcept { return version_; }
  void set_version(std::uint64_t v) noexcept { version_ = v; }

 private:
  std::vector<ServiceInstance> instances_;
  std::uint64_t version_{0};
};

// 注销请求。
class DeregisterRequest final : public serde::Message {
 public:
  DeregisterRequest() = default;
  ~DeregisterRequest() override = default;

  const char* TypeName() const override { return "qzrpc.registry.DeregisterRequest"; }
  void Clear() override;
  bool Encode(serde::Writer* writer) const override;
  bool Decode(serde::Reader* reader) override;
  const serde::FieldMeta* Fields(std::size_t* count) const override;

  const std::string& service_name() const noexcept { return service_name_; }
  void set_service_name(std::string v) { service_name_ = std::move(v); }

  const std::string& instance_id() const noexcept { return instance_id_; }
  void set_instance_id(std::string v) { instance_id_ = std::move(v); }

 private:
  std::string service_name_;
  std::string instance_id_;
};

}  // namespace registry
}  // namespace qzrpc
