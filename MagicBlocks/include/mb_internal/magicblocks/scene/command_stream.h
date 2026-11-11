// MagicBlocks SDK —— L2 S4/S6/S7 `CommandStream`:帧命令流 + 二进制/JSON 序列化 + 稳定哈希
//
// 设计约束(spec §4 L2 S4/S6/S7、§5 命令模式、§6.3 / plans-l2 §4.5 / §4.6.1):
//   - **恰好 11 类命令**,与 L1 `ICommandList` 的 11 个方法一一对应
//     (op 编号沿用 `backend/vulkan/vulkan_command.h:47-59` 的发号,那是 spec 的口径)。
//   - **命令流自包含(B7)**:一条流里既有命令,也有 `frame_index`、输入事件时间戳、
//     以及**完整的场景快照**。因此"从流复现"不需要外部上下文。
//   - **规范化字节序列**:所有字段逐项写小端,`float` 先 `bit_cast` 成 `uint32_t`;
//     **禁止整结构体 memcpy**。这保证跨编译器/跨进程的哈希一致。
//   - **FNV-1a 64 位哈希**(spec §6.3):对前面全部字节做链式哈希,写完再落一个
//     `stream_hash` 尾字段。读入端必须复算比对。
//   - **`commands[]` 含 begin_pass/end_pass(W4)**:于是有强不变量
//     `stream.commands.size() == target->command_count()`。
//   - **`pass_area` 存原始调用值(U2)**:`area_width == 0` 表示整图,L2 **不**把它
//     解析成 extent 宽度(L1 会解析,那是 L1 的入流行为,不代表 L2 的口径)。
//   - 本头**零 Vulkan 类型**(G3)、**不读时钟**(D13 门控)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/device/types.h"
#include "mb_internal/magicblocks/scene/types.h"

namespace magicblocks::scene {

using device::Result;
using device::Status;

// 命令操作码。**取值必须与 L1 backend 的发号一致**(spec R3 的 11 类命令):
//   0 BindPipeline / 1 BindVertexBuffer / 2 BindIndexBuffer / 3 Draw / 4 DrawIndexed /
//   5 DrawInstanced / 6 PushConstants / 7 SetViewport / 8 SetScissor / 9 BeginPass / 10 EndPass
enum class CommandOp : std::uint8_t {
  BindPipeline = 0,
  BindVertexBuffer = 1,
  BindIndexBuffer = 2,
  Draw = 3,
  DrawIndexed = 4,
  DrawInstanced = 5,
  PushConstants = 6,
  SetViewport = 7,
  SetScissor = 8,
  BeginPass = 9,
  EndPass = 10,
};

inline constexpr std::uint32_t kCommandOpCount = 11;  // 恰好 11 类,不多不少

const char* command_op_name(CommandOp op);

// 一条命令。`a/b/c/i/f[]/pass_area/width/height` 是**按 op 复用**的通用槽位
// (与 L1 的 RecordedCommand 同构),各 op 的字段含义见 `docs/ARCHITECTURE.md` L2 段。
// 未用的槽位必须保持默认值 0 —— 否则哈希会随"垃圾值"漂移。
struct CommandRecord {
  CommandOp op = CommandOp::BindPipeline;
  std::uint64_t packed_resource = 0;   // ResourceId::pack() 的结果;0 = 不适用
  std::uint64_t pipeline_hash = 0;     // BindPipeline 用(desc 派生,跨进程稳定)
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::uint32_t c = 0;
  std::int32_t i = 0;
  float f[7] = {};
  std::uint32_t pass_area[4] = {};     // 原始 area(0 = 整图,U2)
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> payload;   // push_constants 的字节

  bool operator==(const CommandRecord& o) const;
  bool operator!=(const CommandRecord& o) const { return !(*this == o); }
};

// 一个 pass 的记录(供 dump/JSON 与人读排查)。字段与 PassDesc 对应。
struct PassRecord {
  std::string name;
  std::uint8_t layer = 0;
  std::uint8_t channel = 0;
  bool clear_color = true;
  float clear_color_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool clear_depth = true;
  float clear_depth_value = 1.0f;
  std::uint32_t area[4] = {};          // 原始调用值(0 = 整图,U2)
  Format color_format = Format::Unknown;
  bool has_depth = false;

