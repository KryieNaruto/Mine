// F-VX 魔法叠叠乐 —— 命令流录制 / 回放 / 复现（设计 §13.3，计划阶段 7）
//
// 三条纪律（与 block_brush.h 顶部同源，这里再加一条本模块专属的）：
//
// 1. **录制态是唯一真源**。`EventRecord` 数组按下标顺序即语义顺序，
//    回放/复现都只读这个数组，不读任何容器遍历序。
// 2. **stroke_id 按记录值原样重放（M10）**。
//    回放时**不得**重新分配 stroke_id：I2 要求"单调不复用"，若回放走
//    `next_stroke_id()` 重新编号，回放态与记录态的 id 数值就会分叉，
//    `hash` 必然不等（`hash` 含 stroke_id，见 §5.2）。所以每笔的 id
//    在录制时就钉死，回放时用 `add_stroke(..., stroke_id)` 的显式 id 重放。
// 3. **三态必须逐字节相等**：记录态 / 回放态 / 复现态得到同样的
//    `IBlockSet::hash` 与同样的图像。少比任何一项，就有一条"看起来能用"
//    的路径在掩盖非确定性。
//
// 本模块**不碰设备**：它只处理逻辑层（Cell / IBlockSet / 颜色），
// 像素比对由调用方用 stage 3 的 renderer 现场跑两张图来完成。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "magicblocks/features/stack/block_brush.h"
#include "magicblocks/features/stack/block_set.h"
#include "magicblocks/features/stack/stack_types.h"

namespace magicblocks::features::stack {

// ---- 事件种类 ----
// 只允许"记录态能唯一重放"的操作进入录制流。查询类操作（find / size /
// hash）不录制 —— 它们不改变真源，录进去只会让回放多做一些无副作用的事。
enum class EventKind : std::uint8_t {
  Stroke = 0,     // 一次落笔：路径点 + 颜色 + 锁定层
  Undo = 1,       // 撤回指定 stroke_id
  SetColor = 2,   // 改单个方块的颜色
  LockLayer = 3,  // 锁层
  UnlockLayer = 4,
};

const char* event_kind_name(EventKind k);

// 一个路径点。**用整数像素**而不是浮点：录制态要能被逐字节比较（M10 的
// 前提），浮点在序列化与重放之间会引入舍入差异。
struct PathPoint {
  std::int32_t px = 0;
  std::int32_t py = 0;

  bool operator==(const PathPoint& o) const { return px == o.px && py == o.py; }
};

// 一条事件记录。
//
// 用**单结构 + 联合语义**而不是多态基类：录制流要按字节可比、可存盘、
// 可 diff。多态会引入 vtable 指针（地址不可复现），存盘后也无法比对。
struct EventRecord {
  EventKind kind = EventKind::Stroke;
  // Stroke / Undo 用：这一笔的 id。Stroke 时是**录制时分配**的值，
  // 回放时必须原样使用（M10）。
  std::uint32_t stroke_id = 0;
  // Stroke 用：路径点
  std::vector<PathPoint> path;
  // Stroke / SetColor 用：颜色 0xRRGGBBAA
  std::uint32_t color = 0;
  // Stroke 用：该笔落在哪一层。`kNoLayer` 表示"由射线决定"（不锁层）。
  std::int32_t layer = kNoLayer;
  // SetColor 用：改哪个格
  Cell cell{};

  static constexpr std::int32_t kNoLayer = -1;
};

// 一条命令流 = 按时间顺序的事件数组。
//
// 用 `std::vector` 而不是 `std::map`/`unordered_map`：顺序来自下标，
// 这是 §13.3 "所有顺序来自真源数组下标"的直接落实。
class ICommandStream {
 public:
  virtual ~ICommandStream() = default;

  // 追加一条事件，返回它在流里的下标。
  virtual std::size_t append(const EventRecord& e) = 0;
  virtual std::size_t size() const = 0;
  virtual const EventRecord& at(std::size_t i) const = 0;
  virtual const std::vector<EventRecord>& events() const = 0;
  virtual void clear() = 0;

  // 流本身的哈希（只覆盖事件内容，不含任何地址/时间）。
  // 用途：录制态与回放态"读回来的流"应当同哈希 —— 这能抓到
  // "序列化丢字段"这类问题，而 IBlockSet 的 hash 抓不到。
  virtual std::uint64_t hash() const = 0;

  // 序列化为文本（每行一条事件，字段固定顺序）。**不是**给人看的日志，
  // 是给 `parse` 精确读回的结构化格式：往返必须无损（round-trip 有测试）。
  virtual std::string serialize() const = 0;
  // 从 `serialize()` 的产物解析。失败返回 false 并在 detail 里说明原因。
  virtual bool parse(const std::string& text, std::string* detail) = 0;
};

std::unique_ptr<ICommandStream> create_command_stream();

// ---- 录制器 ----
//
// 包一个 IBlockSet：所有改变真源的操作都**经过录制器**，它先记事件、
// 再改真源。这样"录制态"不可能与真源漂移。
class IRecorder {
 public:
  virtual ~IRecorder() = default;

