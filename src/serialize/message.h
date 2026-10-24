// 轻舟 RPC (Qingzhou RPC)
// src/serialize/message.h
//
// 职责：所有 IDL 生成的消息类的公共基类。**它是整个序列化层的唯一契约**：
//       上层（protocol / rpc）只认 serde::Message，永远不认识具体生成类型。
//
// 与 phxrpc 的关键差异：
//   phxrpc 的 BaseMessage::ToPb/FromPb 直接把 google::protobuf::Message 钉进抽象基类，
//   导致换序列化格式必须改 msg 层。本类没有任何第三方类型，只有 Writer/Reader。
//
// 生成类的实现要求（codegen 必须遵守，见 doc 04）：
//   * `Encode` 按字段号**升序**输出（protobuf 规范建议，便于 diff 与调试）；
//   * 值为默认值的单数字段**不输出**（proto3 语义），除非字段是 optional 且显式 set 过；
//   * `Decode` 忽略未知字段但保留到 unknown_fields_（保证"读改写"不丢字段）；
//   * `Fields()` 返回静态常量数组，`*count` 写入元素个数。
//
// 线程安全：Message 是**非线程安全**的值对象，与普通 struct 无异。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "serialize/field_meta.h"

namespace qzrpc {
namespace serde {

class Reader;
class Writer;

// 一个未被识别（schema 未知）的字段，原样保留以便无损转发。
struct UnknownField {
  // 字段编号。
  std::uint32_t number = 0;
  // 线类型。
  std::uint8_t wire_type = 0;
  // 字段的原始编码字节，**不含 tag**（tag 可由 number+wire_type 重建）。
  std::string raw;
};

// 序列化消息基类。
class Message {
 public:
  virtual ~Message();

  // 类型全名（"demo.EchoRequest"）。用于日志与 schema 校验。
  virtual const char* TypeName() const = 0;

  // 清空所有字段到默认值，并清空 unknown fields。
  virtual void Clear() = 0;

  // 编码到 writer。返回 false 表示编码失败（例如字符串超过 2GB 长度上限）。
  // 约束：writer 非空。
  virtual bool Encode(Writer* writer) const = 0;

  // 从 reader 解析。reader 的视图范围应恰好是一个消息体（不多不少）。
  // 返回：true 成功；false 表示数据损坏或类型不匹配。
  virtual bool Decode(Reader* reader) = 0;

  // 字段元数据表。
  // 参数：count - 输出元素个数（非空）。
  // 返回：静态常量数组指针，元素个数为 *count；无字段时返回 nullptr 且 *count = 0。
  virtual const FieldMeta* Fields(std::size_t* count) const = 0;

  // ---- 便捷封装（非虚，基于上面 5 个虚函数实现） ----

  // 序列化为 string。失败时 out 不被修改。
  bool SerializeToString(std::string* out) const;

  // 从 string 解析（先 Clear 再 Decode）。
  bool ParseFromString(const std::string& data);

  // 从裸内存解析。data 为 nullptr 或 len 为 0 时视为空消息并返回 true。
  bool ParseFromArray(const void* data, std::size_t len);

  // 编码后的字节数（内部用 Counting Writer 跑一遍 Encode，不分配内存）。
  std::size_t ByteSize() const;

  // 未知字段（只读）。
  const std::vector<UnknownField>& unknown_fields() const noexcept { return unknown_fields_; }

  // 追加一个未知字段（由生成的 Decode 调用）。
  void AddUnknownField(UnknownField field);

  // 清空未知字段。
  void ClearUnknownFields();

 protected:
  Message();

 private:
  std::vector<UnknownField> unknown_fields_;
};

}  // namespace serde
}  // namespace qzrpc
