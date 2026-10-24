// 轻舟 RPC (Qingzhou RPC)
// tests/test_buffer.cpp
//
// 覆盖 common/buffer.h：扩容、compaction、Retrieve 越界、AppendFrom、ShrinkToFit、
// 移动语义，以及"0 字节"和"跨扩容点"这两类最容易写错的边界。
//
// Buffer 是全项目最紧的编译期依赖（net 的每条连接都持有两个），这个测试是它的安全网。

#include <cstring>
#include <string>

#include "common/buffer.h"
#include "test_main.h"

namespace {

// 确定性图案，用于在扩容/compaction 之后校验内容没有错位。
std::string MakePattern(std::size_t length, char base = 'a') {
  std::string pattern(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    pattern[i] = static_cast<char>(base + static_cast<char>(i % 26));
  }
  return pattern;
}

void TestDefaultState() {
  qzrpc::Buffer buffer;
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
  QZ_CHECK_EQ(buffer.capacity(), qzrpc::Buffer::kDefaultInitialCapacity);
  QZ_CHECK_EQ(buffer.WritableBytes(), qzrpc::Buffer::kDefaultInitialCapacity);
  QZ_CHECK(buffer.PeekView().empty());
  QZ_CHECK_EQ(buffer.DebugStateString(), std::string("r=0 w=0 cap=1024"));
}

// 显式传 0 表示"按需分配"，此时不能崩，也不能有非空的 Peek 视图。
void TestZeroInitialCapacity() {
  qzrpc::Buffer buffer(0);
  QZ_CHECK_EQ(buffer.capacity(), std::size_t{0});
  QZ_CHECK_EQ(buffer.WritableBytes(), std::size_t{0});
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
  QZ_CHECK(buffer.PeekView().empty());

  buffer.Append(std::string("abc"));
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{3});
  QZ_CHECK(buffer.capacity() >= qzrpc::Buffer::kDefaultInitialCapacity);
  QZ_CHECK_EQ(buffer.RetrieveAllAsString(), std::string("abc"));
}

void TestAppendAndPeek() {
  qzrpc::Buffer buffer;
  const std::string payload = "hello, qingzhou";
  buffer.Append(payload);

  QZ_CHECK_EQ(buffer.ReadableBytes(), payload.size());
  QZ_CHECK_EQ(std::string(buffer.Peek(), buffer.ReadableBytes()), payload);
  QZ_CHECK_EQ(std::string(buffer.PeekView()), payload);

  // 追加第二个片段，可读区必须是"拼接"而不是"覆盖"
  buffer.Append(std::string(" rpc"));
  QZ_CHECK_EQ(buffer.ReadableBytes(), payload.size() + 4);
  QZ_CHECK_EQ(std::string(buffer.Peek(), buffer.ReadableBytes()), payload + " rpc");
}

// nullptr / 长度 0 的追加必须是空操作（调用方不需要在每个 IO 分支里判空）。
void TestEmptyAppendIsNoOp() {
  qzrpc::Buffer buffer;
  buffer.Append(nullptr, 16);
  buffer.Append(nullptr, 0);
  buffer.Append(std::string());
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
  QZ_CHECK_EQ(buffer.DebugStateString(), std::string("r=0 w=0 cap=1024"));

  buffer.Append("x", 1);
  buffer.Append(nullptr, 4);
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{1});
}

// WriteBegin + HasWritten 是零拷贝写入路径，必须精确对齐下标。
void TestWriteBeginHasWritten() {
  qzrpc::Buffer buffer;
  char* writable = buffer.WriteBegin();
  QZ_CHECK(writable != nullptr);
  std::memcpy(writable, "abcde", 5);
  buffer.HasWritten(5);

  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{5});
  QZ_CHECK_EQ(std::string(buffer.Peek(), buffer.ReadableBytes()), std::string("abcde"));
  QZ_CHECK_EQ(buffer.WritableBytes(), qzrpc::Buffer::kDefaultInitialCapacity - 5);

  // 前移后再写第二个片段
  std::memcpy(buffer.WriteBegin(), "fg", 2);
  buffer.HasWritten(2);
  QZ_CHECK_EQ(buffer.RetrieveAllAsString(), std::string("abcdefg"));
}

void TestRetrieve() {
  qzrpc::Buffer buffer;
  buffer.Append(std::string("0123456789"));

  buffer.Retrieve(4);
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{6});
  QZ_CHECK_EQ(std::string(buffer.PeekView()), std::string("456789"));

  buffer.Skip(2);  // Skip 等价于 Retrieve
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{4});
  QZ_CHECK_EQ(std::string(buffer.PeekView()), std::string("6789"));

  buffer.Retrieve(0);  // 消费 0 字节不应改变任何状态
  QZ_CHECK_EQ(std::string(buffer.PeekView()), std::string("6789"));
}

