// 轻舟 RPC (Qingzhou RPC)
// src/serialize/message.cpp
//
// 职责：序列化消息基类的非虚封装。这里只做"临时 Writer/Reader 的生命周期管理 +
//       错误语义统一"，具体字段编解码由 codegen 生成的 Encode/Decode 完成。

#include "serialize/message.h"

#include <utility>

#include "serialize/reader.h"
#include "serialize/writer.h"

namespace qzrpc {
namespace serde {

Message::Message() = default;

Message::~Message() = default;

bool Message::SerializeToString(std::string* out) const {
  if (out == nullptr) return false;
  Writer writer;
  if (!Encode(&writer)) return false;
  // 成功才覆盖 out：失败时保证 out 不被修改（头文件契约）。
  *out = writer.ReleaseData();
  return true;
}

bool Message::ParseFromString(const std::string& data) {
  return ParseFromArray(data.data(), data.size());
}

bool Message::ParseFromArray(const void* data, std::size_t len) {
  // 先 Clear：解析失败时对象也应处于"干净的空消息"状态，而不是半个旧消息 + 半个新消息。
  Clear();
  if (data == nullptr || len == 0) return true;  // 空消息是合法的
  Reader reader(data, len);
  if (!Decode(&reader)) return false;
  if (reader.failed()) return false;
  // 消息体必须被恰好消费完：剩余字节说明线格式与 schema 不一致（多了字段却没有 tag）。
  return reader.AtEnd();
}

std::size_t Message::ByteSize() const {
  Writer writer = Writer::Counting();
  if (!Encode(&writer)) return 0;
  return writer.size();
}

void Message::AddUnknownField(UnknownField field) {
  unknown_fields_.push_back(std::move(field));
}

void Message::ClearUnknownFields() { unknown_fields_.clear(); }

}  // namespace serde
}  // namespace qzrpc
