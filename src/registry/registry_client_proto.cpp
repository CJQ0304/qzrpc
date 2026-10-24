// 轻舟 RPC (Qingzhou RPC)
// src/registry/registry_client_proto.cpp
//
// 职责：手写实现 registry_client_proto.h 里的 8 个消息的编解码。
//
// 这份实现刻意与 codegen 的输出保持**同形**：先 Encode（字段号升序、默认值不编码、
// 未知字段原样追加），再 Decode（packed/unpacked 都接受、未知字段捕获），
// 最后 Fields 返回静态元数据表。任何人想确认 codegen 该产出什么形状，
// 把本文件与 generated/echo.qz.cpp 对读即可。
//
// ★ 两个容易写错的地方：
//   1) uint32/uint64 一律走 varint（WriteVarint32/64），**不是** fixed32/64。
//      port/weight/ttl_ms/version 用 fixed 编码会让对端按 wire_type=5/1 去读并报错。
//   2) bool 在 protobuf 里是 varint 0/1。false 是默认值 → **不编码**；
//      只有 true 才写。若把 false 也写出来，虽然语义不变，但会让"同一份消息
//      编出的字节"在两个实现之间不一致，破坏黄金样本对拍。

#include "registry/registry_client_proto.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "serialize/reader.h"
#include "serialize/writer.h"

namespace qzrpc {
namespace registry {

namespace {

// 把 wire_type 枚举转成 uint8，少写几次 static_cast。
constexpr std::uint8_t W(serde::WireType type) { return static_cast<std::uint8_t>(type); }

}  // namespace

// ---------------------------------------------------------------------------
// ServiceInstance
// ---------------------------------------------------------------------------

void ServiceInstance::Clear() {
  service_name_.clear();
  host_.clear();
  port_ = 0;
  weight_ = 1;
  instance_id_.clear();
  ttl_ms_ = 0;
  ClearUnknownFields();
}

bool ServiceInstance::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;

