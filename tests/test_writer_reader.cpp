// 轻舟 RPC (Qingzhou RPC)
// tests/test_writer_reader.cpp
//
// 覆盖 serialize/writer.h + serialize/reader.h —— protobuf wire format 兼容性（doc 03 §11）。
// 这是协议正确性的地基：这里的编码一旦和 protobuf 不一致，任何"我们兼容 protobuf"的
// 说法都不成立，而且症状只会在与外部 protobuf 实现互通时才暴露。
//
// 断言尽量写成**逐字节的十六进制比对**，而不是"写出去再读回来相等" ——
// 后者对"写错也读错"的对称 bug 完全无感（例如 fixed32 写成大端、读也按大端）。

#include <cstdint>
#include <string>

#include "common/string_util.h"
#include "serialize/reader.h"
#include "serialize/writer.h"
#include "test_main.h"

namespace {

// "0A0268 69" -> 原始字节。用 HexDecode（自己实现并有单测）解出，便于把 golden
// 字节串写成人类可读形式。
std::string Bytes(const char* hex) {
  std::string out;
  const bool ok = qzrpc::StringUtil::HexDecode(std::string(hex), &out);
  QZ_EXPECT(ok);
  return out;
}

std::string ToHex(const std::string& raw) {
  return qzrpc::StringUtil::HexEncode(raw.data(), raw.size());
}

// 断言 writer 当前输出（十六进制，忽略大小写 —— HexEncode 产出小写）。
void ExpectWriter(qzrpc::serde::Writer& writer, const char* expected_hex) {
  QZ_CHECK_EQ(ToHex(writer.data()), ToHex(Bytes(expected_hex)));
}

void ExpectSize(qzrpc::serde::Writer& writer, std::size_t expected) {
  QZ_CHECK_EQ(writer.size(), expected);
}

// ---------------------------------------------------------------------------
// varint 边界：1/2/3/4/5/10 字节
// ---------------------------------------------------------------------------

void TestVarintBoundaries() {
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(0);
    ExpectWriter(w, "00");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(127);  // 单字节上限
    ExpectWriter(w, "7F");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(128);  // 跨到 2 字节
    ExpectWriter(w, "8001");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(16383);  // 2 字节上限
    ExpectWriter(w, "FF7F");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(16384);  // 跨到 3 字节
    ExpectWriter(w, "808001");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(2097151U);  // 2^21-1，3 字节上限
    ExpectWriter(w, "FFFF7F");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(268435455U);  // 2^28-1，4 字节上限
    ExpectWriter(w, "FFFFFF7F");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint32(0xFFFFFFFFU);  // uint32 上限 = 5 字节
    ExpectWriter(w, "FFFFFFFF0F");
    ExpectSize(w, 5);
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint64(0xFFFFFFFFFFFFFFFFULL);  // uint64 上限 = 10 字节
    ExpectWriter(w, "FFFFFFFFFFFFFFFFFF01");
    ExpectSize(w, 10);
  }
  {
    qzrpc::serde::Writer w;
    w.WriteVarint64(0x100000000ULL);  // 2^32，跨到第 5 字节之后
    ExpectWriter(w, "8080808010");
  }
}

// ---------------------------------------------------------------------------
// 负数 int32 必须是 **10 字节** varint（符号扩展到 64 位）
//
// 这是 protobuf 兼容性最经典的一处：把 -1 写成 5 字节（低 32 位 0xFFFFFFFF）时，
// protobuf 解析器会读成 +4294967295 —— 编译过、我们自己的 reader 也能"读回来相等"
// （因为它按 32 位截断），只有与外部实现互通时才炸。所以这里必须逐字节断言。
// ---------------------------------------------------------------------------

