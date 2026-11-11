// MagicBlocks L4 —— 魔法叠叠乐：IStackSession（命令 / 撤销 / 改色 / 层锁的状态机）
//
// 设计依据：VX 设计 §3.1（本文件就是那里列出的 `stack_session.h`）、§6.5（层锁状态机）、
// §9（撤销）、§11（颜色模式）、§7（落笔 → 出图整链）。
//
// ============================ 为什么要一个显式状态机 ============================
// CLI 原来的 `cmd_stack()` 是一段过程式流程：建 set → 逐笔 lock → stroke → add_stroke →
// apply_color_mode，每一步都靠"代码顺序"保证正确。这带来两个问题：
//
//  1. **顺序错了不会响**。比如"抬笔之后再 Move"、"撤销一笔之后再 commit 一笔"，
//     过程式写法只会静默做出某个结果，没有任何地方能断言"这个事件现在不该被接受"。
//  2. **撤销只活在数据层**。`IBlockSet::remove_stroke` 让方块从真源里消失，但
//     "一笔的完整生命周期"（开始 → 采样 → 抬笔 → 落块 → 被撤销 → 再重做）没有任何
//     单一对象持有。假绿就是这么来的：数据层用例全绿，"撤销后画面回退"从没验过。
//
// 本状态机把"一笔"提升为**会话级**的对象，每个状态转移都可断言：
//
//        idle ──begin_stroke──▶ stroking ──move_sample────────┐(自环)
//         ▲                         │                         │
//         │                         │ end_stroke               │
//         │                         ▼                         │
//         └──── undo_stroke / undo_last / apply_color_mode ── idle
//                                   │  ▲
//                                   │  │
//                                   ▼  │
//                               stroke_failed (整笔失败，缺省 rollback)
//
//   · `begin_stroke`（落笔 / Down）     : idle        → stroking
//   · `move_sample`（移动 / Move）      : stroking    → stroking（自环，追加采样）
//   · `end_stroke`（抬笔 / Up）         : stroking    → idle（落块）或 stroke_failed
//   · `commit_failed_stroke`（补救）    : stroke_failed → idle
//   · `rollback_failed_stroke`（丢弃）  : stroke_failed → idle
//   · `undo_stroke` / `undo_last`       : idle        → idle（数据层 + 生命周期一起回退）
//   · `redo_last`（重做，I2 换新 id）   : idle        → idle
//   · `lock_layer` / `unlock_layer`     : idle        → idle（层锁子状态：unlocked ⇄ locked）
//   · `set_stroke_color` / `apply_color_mode` : idle  → idle
//
// **非法转移一律返回错误**（`StackCode::InvalidTransition`，带"当前状态 / 拒绝的事件 / 该怎么走"
// 的人话 detail），不做"忽略这一下"、不做"自动补一个抬笔"。理由与 D-VX-9 同源：
// 静默吞掉一个事件，等于让调用方误以为它生效了。
//
// ============================ 撤销是会话级、可重做的 ============================
// 撤销走**快照回滚**：`begin_stroke` 时把真源整表（24 B × N）拷一份，`end_stroke`
// 成功落块后丢弃这份快照。撤销 = 把快照写回一个新的真源，于是
//
//   · 撤销后的真源与"那一笔从未落过"**逐字节相同**（hash 相等）—— 这是像素回退的前提；
//   · 会话自己持有"撤销栈"，`undo_last` 不再依赖调用方记得上一个 stroke_id；
//   · `redo_last` 用**新分配的** stroke_id 重放被撤那一笔（I2：id 单调不复用）。
//
// 为什么不用"再画一遍反操作"：反操作要求把整笔的每一格都记下来并精确逆序删除，
// 而快照法在 O(N) 拷一次之外不需要任何额外簿记，且不可能漏格。代价是每次落笔
// 多一份真源拷贝 —— 10k 块 = 240 KB，一次性，不进任何逐帧热路径（设计 §7 关键判据）。
//
// ============================ 落笔 = 一个确定的格集合 ============================
// 本会话接受的是**已经算好的整数格集合**（`StrokeRequest::cells`），不是射线。
// 射线 → 格 这一步属于 `IBlockBrush`（§6），它要相机、要层锁、要 DDA；把两者混在
// 一个类型里会让状态机必须持有相机，导致"状态"不再可快照、不可比较。
// 网格合法性、y 域、越界仍然在这里**再查一遍**（不许"上游查过了"当成不查的理由）。
//
// 本文件不碰设备、不碰 Vulkan、不读时钟（G3 / G4 / G8 门控）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/features/stack/block_set.h"
#include "magicblocks/features/stack/color_mode.h"
#include "magicblocks/features/stack/stack_types.h"

