// 轻舟 RPC (Qingzhou RPC)
// tests/test_endian.cpp
//
// 覆盖 common/endian.h：大端往返、线上字节布局、未对齐指针读写、边界值。
// 这些是帧头解析的地基 —— 写反了 protobuf 解析器还能读但值全错，所以必须逐字节断言。

#include <cstdint>
#include <cstring>

#include "common/endian.h"
#include "test_main.h"

namespace {

void TestBig16Layout() {
  unsigned char buffer[2] = {0, 0};
  qzrpc::StoreBig16(buffer, static_cast<std::uint16_t>(0x1234));
  QZ_CHECK_EQ(buffer[0], 0x12);  // 高字节在前
  QZ_CHECK_EQ(buffer[1], 0x34);
}

void TestBig32Layout() {
  unsigned char buffer[4] = {0, 0, 0, 0};
  qzrpc::StoreBig32(buffer, 0xDEADBEEFU);
  QZ_CHECK_EQ(buffer[0], 0xDE);
  QZ_CHECK_EQ(buffer[1], 0xAD);
  QZ_CHECK_EQ(buffer[2], 0xBE);
  QZ_CHECK_EQ(buffer[3], 0xEF);
}

void TestBig64Layout() {
  unsigned char buffer[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  qzrpc::StoreBig64(buffer, 0x0123456789ABCDEFULL);
  QZ_CHECK_EQ(buffer[0], 0x01);
  QZ_CHECK_EQ(buffer[1], 0x23);
  QZ_CHECK_EQ(buffer[2], 0x45);
  QZ_CHECK_EQ(buffer[3], 0x67);
  QZ_CHECK_EQ(buffer[4], 0x89);
  QZ_CHECK_EQ(buffer[5], 0xAB);
  QZ_CHECK_EQ(buffer[6], 0xCD);
  QZ_CHECK_EQ(buffer[7], 0xEF);
}

void TestLoadFromKnownBytes() {
  const unsigned char bytes16[2] = {0x00, 0x2A};
  QZ_CHECK_EQ(qzrpc::LoadBig16(bytes16), static_cast<std::uint16_t>(42));

  const unsigned char bytes32[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  QZ_CHECK_EQ(qzrpc::LoadBig32(bytes32), 0xFFFFFFFFU);

  const unsigned char bytes64[8] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
  QZ_CHECK_EQ(qzrpc::LoadBig64(bytes64), 0x0000000100000000ULL);
}

void TestRoundTrip16() {
  const std::uint16_t values[] = {0, 1, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFF};
  for (const std::uint16_t value : values) {
    unsigned char buffer[2] = {0, 0};
    qzrpc::StoreBig16(buffer, value);
    QZ_CHECK_EQ(qzrpc::LoadBig16(buffer), value);
  }
}

void TestRoundTrip32() {
  const std::uint32_t values[] = {0,         1,          0xFF,       0x100,
                                  0x0000FFFFU, 0xFFFF0000U, 0x7FFFFFFFU, 0x80000000U,
                                  0xFFFFFFFFU, 0x21678893U};
  for (const std::uint32_t value : values) {
    unsigned char buffer[4] = {0, 0, 0, 0};
    qzrpc::StoreBig32(buffer, value);
    QZ_CHECK_EQ(qzrpc::LoadBig32(buffer), value);
  }
}

void TestRoundTrip64() {
  const std::uint64_t values[] = {0,
                                  1,
                                  0xFFULL,
                                  0x100ULL,
                                  0xFFFFFFFFULL,
                                  0x100000000ULL,
                                  0x7FFFFFFFFFFFFFFFULL,
                                  0x8000000000000000ULL,
                                  0xFFFFFFFFFFFFFFFFULL,
                                  0xCBF29CE484222325ULL};
  for (const std::uint64_t value : values) {
    unsigned char buffer[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    qzrpc::StoreBig64(buffer, value);
    QZ_CHECK_EQ(qzrpc::LoadBig64(buffer), value);
  }
}

// 未对齐读写：帧头在 Buffer 里往往是"从任意偏移开始"的一段，必须用 memcpy 而不是
// reinterpret_cast 到 uint32_t*（在 ARM 上会直接崩，x86 上则是 UB）。
void TestUnalignedAccess() {
  unsigned char buffer[16];
  std::memset(buffer, 0xA5, sizeof(buffer));

  for (std::size_t offset = 1; offset <= 8; ++offset) {
    std::memset(buffer, 0xA5, sizeof(buffer));  // 每轮重置：上一轮的写入会污染本轮的前缀
    qzrpc::StoreBig32(buffer + offset, 0xDEADBEEFU);
    QZ_CHECK_EQ(qzrpc::LoadBig32(buffer + offset), 0xDEADBEEFU);
    // 前后一个字节必须没被踩坏
    QZ_CHECK_EQ(buffer[offset - 1], 0xA5);
    QZ_CHECK_EQ(buffer[offset + 4], 0xA5);
  }

  std::memset(buffer, 0x5A, sizeof(buffer));
  qzrpc::StoreBig64(buffer + 3, 0x0123456789ABCDEFULL);
  QZ_CHECK_EQ(qzrpc::LoadBig64(buffer + 3), 0x0123456789ABCDEFULL);
  QZ_CHECK_EQ(buffer[2], 0x5A);
  QZ_CHECK_EQ(buffer[11], 0x5A);
}

// HostToBig / BigToHost 必须是同一个位运算，互为逆运算。
void TestHostBigSymmetry() {
  const std::uint32_t original = 0x11223344U;
  QZ_CHECK_EQ(qzrpc::BigToHost32(qzrpc::HostToBig32(original)), original);
  QZ_CHECK_EQ(qzrpc::BigToHost16(qzrpc::HostToBig16(static_cast<std::uint16_t>(0x1122))),
              static_cast<std::uint16_t>(0x1122));
  QZ_CHECK_EQ(qzrpc::BigToHost64(qzrpc::HostToBig64(0x1122334455667788ULL)),
              0x1122334455667788ULL);
}

// 小端序主机上转换必须真的换字节；大端序主机上必须原样返回。
// 两种主机都应满足："转换后写出的字节 == 大端表示"。
void TestEndiannessDetection() {
  const std::uint32_t value = 0x01020304U;
  unsigned char as_converted[4];
  const std::uint32_t converted = qzrpc::HostToBig32(value);
  std::memcpy(as_converted, &converted, sizeof(as_converted));
  QZ_CHECK_EQ(as_converted[0], 0x01);
  QZ_CHECK_EQ(as_converted[1], 0x02);
  QZ_CHECK_EQ(as_converted[2], 0x03);
  QZ_CHECK_EQ(as_converted[3], 0x04);
}

}  // namespace

int main() {
  TestBig16Layout();
  TestBig32Layout();
  TestBig64Layout();
  TestLoadFromKnownBytes();
  TestRoundTrip16();
  TestRoundTrip32();
  TestRoundTrip64();
  TestUnalignedAccess();
  TestHostBigSymmetry();
  TestEndiannessDetection();
  return qztest::Summary("test_endian");
}