  // field 1: string service_name
  if (!service_name_.empty()) {
    writer->WriteTag(1, W(serde::WireType::kLengthDelimited));
    writer->WriteString(service_name_);
  }
  // field 2: string host
  if (!host_.empty()) {
    writer->WriteTag(2, W(serde::WireType::kLengthDelimited));
    writer->WriteString(host_);
  }
  // field 3: uint32 port
  if (port_ != 0) {
    writer->WriteTag(3, W(serde::WireType::kVarint));
    writer->WriteVarint32(port_);
  }
  // field 4: uint32 weight（默认值 1）
  if (weight_ != 1) {
    writer->WriteTag(4, W(serde::WireType::kVarint));
    writer->WriteVarint32(weight_);
  }
  // field 5: string instance_id
  if (!instance_id_.empty()) {
    writer->WriteTag(5, W(serde::WireType::kLengthDelimited));
    writer->WriteString(instance_id_);
  }
  // field 6: uint32 ttl_ms
  if (ttl_ms_ != 0) {
    writer->WriteTag(6, W(serde::WireType::kVarint));
    writer->WriteVarint32(ttl_ms_);
  }

  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool ServiceInstance::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadStringField(wire_type, &service_name_)) return false;
        break;
      case 2:
        if (!reader->ReadStringField(wire_type, &host_)) return false;
        break;
      case 3:
        if (!reader->ReadVarint32Field(wire_type, &port_)) return false;
        break;
      case 4:
        if (!reader->ReadVarint32Field(wire_type, &weight_)) return false;
        break;
      case 5:
        if (!reader->ReadStringField(wire_type, &instance_id_)) return false;
        break;
      case 6:
        if (!reader->ReadVarint32Field(wire_type, &ttl_ms_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* ServiceInstance::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "service_name", serde::FieldType::kString, nullptr, false, false, false, nullptr},
      {2, "host", serde::FieldType::kString, nullptr, false, false, false, nullptr},
      {3, "port", serde::FieldType::kUInt32, nullptr, false, false, false, nullptr},
      {4, "weight", serde::FieldType::kUInt32, nullptr, false, false, true, "1"},
      {5, "instance_id", serde::FieldType::kString, nullptr, false, false, false, nullptr},
      {6, "ttl_ms", serde::FieldType::kUInt32, nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ---------------------------------------------------------------------------
// RegisterRequest
// ---------------------------------------------------------------------------

void RegisterRequest::Clear() {
  instance_.Clear();
  lease_ttl_ms_ = 15000;
  ClearUnknownFields();
}

bool RegisterRequest::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;

  // field 1: qzrpc.registry.ServiceInstance instance（message 字段永远有 presence）
  {
    serde::Writer nested;
    if (!instance_.Encode(&nested)) return false;
    writer->WriteTag(1, W(serde::WireType::kLengthDelimited));
    writer->WriteLengthDelimited(nested.data().data(), nested.size());
  }
  // field 2: uint32 lease_ttl_ms（默认值 15000）
  if (lease_ttl_ms_ != 15000) {
    writer->WriteTag(2, W(serde::WireType::kVarint));
    writer->WriteVarint32(lease_ttl_ms_);
  }

  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool RegisterRequest::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1: {
        const char* data = nullptr;
        std::size_t len = 0;
        if (!reader->ReadLengthDelimitedField(wire_type, &data, &len)) return false;
        serde::Reader nested(data, len);
        if (!instance_.Decode(&nested)) return false;
        break;
      }
      case 2:
        if (!reader->ReadVarint32Field(wire_type, &lease_ttl_ms_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* RegisterRequest::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "instance", serde::FieldType::kMessage, "qzrpc.registry.ServiceInstance", false, false,
       false, nullptr},
      {2, "lease_ttl_ms", serde::FieldType::kUInt32, nullptr, false, false, true, "15000"},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ---------------------------------------------------------------------------
// RegisterResponse
// ---------------------------------------------------------------------------

void RegisterResponse::Clear() {
  ok_ = false;
  error_.clear();
  assigned_instance_id_.clear();
  ClearUnknownFields();
}

bool RegisterResponse::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;

  // field 1: bool ok（false 是默认值，不编码）
  if (ok_) {
    writer->WriteTag(1, W(serde::WireType::kVarint));
    writer->WriteBool(ok_);
  }
  // field 2: string error
  if (!error_.empty()) {
    writer->WriteTag(2, W(serde::WireType::kLengthDelimited));
    writer->WriteString(error_);
  }
  // field 3: string assigned_instance_id
  if (!assigned_instance_id_.empty()) {
    writer->WriteTag(3, W(serde::WireType::kLengthDelimited));
    writer->WriteString(assigned_instance_id_);
  }

  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool RegisterResponse::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadBoolField(wire_type, &ok_)) return false;
        break;
      case 2:
        if (!reader->ReadStringField(wire_type, &error_)) return false;
        break;
      case 3:
        if (!reader->ReadStringField(wire_type, &assigned_instance_id_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* RegisterResponse::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "ok", serde::FieldType::kBool, nullptr, false, false, false, nullptr},
      {2, "error", serde::FieldType::kString, nullptr, false, false, false, nullptr},
      {3, "assigned_instance_id", serde::FieldType::kString, nullptr, false, false, false,
       nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ---------------------------------------------------------------------------
// HeartbeatRequest / HeartbeatResponse
// ---------------------------------------------------------------------------

void HeartbeatRequest::Clear() {
  service_name_.clear();
  instance_id_.clear();
  ClearUnknownFields();
}

bool HeartbeatRequest::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;
  if (!service_name_.empty()) {
    writer->WriteTag(1, W(serde::WireType::kLengthDelimited));
    writer->WriteString(service_name_);
  }
  if (!instance_id_.empty()) {
    writer->WriteTag(2, W(serde::WireType::kLengthDelimited));
    writer->WriteString(instance_id_);
  }
  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool HeartbeatRequest::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadStringField(wire_type, &service_name_)) return false;
        break;
      case 2:
        if (!reader->ReadStringField(wire_type, &instance_id_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* HeartbeatRequest::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "service_name", serde::FieldType::kString, nullptr, false, false, false, nullptr},
      {2, "instance_id", serde::FieldType::kString, nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

void HeartbeatResponse::Clear() {
  ok_ = false;
  not_found_ = false;
  ClearUnknownFields();
}

bool HeartbeatResponse::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;
  if (ok_) {
    writer->WriteTag(1, W(serde::WireType::kVarint));
    writer->WriteBool(ok_);
  }
  if (not_found_) {
    writer->WriteTag(2, W(serde::WireType::kVarint));
    writer->WriteBool(not_found_);
  }
  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool HeartbeatResponse::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadBoolField(wire_type, &ok_)) return false;
        break;
      case 2:
        if (!reader->ReadBoolField(wire_type, &not_found_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* HeartbeatResponse::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "ok", serde::FieldType::kBool, nullptr, false, false, false, nullptr},
      {2, "not_found", serde::FieldType::kBool, nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ---------------------------------------------------------------------------
// FetchRequest / FetchResponse
// ---------------------------------------------------------------------------

void FetchRequest::Clear() {
  service_name_.clear();
  ClearUnknownFields();
}

bool FetchRequest::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;
  if (!service_name_.empty()) {
    writer->WriteTag(1, W(serde::WireType::kLengthDelimited));
    writer->WriteString(service_name_);
  }
  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool FetchRequest::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadStringField(wire_type, &service_name_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* FetchRequest::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "service_name", serde::FieldType::kString, nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

void FetchResponse::Clear() {
  instances_.clear();
  version_ = 0;
  ClearUnknownFields();
}

ServiceInstance* FetchResponse::add_instances() {
  instances_.emplace_back();
  return &instances_.back();
}

bool FetchResponse::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;

  // field 1: repeated ServiceInstance instances（message 元素逐条带 tag，不 packed）
  for (const ServiceInstance& item : instances_) {
    serde::Writer nested;
    if (!item.Encode(&nested)) return false;
    writer->WriteTag(1, W(serde::WireType::kLengthDelimited));
    writer->WriteLengthDelimited(nested.data().data(), nested.size());
  }
  // field 2: uint64 version
  if (version_ != 0) {
    writer->WriteTag(2, W(serde::WireType::kVarint));
    writer->WriteVarint64(version_);
  }

  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool FetchResponse::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1: {
        const char* data = nullptr;
        std::size_t len = 0;
        if (!reader->ReadLengthDelimitedField(wire_type, &data, &len)) return false;
        serde::Reader nested(data, len);
        ServiceInstance item;
        if (!item.Decode(&nested)) return false;
        instances_.push_back(std::move(item));
        break;
      }
      case 2:
        if (!reader->ReadVarint64Field(wire_type, &version_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* FetchResponse::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "instances", serde::FieldType::kMessage, "qzrpc.registry.ServiceInstance", true, false,
       false, nullptr},
      {2, "version", serde::FieldType::kUInt64, nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ---------------------------------------------------------------------------
// DeregisterRequest
// ---------------------------------------------------------------------------

void DeregisterRequest::Clear() {
  service_name_.clear();
  instance_id_.clear();
  ClearUnknownFields();
}

bool DeregisterRequest::Encode(serde::Writer* writer) const {
  if (writer == nullptr) return false;
  if (!service_name_.empty()) {
    writer->WriteTag(1, W(serde::WireType::kLengthDelimited));
    writer->WriteString(service_name_);
  }
  if (!instance_id_.empty()) {
    writer->WriteTag(2, W(serde::WireType::kLengthDelimited));
    writer->WriteString(instance_id_);
  }
  for (const serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool DeregisterRequest::Decode(serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadStringField(wire_type, &service_name_)) return false;
        break;
      case 2:
        if (!reader->ReadStringField(wire_type, &instance_id_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const serde::FieldMeta* DeregisterRequest::Fields(std::size_t* count) const {
  static const serde::FieldMeta kFields[] = {
      {1, "service_name", serde::FieldType::kString, nullptr, false, false, false, nullptr},
      {2, "instance_id", serde::FieldType::kString, nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

}  // namespace registry
}  // namespace qzrpc