void TestNegativeInt32IsTenBytes() {
  {
    qzrpc::serde::Writer w;
    w.WriteInt32(-1);
    ExpectWriter(w, "FFFFFFFFFFFFFFFFFF01");
    ExpectSize(w, 10);  // 绝不能是 5
  }
  {
    qzrpc::serde::Writer w;
    w.WriteInt32(-2);
    ExpectWriter(w, "FEFFFFFFFFFFFFFFFF01");
    ExpectSize(w, 10);
  }
  {
    qzrpc::serde::Writer w;
    w.WriteInt32(INT32_MIN);  // -2147483648
    ExpectWriter(w, "80808080F8FFFFFFFF01");
    ExpectSize(w, 10);
  }
  {
    // 正数仍然走最短编码，不受"负数 10 字节"规则影响
    qzrpc::serde::Writer w;
    w.WriteInt32(INT32_MAX);
    ExpectWriter(w, "FFFFFFFF07");
    ExpectSize(w, 5);
  }
  {
    qzrpc::serde::Writer w;
    w.WriteInt64(INT64_MIN);
    ExpectWriter(w, "80808080808080808001");
    ExpectSize(w, 10);
  }

  // 读回来必须还是那个负数（负数 10 字节形式 -> 截断到 32 位）
  {
    qzrpc::serde::Writer w;
    w.WriteInt32(-1);
    w.WriteInt32(INT32_MIN);
    w.WriteInt32(-123456789);
    const std::string bytes = w.data();
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    QZ_CHECK(r.ReadInt32(&a));
    QZ_CHECK(r.ReadInt32(&b));
    QZ_CHECK(r.ReadInt32(&c));
    QZ_CHECK_EQ(a, -1);
    QZ_CHECK_EQ(b, INT32_MIN);
    QZ_CHECK_EQ(c, -123456789);
    QZ_CHECK(!r.failed());
    QZ_CHECK(r.AtEnd());
  }
}

// ---------------------------------------------------------------------------
// zigzag：sint32/sint64
// zigzag(n) = (n << 1) ^ (n >> 31)（32 位算术右移）
// ---------------------------------------------------------------------------

void TestZigzag() {
  {
    qzrpc::serde::Writer w;
    w.WriteSInt32(-1);  // -> 1
    ExpectWriter(w, "01");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteSInt32(1);  // -> 2
    ExpectWriter(w, "02");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteSInt32(-2);  // -> 3
    ExpectWriter(w, "03");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteSInt32(0);
    ExpectWriter(w, "00");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteSInt32(INT32_MIN);  // -> 0xFFFFFFFF（zigzag 的著名性质）
    ExpectWriter(w, "FFFFFFFF0F");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteSInt32(INT32_MAX);  // -> 0xFFFFFFFE
    ExpectWriter(w, "FEFFFFFF0F");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteSInt64(INT64_MIN);  // -> UINT64_MAX
    ExpectWriter(w, "FFFFFFFFFFFFFFFFFF01");
  }

  // 往返：zigzag 必须严格单调映射，不能把两个不同值映到同一编码
  {
    const std::int32_t values[] = {0,     1,      -1,     2,       -2,   12345,
                                   -12345, 1000000, -1000000, INT32_MAX, INT32_MIN};
    for (const std::int32_t value : values) {
      qzrpc::serde::Writer w;
      w.WriteSInt32(value);
      const std::string bytes = w.data();
      qzrpc::serde::Reader r(bytes.data(), bytes.size());
      std::int32_t back = 0;
      QZ_CHECK(r.ReadSInt32(&back));
      QZ_CHECK_EQ(back, value);
      QZ_CHECK(r.AtEnd());
    }
  }
  {
    const std::int64_t values[] = {0, 1, -1, INT64_MAX, INT64_MIN, -1234567890123LL};
    for (const std::int64_t value : values) {
      qzrpc::serde::Writer w;
      w.WriteSInt64(value);
      const std::string bytes = w.data();
      qzrpc::serde::Reader r(bytes.data(), bytes.size());
      std::int64_t back = 0;
      QZ_CHECK(r.ReadSInt64(&back));
      QZ_CHECK_EQ(back, value);
    }
  }
}

// ---------------------------------------------------------------------------
// fixed32/fixed64 是 **小端**（protobuf 规定），而帧头是大端 ——
// 同一个项目里两种字节序并存，这里把"小端"钉死。
// ---------------------------------------------------------------------------

void TestFixedIsLittleEndian() {
  {
    qzrpc::serde::Writer w;
    w.WriteFixed32(0x12345678U);
    ExpectWriter(w, "78563412");  // 小端：低字节在前
    ExpectSize(w, 4);
  }
  {
    qzrpc::serde::Writer w;
    w.WriteFixed32(0x00000001U);
    ExpectWriter(w, "01000000");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteFixed64(0x0102030405060708ULL);
    ExpectWriter(w, "0807060504030201");
    ExpectSize(w, 8);
  }
  {
    qzrpc::serde::Writer w;
    w.WriteFloat(1.0F);  // 0x3F800000 -> 小端 00 00 80 3F
    ExpectWriter(w, "0000803F");
  }
  {
    // -0.0 的位模式必须原样保留（这也是为什么用 memcpy 取位而不是算术转换）
    qzrpc::serde::Writer w;
    w.WriteFloat(-0.0F);
    ExpectWriter(w, "00000080");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteDouble(1.0);  // 0x3FF0000000000000 -> 小端
    ExpectWriter(w, "000000000000F03F");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteDouble(-0.0);
    ExpectWriter(w, "0000000000000080");
  }

  // 往返
  {
    qzrpc::serde::Writer w;
    w.WriteFixed32(0xDEADBEEFU);
    w.WriteFixed64(0x0123456789ABCDEFULL);
    w.WriteFloat(3.5F);
    w.WriteDouble(-2.25);
    const std::string bytes = w.data();
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    float f = 0.0F;
    double d = 0.0;
    QZ_CHECK(r.ReadFixed32(&u32));
    QZ_CHECK(r.ReadFixed64(&u64));
    QZ_CHECK(r.ReadFloat(&f));
    QZ_CHECK(r.ReadDouble(&d));
    QZ_CHECK_EQ(u32, 0xDEADBEEFU);
    QZ_CHECK_EQ(u64, 0x0123456789ABCDEFULL);
    QZ_CHECK_EQ(f, 3.5F);
    QZ_CHECK_EQ(d, -2.25);
    QZ_CHECK(r.AtEnd());
  }
}