// n 超过可读字节数时按头文件约定等价于 RetrieveAll()，不得越界。
void TestRetrieveOverflow() {
  qzrpc::Buffer buffer;
  buffer.Append(std::string("abc"));
  buffer.Retrieve(100);
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
  QZ_CHECK_EQ(buffer.DebugStateString(), std::string("r=0 w=0 cap=1024"));

  // 先消费一部分，再用超大 n 触发 RetrieveAll
  buffer.Append(std::string("defg"));
  buffer.Retrieve(1);
  buffer.Retrieve(1000);
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
}

void TestReadBytesInsufficient() {
  qzrpc::Buffer buffer;
  buffer.Append(std::string("hello"));

  char out[8] = {0};
  QZ_CHECK_EQ(buffer.ReadBytes(out, 6), false);  // 可读不足 -> false
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{5});  // 且不消费
  QZ_CHECK_EQ(buffer.ReadBytes(nullptr, 3), false);

  QZ_CHECK(buffer.ReadBytes(out, 3));
  QZ_CHECK_EQ(std::string(out, 3), std::string("hel"));
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{2});

  // 读 0 字节是合法操作
  QZ_CHECK(buffer.ReadBytes(out, 0));
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{2});
}

void TestReadString() {
  qzrpc::Buffer buffer;
  buffer.Append(std::string("payload!"));  // 8 字节

  std::string out = "stale content must be overwritten";
  QZ_CHECK_EQ(buffer.ReadString(&out, 9), false);  // 不足
  QZ_CHECK_EQ(out, std::string("stale content must be overwritten"));

  QZ_CHECK(buffer.ReadString(&out, 8));
  QZ_CHECK_EQ(out, std::string("payload!"));
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});

  QZ_CHECK_EQ(buffer.ReadString(nullptr, 1), false);
}

void TestRetrieveAllAsString() {
  qzrpc::Buffer buffer;
  QZ_CHECK_EQ(buffer.RetrieveAllAsString(), std::string());

  buffer.Append(std::string("xyz"));
  QZ_CHECK_EQ(buffer.RetrieveAllAsString(), std::string("xyz"));
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});
  QZ_CHECK_EQ(buffer.RetrieveAllAsString(), std::string());  // 空 buffer 取串不崩
}

// 逐字节追加，跨越初始容量（1024）后必须"翻倍"增长且内容不错位。
void TestGrowthAcrossCapacity() {
  const std::size_t total = 5000;
  const std::string pattern = MakePattern(total);

  qzrpc::Buffer buffer;  // 初始 1024
  for (std::size_t i = 0; i < total; ++i) {
    buffer.Append(pattern.data() + i, 1);
    if (i + 1 == qzrpc::Buffer::kDefaultInitialCapacity) {
      QZ_CHECK_EQ(buffer.ReadableBytes(), qzrpc::Buffer::kDefaultInitialCapacity);
    }
  }
  QZ_CHECK_EQ(buffer.ReadableBytes(), total);
  QZ_CHECK(buffer.capacity() >= total);
  QZ_CHECK_EQ(std::string(buffer.PeekView()), pattern);

  // 越过 1024 之后容量至少翻倍（2048），否则逐字节追加会退化成 O(n^2)
  qzrpc::Buffer doubling;
  doubling.Append(std::string(qzrpc::Buffer::kDefaultInitialCapacity, 'z'));
  doubling.Append(std::string(1, 'z'));
  QZ_CHECK(doubling.capacity() >= qzrpc::Buffer::kDefaultInitialCapacity * 2);
  QZ_CHECK_EQ(doubling.ReadableBytes(), qzrpc::Buffer::kDefaultInitialCapacity + 1);
}

// 可写区不足时应该先 compaction（前移可读数据），而不是无脑扩容。
void TestCompactionWithoutGrowth() {
  qzrpc::Buffer buffer(qzrpc::Buffer::kDefaultInitialCapacity);
  const std::string payload(900, 'h');
  buffer.Append(payload);
  buffer.Retrieve(500);  // read=500 write=900 readable=400 writable=124

  const std::size_t capacity_before = buffer.capacity();
  QZ_CHECK_EQ(buffer.WritableBytes(), std::size_t{124});

  buffer.EnsureWritable(200);  // 200 > 124 -> compaction 后 writable=624，够用

  QZ_CHECK_EQ(buffer.capacity(), capacity_before);  // 没有扩容
  QZ_CHECK(buffer.DebugStateString().rfind("r=0 ", 0) == 0);  // 读下标已归零
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{400});
  QZ_CHECK_EQ(buffer.WritableBytes(), capacity_before - 400);
  QZ_CHECK_EQ(std::string(buffer.PeekView()), payload.substr(500));
}

// compaction 之后仍然不够 -> 扩容，且扩容前已 compact，最终容量 = max(2*cap, readable+n)
void TestCompactionThenGrowth() {
  qzrpc::Buffer buffer(qzrpc::Buffer::kDefaultInitialCapacity);
  buffer.Append(std::string(1000, 'a'));
  buffer.Retrieve(600);  // read=600 write=1000 readable=400 writable=24

  buffer.EnsureWritable(1000);  // compact -> 400+1000=1400 > 1024 -> 扩容

  QZ_CHECK(buffer.capacity() >= 1400);
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{400});
  QZ_CHECK_EQ(std::string(buffer.PeekView()), std::string(400, 'a'));
  QZ_CHECK(buffer.DebugStateString().rfind("r=0 ", 0) == 0);
}

