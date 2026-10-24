// 轻舟 RPC (Qingzhou RPC)
// src/common/buffer.cpp
//
// Buffer 的实现：连续内存 + read/write 双下标。
//
// 扩容策略（对应 docs/00-file-map.md C-05）：
//   1) 可写区不足时**先** Compact()：把可读数据整体前移到下标 0，回收已消费的前缀空洞；
//   2) 仍然不足时才真正扩容，容量取 max(2 * capacity, readable + n)，
//      保证"逐字节追加"的摊还成本是 O(1)（每次扩容至少把容量翻一倍）。
//
// 为什么不用 std::vector<char> 的 reserve + 指针：
//   vector 的 size 必须等于"有效字节数"，否则无法用下标读写；所以这里把
//   vector 的 size 当作 capacity 使用，read_idx_/write_idx_ 才是逻辑上的读写位置。
//   代价是扩容时 resize() 会零初始化新分配的区域，相对 memcpy 的开销可以忽略。

#include "common/buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

namespace qzrpc {
namespace {

// 翻倍增长的天花板：2 GiB。
// 超过它之后不再翻倍，只按"实际需要"精确扩容 —— 否则一次翻倍就可能把进程内存打满。
// 帧上限是 16MB（protocol_const.h），正常路径永远碰不到这个分支。
constexpr std::size_t kMaxDoubleCapacity = static_cast<std::size_t>(1) << 31;

}  // namespace

Buffer::Buffer(std::size_t initial_capacity) {
  if (initial_capacity != 0) {
    data_.resize(initial_capacity);
  }
}

Buffer::Buffer(Buffer&& other) noexcept
    : data_(std::move(other.data_)),
      read_idx_(other.read_idx_),
      write_idx_(other.write_idx_) {
  // 被移动方的双下标归零，处于合法的"空"状态（capacity 可能非 0，这与 vector 语义一致）。
  other.read_idx_ = 0;
  other.write_idx_ = 0;
  other.data_.clear();
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    data_ = std::move(other.data_);
    read_idx_ = other.read_idx_;
    write_idx_ = other.write_idx_;
    other.read_idx_ = 0;
    other.write_idx_ = 0;
    other.data_.clear();
  }
  return *this;
}

Buffer::~Buffer() = default;

// ---------------------------------------------------------------------------
// 写侧
// ---------------------------------------------------------------------------

char* Buffer::WriteBegin() noexcept {
  return data_.empty() ? nullptr : (data_.data() + write_idx_);
}

const char* Buffer::WriteBegin() const noexcept {
  return data_.empty() ? nullptr : (data_.data() + write_idx_);
}

std::size_t Buffer::WritableBytes() const noexcept {
  return data_.size() - write_idx_;
}

void Buffer::HasWritten(std::size_t n) noexcept {
  if (n > WritableBytes()) {
    // 前置条件被违反 = 调用方写越界，属于编程错误；继续跑会破坏内存，直接终止。
    std::abort();
  }
  write_idx_ += n;
}

void Buffer::Append(const void* data, std::size_t len) {
  if (data == nullptr || len == 0) {
    return;
  }
  EnsureWritable(len);
  std::memcpy(data_.data() + write_idx_, data, len);
  write_idx_ += len;
}

void Buffer::Append(const std::string& s) {
  Append(s.data(), s.size());
}

void Buffer::AppendFrom(Buffer* other) {
  if (other == nullptr || other == this) {
    return;
  }
  const std::size_t n = other->ReadableBytes();
  if (n != 0) {
    Append(other->Peek(), n);
  }
  other->RetrieveAll();
}