// ---------------------------------------------------------------------------
// tag 编码：tag = (field_number << 3) | wire_type
// ---------------------------------------------------------------------------

void TestTagEncoding() {
  {
    qzrpc::serde::Writer w;
    w.WriteTag(1, 0);  // varint
    ExpectWriter(w, "08");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteTag(1, 2);  // length-delimited（string/bytes/message）—— EchoRequest.message 就是它
    ExpectWriter(w, "0A");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteTag(1, 5);  // fixed32
    ExpectWriter(w, "0D");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteTag(1, 1);  // fixed64
    ExpectWriter(w, "09");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteTag(16, 0);  // field 16 需要 2 字节 tag
    ExpectWriter(w, "8001");
  }

  // 往返
  {
    qzrpc::serde::Writer w;
    w.WriteTag(1, 2);
    w.WriteTag(17, 0);
    w.WriteTag(536870911U, 5);  // field_number 上限 2^29-1
    const std::string bytes = w.data();
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::uint32_t field = 0;
    std::uint8_t wire = 0;
    QZ_CHECK(r.ReadTag(&field, &wire));
    QZ_CHECK_EQ(field, 1U);
    QZ_CHECK_EQ(wire, 2);
    QZ_CHECK(r.ReadTag(&field, &wire));
    QZ_CHECK_EQ(field, 17U);
    QZ_CHECK_EQ(wire, 0);
    QZ_CHECK(r.ReadTag(&field, &wire));
    QZ_CHECK_EQ(field, 536870911U);
    QZ_CHECK_EQ(wire, 5);
    QZ_CHECK(r.AtEnd());
  }
}

// ---------------------------------------------------------------------------
// length-delimited：空串 / 含 \0 的 bytes / 长度恰好等于剩余 / 长度超出剩余
// ---------------------------------------------------------------------------

