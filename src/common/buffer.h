// 轻舟 RPC (Qingzhou RPC)
// src/common/buffer.h
//
// 职责：框架唯一的字节缓冲区实现，用于网络收发、编解码中转、帧拼装。
//
// 设计要点：
//   * 连续内存 + read/write 双下标（Netty ByteBuf 的极简版），不做分片链（chain），
//     因为 v1 的帧上限是 16MB，单块 vector 完全够用，分片只会增加复杂度。
//   * 可写区不足时**按 2 倍扩容**，并优先把可读数据前移到起始位置（compaction）再扩容。
//   * 无自动回收：长时间持有大 buffer 请显式调用 ShrinkToFit()。
//
// 线程安全：**非线程安全**。每个 Connection 的 input/output buffer 由该 Connection
//           所属 EventLoop 线程独占访问。跨线程传递数据必须整体 move 或拷贝。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qzrpc {

// 可增长的字节缓冲区。
class Buffer {
 public:
  // 默认初始容量，够放一个典型的 Echo 请求帧，避免小包频繁扩容。
  static constexpr std::size_t kDefaultInitialCapacity = 1024;

  // 构造。参数 initial_capacity - 预分配字节数，0 表示按需分配。
  // 注意："按需分配"有一个下限——capacity()==0 时，首次 EnsureWritable 仍会一次
  // 分配到 kDefaultInitialCapacity 而不是只分配恰好够用的字节。否则从 3 字节起步会
  // 走 3→6→12→24 的小步增长，长连接上每个 buffer 都要多付十几次 realloc。
  explicit Buffer(std::size_t initial_capacity = kDefaultInitialCapacity);

  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  ~Buffer();

  // ---- 写侧 ----

  // 返回可写区起始指针，有效长度为 WritableBytes()。
  // 注意：调用 Append/HasWritten 后指针可能失效（扩容导致 realloc）。
  char* WriteBegin() noexcept;
  const char* WriteBegin() const noexcept;

  // 当前可直接写入且无需扩容的字节数。
  std::size_t WritableBytes() const noexcept;

  // 提交写入：把写下标前移 n 字节。通常配合 WriteBegin() + 直接 memcpy 使用。
  // 前置条件：n <= WritableBytes()，否则调用 abort（编程错误，不是运行时错误）。
  void HasWritten(std::size_t n) noexcept;

  // 追加数据，自动扩容。data 为 nullptr 或 len 为 0 时是空操作。
  void Append(const void* data, std::size_t len);

  // 追加字符串。
  void Append(const std::string& s);

  // 追加另一个 buffer 的全部可读数据，并清空对方（零拷贝合并的首选写法）。
  void AppendFrom(Buffer* other);

  // 确保可写区至少有 n 字节，不足则扩容。一般不需要手动调用。
  // 扩容顺序：① 先把可读数据前移（Compact）复用已分配空间；② 仍不够则容量倍增到
  // max(2*capacity, read+write+n)；③ capacity()==0 的特殊情况以 kDefaultInitialCapacity 为下限。
  void EnsureWritable(std::size_t n);

  // ---- 读侧 ----

  // 返回可读区起始指针，有效长度为 ReadableBytes()。
  // **ReadableBytes()==0 时返回 nullptr**（底层 vector 在 capacity==0 时 data() 为 nullptr）。
  // 因此 `memcpy(dst, Peek(), 0)` 严格说是 UB：调用方必须先判长度，再解引用。
  const char* Peek() const noexcept;

  // 当前可读字节数。
  std::size_t ReadableBytes() const noexcept;

  // 可读区的 string_view 视图（零拷贝）。生命周期绑定到本 buffer 或下一次写操作。
  std::string_view PeekView() const noexcept;

  // 消费 n 字节。n 超过 ReadableBytes() 时等价于 RetrieveAll()。
  // 注意：n 超过 ReadableBytes() 会**静默清空**整个 buffer（不是空操作、不报错、不夹紧）。
  //       拆包时务必先用 ReadableBytes() 比长度——否则会把粘包的后半帧一起吞掉，
  //       症状是偶发丢包 / 连接僵死，且抓包看起来"对端明明发了"。
  void Retrieve(std::size_t n) noexcept;

  // 丢弃全部可读数据（read_idx = write_idx = 0）。
  void RetrieveAll() noexcept;

  // 拷贝读出 n 字节到 out。
  // 返回：true 成功；false 表示可读字节不足（此时 buffer 不被修改）。
  bool ReadBytes(void* out, std::size_t n);

  // 拷贝读出 n 字节到 out 字符串（覆盖写）。
  bool ReadString(std::string* out, std::size_t n);

  // 取出全部可读数据为 string 并清空 buffer。
  std::string RetrieveAllAsString();

  // 跳过 n 字节（等价 Retrieve），语义更贴近"跳过帧头"。
  void Skip(std::size_t n) noexcept { Retrieve(n); }

  // ---- 容量 ----

  // 当前已分配的容量（不等于可读+可写，包含 compaction 前的空闲前缀区）。
  std::size_t capacity() const noexcept;

  // 保证总容量至少为 new_cap。
  void Reserve(std::size_t new_cap);

  // 把容量收缩到恰好容纳可读数据，并把读下标归零。
  void ShrinkToFit();

  // 调试用：返回 "r=12 w=48 cap=1024" 这样的状态串。
  std::string DebugStateString() const;

 private:
  // 把可读数据整体前移到起始位置，为写侧腾出空间。
  void Compact() noexcept;

  std::vector<char> data_;
  std::size_t read_idx_{0};
  std::size_t write_idx_{0};
};

}  // namespace qzrpc