void TestAppendFrom() {
  qzrpc::Buffer source;
  source.Append(std::string("hello "));
  source.Append(std::string("world"));

  qzrpc::Buffer target;
  target.Append(std::string("["));
  target.AppendFrom(&source);

  QZ_CHECK_EQ(std::string(target.PeekView()), std::string("[hello world"));
  QZ_CHECK_EQ(source.ReadableBytes(), std::size_t{0});  // 源被清空

  // 空源是空操作
  target.AppendFrom(&source);
  QZ_CHECK_EQ(std::string(target.PeekView()), std::string("[hello world"));

  // 自追加必须是空操作（否则 memcpy 区间重叠，未定义行为）
  target.AppendFrom(&target);
  QZ_CHECK_EQ(std::string(target.PeekView()), std::string("[hello world"));

  target.AppendFrom(nullptr);
  QZ_CHECK_EQ(std::string(target.PeekView()), std::string("[hello world"));
}

void TestShrinkToFit() {
  qzrpc::Buffer buffer;
  buffer.Append(std::string(100, 'x'));
  QZ_CHECK(buffer.capacity() >= 1024);

  buffer.ShrinkToFit();
  QZ_CHECK(buffer.capacity() < qzrpc::Buffer::kDefaultInitialCapacity);
  QZ_CHECK(buffer.capacity() >= 100);
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{100});
  QZ_CHECK_EQ(std::string(buffer.PeekView()), std::string(100, 'x'));

  // 有"前缀空洞"时收缩，读下标也必须归零，否则数据会整体错位
  qzrpc::Buffer holed;
  holed.Append(std::string(50, 'y'));
  holed.Retrieve(20);
  holed.ShrinkToFit();
  QZ_CHECK(holed.DebugStateString().rfind("r=0 ", 0) == 0);
  QZ_CHECK_EQ(holed.ReadableBytes(), std::size_t{30});
  QZ_CHECK_EQ(std::string(holed.PeekView()), std::string(30, 'y'));
}

void TestReserve() {
  qzrpc::Buffer buffer;
  buffer.Reserve(4096);
  QZ_CHECK_EQ(buffer.capacity(), std::size_t{4096});
  QZ_CHECK_EQ(buffer.ReadableBytes(), std::size_t{0});

  buffer.Reserve(100);  // 更小的请求不该收缩容量
  QZ_CHECK_EQ(buffer.capacity(), std::size_t{4096});
}

void TestLargeAppend() {
  const std::size_t total = 200000;  // 远大于 16MB 之外的所有常见扩容点
  const std::string pattern = MakePattern(total, 'A');

  qzrpc::Buffer buffer;
  buffer.Append(pattern.data(), pattern.size());
  QZ_CHECK_EQ(buffer.ReadableBytes(), total);
  QZ_CHECK(buffer.capacity() >= total);
  QZ_CHECK(buffer.RetrieveAllAsString() == pattern);
}

void TestMoveSemantics() {
  qzrpc::Buffer source;
  source.Append(std::string("moved payload"));

  qzrpc::Buffer moved(std::move(source));
  QZ_CHECK_EQ(std::string(moved.PeekView()), std::string("moved payload"));
  QZ_CHECK_EQ(source.ReadableBytes(), std::size_t{0});  // 被移动方处于合法的空状态

  qzrpc::Buffer assigned;
  assigned.Append(std::string("old"));
  assigned = std::move(moved);
  QZ_CHECK_EQ(std::string(assigned.PeekView()), std::string("moved payload"));
  QZ_CHECK_EQ(moved.ReadableBytes(), std::size_t{0});

  // 移动后仍可继续使用（不是"僵尸"对象）
  source.Append(std::string("reuse"));
  QZ_CHECK_EQ(std::string(source.PeekView()), std::string("reuse"));
}

void TestDebugStateString() {
  qzrpc::Buffer buffer(64);
  buffer.Append(std::string(10, 'q'));
  buffer.Retrieve(4);
  QZ_CHECK_EQ(buffer.DebugStateString(), std::string("r=4 w=10 cap=64"));
}

}  // namespace

int main() {
  TestDefaultState();
  TestZeroInitialCapacity();
  TestAppendAndPeek();
  TestEmptyAppendIsNoOp();
  TestWriteBeginHasWritten();
  TestRetrieve();
  TestRetrieveOverflow();
  TestReadBytesInsufficient();
  TestReadString();
  TestRetrieveAllAsString();
  TestGrowthAcrossCapacity();
  TestCompactionWithoutGrowth();
  TestCompactionThenGrowth();
  TestAppendFrom();
  TestShrinkToFit();
  TestReserve();
  TestLargeAppend();
  TestMoveSemantics();
  TestDebugStateString();
  return qztest::Summary("test_buffer");
}