void TestLengthDelimited() {
  {
    qzrpc::serde::Writer w;
    w.WriteString(std::string());  // 空串仍要写长度 0
    ExpectWriter(w, "00");
  }
  {
    qzrpc::serde::Writer w;
    w.WriteString(std::string("hi"));
    ExpectWriter(w, "026869");
  }
  {
    // 300 字节 -> 长度前缀是 varint(300) = AC 02（两字节），验证长度本身也是 varint
    qzrpc::serde::Writer w;
    w.WriteLengthDelimited(nullptr, 0);
    ExpectWriter(w, "00");

    qzrpc::serde::Writer w2;
    const std::string payload(300, 'x');
    w2.WriteLengthDelimited(payload.data(), payload.size());
    QZ_CHECK_EQ(ToHex(w2.data()).substr(0, 4), std::string("ac02"));
    QZ_CHECK_EQ(w2.size(), std::size_t{302});
  }
  {
    // bytes 里允许出现 '\0'，长度必须靠前缀而不是 C 串终止符
    const std::string raw("a\0b", 3);
    qzrpc::serde::Writer w;
    w.WriteBytes(raw.data(), raw.size());
    ExpectWriter(w, "03610062");

    const std::string bytes = w.data();
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::string back;
    QZ_CHECK(r.ReadBytes(&back));
    QZ_CHECK_EQ(back.size(), std::size_t{3});
    QZ_CHECK_EQ(back, raw);
    QZ_CHECK(r.AtEnd());
  }
  {
    // 零拷贝视图必须指向原始缓冲，而不是临时拷贝
    const std::string bytes = Bytes("026869");
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    const char* view = nullptr;
    std::size_t len = 0;
    QZ_CHECK(r.ReadLengthDelimited(&view, &len));
    QZ_CHECK_EQ(len, std::size_t{2});
    QZ_CHECK(view >= bytes.data() && view < bytes.data() + bytes.size());
    QZ_CHECK_EQ(std::string(view, len), std::string("hi"));
  }
  {
    // 长度恰好等于剩余字节：成功
    const std::string bytes = Bytes("03616263");  // len=3 "abc"
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::string out;
    QZ_CHECK(r.ReadString(&out));
    QZ_CHECK_EQ(out, std::string("abc"));
    QZ_CHECK(r.AtEnd());
  }
  {
    // 长度超出剩余字节：必须 false，且 out 不被写坏。
    //
    // 关于"是否消费"的实测结论：Reader 会先读出长度前缀（消费掉那 1 字节 varint），
    // 再发现 payload 不够而失败 —— 所以 bytes_left() 会少 1，不是 0 变化的原子操作。
    // 这不构成契约违背：头文件写明"读失败后 Reader 进入 failed 状态，后续调用一律
    // false"，失败是终态、位置不再有意义（调用方必须丢弃整个 Reader）。
    // 如果将来要在"喂更多数据后重试"的场景复用同一个 Reader，才需要位置回滚。
    const std::string bytes = Bytes("05616263");  // 声明 5 字节，实际只有 3
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::string out = "unchanged";
    QZ_CHECK(!r.ReadString(&out));
    QZ_CHECK_EQ(out, std::string("unchanged"));  // 失败时 out 不被写坏
    QZ_CHECK(r.failed());
    QZ_CHECK_EQ(r.bytes_left(), std::size_t{3});  // 只消费了长度前缀，payload 一个字节没动
    // 失败后位置冻结：再调用不改变任何状态
    const std::size_t frozen = r.bytes_left();
    char one = 0;
    QZ_CHECK(!r.ReadRaw(&one, 1));
    QZ_CHECK_EQ(r.bytes_left(), frozen);
  }
  {
    // 恶意长度：0xFFFFFFFF，必须拒绝而不是试图分配 4GB
    const std::string bytes = Bytes("FFFFFFFF0F6162");
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::string out;
    QZ_CHECK(!r.ReadString(&out));
    QZ_CHECK(r.failed());
  }
}

// ---------------------------------------------------------------------------
// Reader 的失败传染：一次失败之后，所有后续读取一律 false
// ---------------------------------------------------------------------------

