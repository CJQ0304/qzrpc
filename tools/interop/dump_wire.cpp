// 轻舟 RPC —— 互通验证的 C++ 端
//
// 两个模式：
//   dump_wire encode
//       构造一个覆盖全部边界值的 demo.EchoRequest，用 qzrpc 自研序列化编码，
//       把字节流以十六进制打印到 stdout。供 Python 侧用官方 protobuf 解析。
//   dump_wire decode <hex>
//       把十六进制串交给 qzrpc 解析，然后按固定格式打印每个字段。
//       供 Python 侧与自己的解析结果逐行比对（反向验证）。
//
// 这个工具刻意放在 tools/ 而不是 example/：它不是框架的使用示例，
// 而是"证明协议兼容"的验收装置。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "echo.qz.h"
#include "serialize/serializer.h"

namespace {

// 边界值样本：每一个都对应一个已知的编码陷阱。
void BuildSample(demo::EchoRequest* req) {
  // UTF-8 多字节，验证长度前缀按字节算而不是按字符算。
  req->set_message("hello, \xE8\xBD\xBB\xE8\x88\x9F");  // "hello, 轻舟"

  // 负 int32：必须符号扩展到 64 位再 varint（10 字节），否则对端读到正数。
  req->set_repeat(-7);

  // packed 负数 + 边界：0 / 127（1 字节）/ 128（2 字节）/ -1 / INT32_MIN / INT32_MAX。
  const std::int32_t ids[] = {0, 127, 128, -1, INT32_MIN, INT32_MAX};
  for (std::int32_t v : ids) req->add_ids(v);

  // repeated string：逐元素带 tag；含空串与多字节。
  req->add_tags("a");
  req->add_tags("");
  req->add_tags("\xE4\xB8\xAD\xE6\x96\x87");  // "中文"

  // optional：显式 set 成空串，编码后必须存在该字段（presence 语义）。
  req->set_lang("");

  // 嵌套消息 + double 负小数 + bool。
  demo::EchoBody* body = req->mutable_body();
  body->set_text("nested");
  body->set_weight(-3);
  body->set_score(-2.5);
  body->set_valid(true);

  // bytes：含 0x00 与 0xFF，验证长度定界而不是 C 字符串。
  req->set_raw(std::string("\x00\xFF\x01", 3));

  req->set_site(demo::SiteType::VIDEO);
}

std::string ToHex(const std::string& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

std::string FromHex(const std::string& hex) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return std::string();
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

// 固定格式输出：Python 侧用完全相同的规则拼串，逐字节比对。
void DumpCanonical(const demo::EchoRequest& req) {
  std::printf("message=%s\n", ToHex(req.message()).c_str());
  std::printf("repeat=%d\n", static_cast<int>(req.repeat()));

  std::string ids;
  for (std::size_t i = 0; i < req.ids_size(); ++i) {
    if (i != 0) ids.push_back(',');
    ids += std::to_string(req.ids(i));
  }
  std::printf("ids=%s\n", ids.c_str());

  std::string tags;
  for (std::size_t i = 0; i < req.tags_size(); ++i) {
    if (i != 0) tags.push_back(',');
    tags += ToHex(req.tags(i));
  }
  std::printf("tags=%s\n", tags.c_str());

  std::printf("has_lang=%d\n", req.has_lang() ? 1 : 0);
  std::printf("lang=%s\n", ToHex(req.lang()).c_str());

  std::printf("has_body=%d\n", req.has_body() ? 1 : 0);
  std::printf("body.text=%s\n", ToHex(req.body().text()).c_str());
  std::printf("body.weight=%d\n", static_cast<int>(req.body().weight()));
  std::printf("body.score=%.17g\n", req.body().score());
  std::printf("body.valid=%d\n", req.body().valid() ? 1 : 0);

  std::printf("raw=%s\n", ToHex(req.raw()).c_str());
  std::printf("site=%d\n", static_cast<int>(req.site()));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s encode | decode <hex>\n", argv[0]);
    return 2;
  }

  const std::string mode = argv[1];

  if (mode == "encode") {
    demo::EchoRequest req;
    BuildSample(&req);

    std::string wire;
    if (!req.SerializeToString(&wire)) {
      std::fprintf(stderr, "serialize failed\n");
      return 1;
    }
    std::printf("%s\n", ToHex(wire).c_str());
    return 0;
  }

  if (mode == "decode") {
    if (argc < 3) {
      std::fprintf(stderr, "decode requires hex\n");
      return 2;
    }
    std::string wire = FromHex(argv[2]);
    demo::EchoRequest req;
    if (!req.ParseFromString(wire)) {
      std::fprintf(stderr, "parse failed\n");
      return 1;
    }
    DumpCanonical(req);
    return 0;
  }

  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  return 2;
}
