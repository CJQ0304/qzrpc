// 轻舟 RPC (Qingzhou RPC)
// src/serialize/serializer.h
//
// 职责：把 serde::Message 与 string / Buffer 互转的门面。
//       上层（protocol / rpc）只调本类的 4 个函数，不直接 new Writer/Reader。
//
// 为什么单独做一层门面：
//   * 便于将来替换编解码实现（只改本类的 .cpp）；
//   * 便于统一错误语义（一律 bool + 上层翻译成 Status::kSerializeError）；
//   * 便于统计序列化耗时与字节数（metrics 打点集中在一处）。
//
// 线程安全：全部为无状态静态函数，线程安全。
#pragma once

#include <cstddef>
#include <string>

#include "common/noncopyable.h"

namespace qzrpc {

class Buffer;

namespace serde {

class Message;

// 序列化门面（纯静态）。
class Serializer : private StaticOnly {
 public:
  // 序列化为字符串。先清空 out，失败时 out 为空。
  // 返回：true 成功。
  static bool SerializeToString(const Message& message, std::string* out);

  // 序列化并**追加**到 Buffer（零额外拷贝写入网络输出缓冲）。
  // 返回：true 成功。
  static bool SerializeToBuffer(const Message& message, Buffer* out);

  // 从内存解析。先 Clear(message) 再 Decode。
  // 返回：true 成功；false 表示数据非法（调用方应回 kSerializeError）。
  static bool ParseFromArray(const void* data, std::size_t len, Message* message);

  // 从字符串解析。
  static bool ParseFromString(const std::string& data, Message* message);

  // 编码后字节数（不实际分配输出）。
  static std::size_t ByteSize(const Message& message);
};

}  // namespace serde
}  // namespace qzrpc