void TestReaderFailureIsSticky() {
  {
    // 第一个 varint 是坏的（11 个续位字节，超过 10 字节上限）
    qzrpc::serde::Writer w;
    w.WriteRaw("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 11);
    w.WriteBool(true);
    const std::string bytes = w.data();

    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::uint64_t v = 0;
    QZ_CHECK(!r.ReadVarint64(&v));
    QZ_CHECK(r.failed());
    QZ_CHECK(r.error() != nullptr);
    QZ_CHECK(r.error()[0] != '\0');  // 必须有可读的失败原因

    // 后续读取即使数据合法也必须失败（generated code 依赖这个性质省掉错误传播）
    bool b = false;
    QZ_CHECK(!r.ReadBool(&b));
    std::uint32_t v32 = 0;
    QZ_CHECK(!r.ReadVarint32(&v32));
    std::string s;
    QZ_CHECK(!r.ReadString(&s));
    const char* view = nullptr;
    std::size_t len = 0;
    QZ_CHECK(!r.ReadLengthDelimited(&view, &len));
    std::uint8_t wire = 0;
    std::uint32_t field = 0;
    QZ_CHECK(!r.ReadTag(&field, &wire));
  }
  {
    // 空 reader：任何读取都失败，AtEnd 为真
    qzrpc::serde::Reader r(nullptr, 0);
    QZ_CHECK(r.AtEnd());
    QZ_CHECK_EQ(r.bytes_left(), std::size_t{0});
    std::uint32_t v = 0;
    QZ_CHECK(!r.ReadVarint32(&v));
    QZ_CHECK(r.failed());
  }
  {
    // 数据不足（varint 续位在中间断掉）
    const std::string bytes = Bytes("8080");
    qzrpc::serde::Reader r(bytes.data(), bytes.size());
    std::uint32_t v = 0;
    QZ_CHECK(!r.ReadVarint32(&v));
    QZ_CHECK(r.failed());
  }
}

// ---------------------------------------------------------------------------
// 未知字段跳过与"原始字节捕获"（protobuf 3 的 unknown field 语义）
// ---------------------------------------------------------------------------

void TestSkipAndCapture() {
  // tag(field=5, wt=2) + len 3 + "abc"，然后是 tag(field=6, wt=0) + varint 42
  const std::string bytes = Bytes("2A03616263302A");
  qzrpc::serde::Reader r(bytes.data(), bytes.size());
  std::uint32_t field = 0;
  std::uint8_t wire = 0;

  QZ_CHECK(r.ReadTag(&field, &wire));
  QZ_CHECK_EQ(field, 5U);
  QZ_CHECK_EQ(wire, 2);
  QZ_CHECK(r.SkipField(wire));
  QZ_CHECK_EQ(r.position(), std::size_t{5});  // 跳过了 tag(1) + len(1) + 3 字节

  // 捕获：跳过同时保留原始编码字节（不含 tag），写回时能拼回原样
  QZ_CHECK(r.ReadTag(&field, &wire));
  QZ_CHECK_EQ(field, 6U);
  QZ_CHECK_EQ(wire, 0);
  std::string captured;
  QZ_CHECK(r.SkipFieldCapture(wire, &captured));
  QZ_CHECK_EQ(ToHex(captured), std::string("2a"));  // varint 42 = 0x2A
  QZ_CHECK(r.AtEnd());

  // 非法 wire_type（3 是废弃的 group 起始）在 ReadTag 就被拒绝 —— 比"读出来再让
  // SkipField 拒绝"更严格：group 语义 protobuf 自己都已废弃，没必要放过第一道关。
  const std::string bad = Bytes("1B");  // tag = (3<<3)|3
  qzrpc::serde::Reader r2(bad.data(), bad.size());
  QZ_CHECK(!r2.ReadTag(&field, &wire));
  QZ_CHECK(r2.failed());
  QZ_CHECK(r2.error()[0] != '\0');

  // field number 0 同样非法
  const std::string zero_field = Bytes("02");  // tag = 0 -> (0<<3)|2
  qzrpc::serde::Reader r3(zero_field.data(), zero_field.size());
  QZ_CHECK(!r3.ReadTag(&field, &wire));
  QZ_CHECK(r3.failed());
}

// ---------------------------------------------------------------------------
// Counting 模式：ByteSize 用的"只计数不存储"，长度必须与真实写入一致
// ---------------------------------------------------------------------------

void TestCountingWriter() {
  qzrpc::serde::Writer real;
  qzrpc::serde::Writer counting = qzrpc::serde::Writer::Counting();
  QZ_CHECK(counting.counting());
  QZ_CHECK(!real.counting());

  const std::string text = "qingzhou rpc";
  for (qzrpc::serde::Writer* w : {&real, &counting}) {
    w->WriteTag(1, 2);
    w->WriteString(text);
    w->WriteTag(2, 0);
    w->WriteInt32(-7);
    w->WriteTag(3, 5);
    w->WriteFixed32(0xAABBCCDDU);
  }
  QZ_CHECK_EQ(counting.size(), real.size());
  QZ_CHECK(counting.data().empty());  // 计数模式不积累字节
  QZ_CHECK_EQ(real.size(), std::size_t{1 + 1 + text.size() + 1 + 10 + 1 + 4});

  counting.Reset();
  QZ_CHECK_EQ(counting.size(), std::size_t{0});
  real.Reset();
  QZ_CHECK_EQ(real.size(), std::size_t{0});
  QZ_CHECK_EQ(real.data().size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// bool / enum / WriteRaw
// ---------------------------------------------------------------------------

void TestMiscPrimitives() {
  {
    qzrpc::serde::Writer w;
    w.WriteBool(true);
    w.WriteBool(false);
    ExpectWriter(w, "0100");
  }
  {
    // enum 与 int32 同语义（负数同样 10 字节）
    qzrpc::serde::Writer w;
    w.WriteEnum(2);
    w.WriteEnum(-1);
    ExpectWriter(w, "02FFFFFFFFFFFFFFFFFF01");
  }
  {
    // WriteRaw 不带长度前缀
    qzrpc::serde::Writer w;
    w.WriteRaw("QZRP", 4);
    ExpectWriter(w, "515A5250");
  }
  {
    // ReleaseData 移出内容并清空
    qzrpc::serde::Writer w;
    w.WriteString(std::string("abc"));
    const std::string released = w.ReleaseData();
    QZ_CHECK_EQ(ToHex(released), std::string("03616263"));
    QZ_CHECK_EQ(w.size(), std::size_t{0});
  }
}

}  // namespace

int main() {
  TestVarintBoundaries();
  TestNegativeInt32IsTenBytes();
  TestZigzag();
  TestFixedIsLittleEndian();
  TestTagEncoding();
  TestLengthDelimited();
  TestReaderFailureIsSticky();
  TestSkipAndCapture();
  TestCountingWriter();
  TestMiscPrimitives();
  return qztest::Summary("test_writer_reader");
}