void Buffer::EnsureWritable(std::size_t n) {
  if (n <= WritableBytes()) {
    return;
  }
  if (read_idx_ != 0) {
    Compact();
    if (n <= WritableBytes()) {
      return;
    }
  }
  // 走到这里 read_idx_ 必为 0，可读数据紧贴下标 0。
  const std::size_t readable = ReadableBytes();
  const std::size_t required = readable + n;
  if (required < readable) {
    // readable + n 溢出 size_t：不可能是一次合法的写请求。
    std::abort();
  }
  const std::size_t current = data_.size();
  const std::size_t doubled =
      (current > kMaxDoubleCapacity / 2) ? kMaxDoubleCapacity : current * 2;
  // current == 0 是 Buffer(0)（"按需分配"）首次写入的场景，此时 2*0 == 0，
  // 若严格按 max(2*cap, readable+n) 计算，一次 3 字节的追加只会分配 3 字节，
  // 之后是 6、12、24…… 小步增长在小包场景下会造成可观的 realloc 次数。
  // 因此首次分配至少给一个 kDefaultInitialCapacity，之后的增长仍严格翻倍。
  const std::size_t growth_floor = (current == 0) ? kDefaultInitialCapacity : 0;
  Reserve(std::max(required, std::max(doubled, growth_floor)));
}

void Buffer::Reserve(std::size_t new_cap) {
  if (new_cap <= data_.size()) {
    return;
  }
  data_.resize(new_cap);
}

// ---------------------------------------------------------------------------
// 读侧
// ---------------------------------------------------------------------------

const char* Buffer::Peek() const noexcept {
  return data_.empty() ? nullptr : (data_.data() + read_idx_);
}

std::size_t Buffer::ReadableBytes() const noexcept {
  return write_idx_ - read_idx_;
}

std::string_view Buffer::PeekView() const noexcept {
  const std::size_t n = ReadableBytes();
  if (n == 0) {
    return std::string_view();
  }
  return std::string_view(data_.data() + read_idx_, n);
}

void Buffer::Retrieve(std::size_t n) noexcept {
  // n 超过可读字节数时等价于 RetrieveAll()（头文件明确约定，不做断言）。
  if (n >= ReadableBytes()) {
    RetrieveAll();
    return;
  }
  read_idx_ += n;
}

void Buffer::RetrieveAll() noexcept {
  read_idx_ = 0;
  write_idx_ = 0;
}

bool Buffer::ReadBytes(void* out, std::size_t n) {
  if (n > ReadableBytes()) {
    return false;  // 可读不足，buffer 保持不变
  }
  if (n == 0) {
    return true;
  }
  if (out == nullptr) {
    return false;
  }
  std::memcpy(out, data_.data() + read_idx_, n);
  read_idx_ += n;
  return true;
}

bool Buffer::ReadString(std::string* out, std::size_t n) {
  if (out == nullptr || n > ReadableBytes()) {
    return false;
  }
  out->clear();
  if (n != 0) {
    out->assign(data_.data() + read_idx_, n);
  }
  read_idx_ += n;
  return true;
}

std::string Buffer::RetrieveAllAsString() {
  const std::size_t n = ReadableBytes();
  std::string result;
  if (n != 0) {
    result.assign(data_.data() + read_idx_, n);
  }
  RetrieveAll();
  return result;
}

// ---------------------------------------------------------------------------
// 容量
// ---------------------------------------------------------------------------

std::size_t Buffer::capacity() const noexcept {
  return data_.size();
}

void Buffer::ShrinkToFit() {
  Compact();
  const std::size_t readable = ReadableBytes();
  data_.resize(readable);
  data_.shrink_to_fit();
}

std::string Buffer::DebugStateString() const {
  return std::string("r=") + std::to_string(read_idx_) + " w=" + std::to_string(write_idx_) +
         " cap=" + std::to_string(data_.size());
}

void Buffer::Compact() noexcept {
  if (read_idx_ == 0) {
    return;
  }
  const std::size_t readable = ReadableBytes();
  if (readable != 0) {
    // 源区间与目标区间重叠，必须 memmove（不得 memcpy）。
    std::memmove(data_.data(), data_.data() + read_idx_, readable);
  }
  read_idx_ = 0;
  write_idx_ = readable;
}

}  // namespace qzrpc