namespace magicblocks::features::stack {

// ---------------------------------------------------------------- 状态

enum class SessionState : std::uint8_t {
  Idle = 0,         // 没有进行中的落笔
  Stroking = 1,     // 已落笔（Down），正在采样
  StrokeFailed = 2, // 抬笔失败，整笔未落成：必须显式补救或回滚
};

const char* session_state_name(SessionState s);

// 事件全集。名字即"状态转移"，`session_event_name` 用于错误 detail 与快照哈希。
enum class SessionEvent : std::uint8_t {
  BeginStroke = 0,
  MoveSample = 1,
  EndStroke = 2,
  CommitFailedStroke = 3,
  RollbackFailedStroke = 4,
  UndoStroke = 5,
  UndoLast = 6,
  RedoLast = 7,
  LockLayer = 8,
  UnlockLayer = 9,
  SetStrokeColor = 10,
  ApplyColorMode = 11,
};

const char* session_event_name(SessionEvent e);

// 状态转移表：`from` 状态收到事件 `e` 是否合法。纯函数，可脱离任何实例断言。
bool session_transition_allowed(SessionState from, SessionEvent e);

// ---------------------------------------------------------------- 输入

// 一次落笔请求：**已经吸附好的整数格**（可由 IBlockBrush 产出）+ 颜色。
//
// `cells` 允许重复（同一笔自我交叉），落块时按首次出现的顺序去重：
// 去重是整数格相等（§6.4），不依赖任何哈希容器遍历序。
struct StrokeRequest {
  std::vector<Cell> cells;
  std::uint32_t rgba = 0xFFFFFFFFu;
  // 该笔落在第几层（用于会话快照与旁证；**不**在这里改格子的 y）。
  // 负数 = 未指定（由上游决定）。层锁生效时调用方应传锁定层。
  std::int32_t layer = -1;
};

struct StrokeResult {
  std::uint32_t stroke_id = kInvalidStrokeId;
  std::uint32_t cells_requested = 0;  // 请求里的格数（含重复）
  std::uint32_t cells_added = 0;      // 真源实际新增的格数（去重 + 撞击占格后）
  std::uint32_t skipped_occupied = 0; // 因格已被占用而丢掉的格数
};

// ---------------------------------------------------------------- 撤销 / 重做 / 改色

// 撤销栈里的一笔。
struct UndoEntry {
  std::uint32_t stroke_id = kInvalidStrokeId;
  std::int32_t layer = -1;
  std::vector<Block> blocks; // 按真源下标序（重做时按这个顺序重放）
};

struct RedoResult {
  std::uint32_t stroke_id = kInvalidStrokeId; // 新分配的 id（I2：永不复用）
  std::uint32_t cells_added = 0;
};

struct SessionColorResult {
  std::uint32_t blocks_changed = 0;
  std::size_t undo_depth = 0; // 改色会清空撤销栈（改色之后撤销"整笔"语义已不成立）
};

// ---------------------------------------------------------------- 快照

// 会话的可比较快照。
//
// 刻意**不含**真源本身（真源由 `IBlockSet::hash()` 单独比较），只含"状态机的状态"：
// 两次同事件序列必须得到同一份快照 —— 这是"状态可快照、可比较"的可断言形式。
struct SessionSnapshot {
  SessionState state = SessionState::Idle;
  std::int32_t locked_layer = -1;
  std::uint64_t stroke_gen = 0;       // 已开始的落笔次数（单调）
  std::uint64_t committed_strokes = 0; // 已成功落块的笔数（单调）
  std::uint32_t pending_samples = 0;   // 当前落笔已收到的采样数
  std::uint32_t last_stroke_id = kInvalidStrokeId;
  std::size_t undo_depth = 0;
  std::size_t redo_depth = 0;
  std::vector<std::uint32_t> undo_ids; // 按栈序（底 → 顶）
  std::uint64_t hash = 0;              // 覆盖上述全部字段的 FNV-1a 64

