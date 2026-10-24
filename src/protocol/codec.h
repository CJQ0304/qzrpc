// 轻舟 RPC (Qingzhou RPC)
// src/protocol/codec.h
//
// 职责：字节流 <-> Frame 的转换，是"粘包/拆包"的唯一实现处。
//
// 粘包拆包策略：**长度前缀 + 隐式状态机**。
//   * 每个帧头里都有 body_len，所以不需要显式状态机：每次解码只看
//     "可读字节是否 >= 20"、"可读字节是否 >= 20 + body_len" 两个条件；
//   * 状态天然保存在 Connection 的 read Buffer 里（未消费的字节留到下次），
//     不做 extra 的 per-connection 状态机对象 —— 这是刻意的简化，避免状态机与
//     缓冲生命周期不一致导致的 bug；
//   * 半包：Decode 返回 kNeedMore，调用方直接返回等下一次可读事件；
//   * 粘包：调用方在循环里反复 Decode 直到 kNeedMore 或连接关闭。
//
// 恶意包防护：
//   * 魔数/版本/flags 不合法 → kError，立即断开（不尝试重同步，避免被用作探测工具）；
//   * body_len > max_frame_size → kError，立即断开（防止 4GB 长度造成预分配 OOM）；
//   * body_len < 4 + metadata_len（metadata 长度自相矛盾）→ kError；
//   * 所有长度都先与 Buffer::ReadableBytes() 比较再 memcpy，无越界可能。
//
// 线程安全：全部为无状态静态函数。Frame 与 Buffer 参数由调用方保证独占访问。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/buffer.h"
#include "common/noncopyable.h"
#include "protocol/frame.h"

namespace qzrpc {
namespace protocol {

// 帧编解码器（纯静态）。
class Codec : private StaticOnly {
 public:
  // 解码结果。
  enum class Result {
    kOk = 0,      // 成功解出一帧（已从 input 消费掉该帧的全部字节）
    kNeedMore = 1,  // 半包，input 未做任何消费，等更多数据
    kError = 2,   // 协议错误，连接必须断开
  };

  // 从 input 尝试解出一帧。
  // 参数：input - 读缓冲（会被消费）；frame - 输出帧（成功时被完全覆盖）；
  //       max_frame_size - body 上限，传 0 表示用 kDefaultMaxFrameSize；
  //       err - 可选，失败原因。
  // 返回：见 Result。
  static Result Decode(Buffer* input, Frame* frame, std::uint32_t max_frame_size = 0,
                       std::string* err = nullptr);

  // 把 frame 编码并**追加**到 output。
  // 参数：frame - 待编码帧（其 header.body_len 会在内部被重新计算并回写）；
  //       output - 输出缓冲；max_frame_size - 校验用上限，超限返回 false。
  // 返回：true 成功。
  static bool Encode(Frame* frame, Buffer* output, std::uint32_t max_frame_size = 0);

  // 只做头部校验（Decode 内部复用，也供测试直接调用）。
  // 返回：true 表示头部合法。
  static bool ValidateHeader(const FrameHeader& header, std::uint32_t max_frame_size,
                             std::string* err);

  // 仅窥探：若头部已完整，返回该帧线上总字节数（20 + body_len）；否则返回 0。
  // 不消费 input。用于"先判断是否整帧到达再分配"的场景。
  static std::size_t PeekFrameSize(const Buffer& input, std::uint32_t max_frame_size,
                                   bool* header_invalid);
};

}  // namespace protocol
}  // namespace qzrpc
