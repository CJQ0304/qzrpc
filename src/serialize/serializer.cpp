// 轻舟 RPC (Qingzhou RPC)
// src/serialize/serializer.cpp
//
// 职责：Message <-> string/Buffer 的薄门面。上层只认这 4+1 个函数，
//       不直接构造 Writer/Reader（便于将来替换编解码实现与集中打点）。

#include "serialize/serializer.h"

#include "common/buffer.h"
#include "serialize/message.h"
#include "serialize/reader.h"
#include "serialize/writer.h"

namespace qzrpc {
namespace serde {

bool Serializer::SerializeToString(const Message& message, std::string* out) {
  if (out == nullptr) return false;
  out->clear();
  return message.SerializeToString(out);
}

bool Serializer::SerializeToBuffer(const Message& message, Buffer* out) {
  if (out == nullptr) return false;
  Writer writer;
  if (!message.Encode(&writer)) return false;
  out->Append(writer.data().data(), writer.size());
  return true;
}

bool Serializer::ParseFromArray(const void* data, std::size_t len, Message* message) {
  if (message == nullptr) return false;
  return message->ParseFromArray(data, len);
}

bool Serializer::ParseFromString(const std::string& data, Message* message) {
  if (message == nullptr) return false;
  return message->ParseFromString(data);
}

std::size_t Serializer::ByteSize(const Message& message) { return message.ByteSize(); }

}  // namespace serde
}  // namespace qzrpc