  // 直接包一个 IBlockSet（真源由调用方持有）。
  virtual StackStatus attach(IBlockSet* set, const GridSpec& grid) = 0;
  virtual IBlockSet* set() const = 0;
  virtual ICommandStream& stream() = 0;
  virtual const ICommandStream& stream() const = 0;

  // 落笔：给定平面路径点 + 颜色 + 层，走完整笔刷链路后写入真源。
  // stroke_id **由录制器分配**（真源下一个可用 id），并记进事件。
  // `layer` 为负数时不锁层（由射线决定落点层）。
  virtual StackResult<std::uint32_t> stroke(const std::vector<PathPoint>& path,
                                            std::uint32_t rgba,
                                            std::int32_t layer) = 0;

  // 整笔撤销（撤回最近一笔）。
  virtual StackStatus undo_last() = 0;
  // 撤销指定笔。
  virtual StackStatus undo(std::uint32_t stroke_id) = 0;

  // 改色。
  virtual StackStatus set_color(const Cell& cell, std::uint32_t rgba) = 0;

  // 锁层 / 解锁。
  virtual StackStatus lock_layer(std::int32_t layer) = 0;
  virtual StackStatus unlock_layer() = 0;

  // 当前的层锁值（未锁返回 -1）。
  virtual std::int32_t locked_layer() const = 0;
};

std::unique_ptr<IRecorder> create_recorder();

// ---- 回放 ----
//
// 把一个 `ICommandStream` 原样重放到一个**空的** IBlockSet 上。
//
// **关键（M10）**：这里绝不调用 `next_stroke_id()` 去重新编号。
// 每条 Stroke 事件都把自己的 `stroke_id` 原值透传给 `add_stroke`。
// 于是回放态每笔的 id 与记录态逐值相等 —— 这正是
// `StackCommandMode.ReplayPreservesStrokeIdValues` 要钉住的点。

struct ReplayStats {
  std::size_t strokes = 0;      // 重放了笔事件
  std::size_t undos = 0;        // 重放了撤销
  std::size_t color_sets = 0;   // 重放了改色
  std::size_t locks = 0;        // 重放了锁层/解锁
  std::uint32_t blocks = 0;     // 最终真源方块数
  std::uint64_t result_hash = 0;  // 最终 IBlockSet::hash
};

StackResult<ReplayStats> replay(const ICommandStream& stream, IBlockSet& set,
                                const GridSpec& grid);

// ---- 复现 ----
//
// 复现 = "再录一遍同样的输入"。它不是"回放同一份流"：
// 回放验证的是**流 → 真源**这一段;复现验证的是**输入 → 流 → 真源**
// 整条链路在两个独立实例上是否给出同样结果。
// 两者都过，才排除"流被污染"和"生成逻辑不确定"两类问题。
//
// 具体做法：用 `build` 在两个**全新的** recorder 上各跑一遍同样的路径输入，
// 比较两条流的 hash 与两个真源的 hash。

// 一段路径输入（纯数据，不含 id / 颜色 —— 那些由 build 确定性地给）。
struct StrokeScript {
  std::vector<PathPoint> path;
  std::uint32_t rgba = 0;
  std::int32_t layer = -1;
};

struct BuildResult {
  std::uint64_t stream_hash = 0;
  std::uint64_t set_hash = 0;
  std::uint32_t blocks = 0;
  std::uint32_t stroke_count = 0;
};

// 在一个全新的 recorder 上执行整段脚本。每次调用都从零开始，
// 所以两次调用的结果相等 ⇔ 整条链路确定性。
StackResult<BuildResult> build(const std::vector<StrokeScript>& script,
                               const GridSpec& grid);

// ---- 三态校验 ----
//
// 一次性跑完"记录 / 回放 / 复现"并逐项比对。
// 任何一项不等都**直接失败**，不降级、不放宽（D8）。
struct CommandCheck {
  bool stream_hash_equal = false;   // 录制流 vs 回放后重新序列化的流
  bool set_hash_equal = false;      // 录制真源 vs 回放真源
  bool reproduce_equal = false;     // 两次独立 build 的流哈希与真源哈希
  bool blocks_equal = false;        // 方块数一致
  std::uint64_t recorded_set_hash = 0;
  std::uint64_t replayed_set_hash = 0;
  std::uint64_t reproduced_set_hash = 0;
  std::uint64_t recorded_stream_hash = 0;
  std::uint64_t reproduced_stream_hash = 0;

  // 三态是否全部一致。这是唯一可以用来说"三态通过"的判据。
  bool all_equal() const {
    return stream_hash_equal && set_hash_equal && reproduce_equal && blocks_equal;
  }
};

// 跑三态校验。`grid` 给出网格；`script` 是输入。
StackResult<CommandCheck> check_three_states(const std::vector<StrokeScript>& script,
                                             const GridSpec& grid);

}  // namespace magicblocks::features::stack