  bool operator==(const SessionSnapshot& o) const { return hash == o.hash; }
  bool operator!=(const SessionSnapshot& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------- 接口

class IStackSession {
 public:
  virtual ~IStackSession() = default;

  virtual GridSpec grid() const = 0;
  // 真源只读访问（渲染/哈希/统计用）。会话拥有它。
  virtual const IBlockSet& set() const = 0;
  virtual IBlockSet& mutable_set() = 0;

  virtual SessionState state() const = 0;
  virtual SessionSnapshot snapshot() const = 0;
  // 状态机的可读转写（`idle|locked=2|gen=3|...`），用于日志与"同序不同态"的定位。
  virtual std::string describe() const = 0;

  // ---- 落笔三段式 ----
  // 落笔（Down）。`req.rgba` 即本笔颜色；`req.cells` 此时可以为空（后续靠 move_sample 追加）。
  virtual StackStatus begin_stroke(const StrokeRequest& req) = 0;
  // 移动（Move）：把新吸附出来的格追加进当前落笔。
  virtual StackStatus move_sample(const std::vector<Cell>& cells) = 0;
  // 抬笔（Up）：去重 → 逐格入真源 → 清撤销栈/重做栈。
  // 任一格越界/非法 → **整笔回滚**（真源回到 begin_stroke 前），状态进 StrokeFailed。
  virtual StackResult<StrokeResult> end_stroke() = 0;
  // 补救：把失败的整笔当从未发生（真源已是 begin 前的样子），回 idle。
  virtual StackStatus rollback_failed_stroke() = 0;
  // 补救：改正后重签这笔（同一次落笔的格集合被替换），再走一遍落块。
  virtual StackResult<StrokeResult> commit_failed_stroke(const std::vector<Cell>& cells) = 0;

  // ---- 撤销 / 重做（会话级）----
  // 撤销指定笔。不在撤销栈里 → UnknownStroke。成功时真源回到该笔落笔前。
  virtual StackStatus undo_stroke(std::uint32_t stroke_id) = 0;
  virtual StackStatus undo_last() = 0;
  virtual StackResult<RedoResult> redo_last() = 0;
  virtual std::size_t undo_depth() const = 0;
  virtual std::size_t redo_depth() const = 0;
  virtual const std::vector<UndoEntry>& undo_stack() const = 0;

  // ---- 改色 ----
  // 改**某一笔**的颜色（只改 color 字节；几何/实例顺序不动）。不在真源里 → UnknownStroke。
  virtual StackStatus set_stroke_color(std::uint32_t stroke_id, std::uint32_t rgba) = 0;
  // 按模式整堆改色（§11）。失败（空色板 / Flat 非 1 项）→ 真源一个字节都不动。
  virtual StackResult<SessionColorResult> apply_color_mode(ColorMode mode,
                                                           const ColorPalette& palette) = 0;

  // ---- 层锁（§6.5 子状态机）----
  virtual StackStatus lock_layer(std::int32_t layer) = 0;
  virtual StackStatus unlock_layer() = 0;
  virtual bool layer_locked() const = 0;
  // 未锁 = -1（不是 0！0 是"锁在第 0 层"）。
  virtual std::int32_t locked_layer() const = 0;
};

// 构造。`grid` 非法 → 显式错误（与 create_block_set 同一口径，不返回"夹过参数"的会话）。
std::unique_ptr<IStackSession> create_stack_session(const GridSpec& grid);

}  // namespace magicblocks::features::stack