  bool operator==(const PassRecord& o) const;
  bool operator!=(const PassRecord& o) const { return !(*this == o); }
};

// 输入事件的时间戳记录。**时间来自流,不来自时钟**(S8/D13)。
enum class EventKind : std::uint8_t {
  PointerDown = 0,
  PointerUp = 1,
  PointerMove = 2,
  PointerCancel = 3,
};

struct EventRecord {
  EventKind kind = EventKind::PointerMove;
  std::uint32_t pointer_id = 0;
  float x = 0.0f;
  float y = 0.0f;
  float dx = 0.0f;
  float dy = 0.0f;
  std::int32_t button = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int64_t timestamp_us = 0;   // 由调用方给;L2 绝不读时钟
  bool handled = false;
};

// 一帧的完整命令流(S4)。
struct CommandStream {
  std::string backend;              // "vulkan"(设备名无关,跨机稳定)
  std::uint8_t engine_major = 1;
  std::uint64_t frame_index = 0;
  Extent2D extent{};
  std::vector<EventRecord> events;
  SceneSnapshot scene;              // 完整初始场景(B7)—— 自包含的关键
  std::vector<PassRecord> passes;
  std::vector<CommandRecord> commands;   // 含 begin_pass/end_pass(W4)
  std::uint64_t stream_hash = 0;         // encode 时算出;decode 时校验

  bool operator==(const CommandStream& o) const;
  bool operator!=(const CommandStream& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------- 二进制(S6)

inline constexpr char kBinaryMagic[4] = {'M', 'B', 'L', '2'};
// v2:body 开头加入 backend 字符串,让 backend 名字进哈希(v1 只有头部 1 字节枚举,
//     `"vulkan"` 与 `"vulkan2"` 哈希会碰撞)。布局变了就必须升版本,否则 v1 文件
//     会被当成 v2 误读。
inline constexpr std::uint16_t kBinaryFormatVersion = 2;
inline constexpr std::size_t kBinaryHeaderBytes = 12;  // magic4 + version2 + rsv2 + backend1 + major1 + rsv2

// 编码成规范化小端字节序列。成功时同时把 `stream.stream_hash` 填好
// (对 hash 字段之前的全部字节取 FNV-1a 64)。
Result<std::vector<std::uint8_t>> encode_binary(CommandStream& stream);

// 从字节序列解码。任何不一致(magic / 版本 / 截断 / 尾部哈希不符)→ Invalid,
// **不返回半截流**。
Result<CommandStream> decode_binary(const std::vector<std::uint8_t>& bytes);

// ---------------------------------------------------------------- JSON(B8)

// 稳定的 JSON 文本。键序固定为:
//   format_version, engine_version, backend, frame_index, extent, input_events,
//   scene_snapshot, passes, commands, hash
// 浮点用 %.9g。连续两次调用必须**字节完全相同**(Stream.JsonDumpIsStable)。
std::string to_json(const CommandStream& stream);

// 从 JSON 解回。键序与空白不敏感,但**语义**必须与 to_json 对称:
// `from_json(to_json(s))` 的哈希 == s 的哈希(Stream.JsonRoundTripMatchesBinary)。
Result<CommandStream> from_json(const std::string& text);

// ---------------------------------------------------------------- 哈希(S7)

// FNV-1a 64 位。对**已规范化的字节**做链式哈希,不是对结构体做 hash。
inline constexpr std::uint64_t kFnv1aOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnv1aPrime = 0x100000001b3ull;

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size,
                      std::uint64_t seed = kFnv1aOffsetBasis);

// 对一整条流求哈希(= encode 出来的尾部 hash 值,不修改入参)。
std::uint64_t compute_stream_hash(const CommandStream& stream);

}  // namespace magicblocks::scene
