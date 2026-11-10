// MagicBlocks L4 —— 魔法叠叠乐：值类型与错误码（F-VX 基座）
//
// ============================ 层归属 ============================
// 这是 **L4 玩法层**。整个 MagicBlocks 里只有 features/ 可以出现玩法语义
// （"叠叠乐""积木""柱"这类词）。L0-L3 一律不得引用本文件。
// 机械门控见 tools/check_layer_purity.py（G3）与
// tools/check_l3_no_gameplay_names.py（G7，只扫 primitives/，不扫 features/）。
//
// ============================ 为什么 Cell 是 int32 而不是 float ============================
// 见设计 §4.2，三条：
//   1. 去重的唯一判据必须是**精确相等**。浮点 cell 需要 ε 比较，而 ε 比较在
//      "边界恰好落在格线上"时会两边都不等 → 产生**重叠方块**；整数比较没有这个问题。
//   2. 哈希可复现。`Block` 逐字节哈希 → 文档哈希 → golden 文件名；float 的
//      NaN / -0.0 会破坏"同样输入同样字节"。
//   3. 世界坐标是派生量：`world = origin + cell * cell_size`，由 GridSpec 单向计算，
//      永不回写。
//
// ============================ Block 为什么是 24 B ============================
// 设计 §4.1 勘误 E-VX-3：L3 设计文档 §6A 里的 32 B `Block` **并未随 L3 交付**
// （`include/magicblocks/primitives/` 下不存在 Block 类型）。所以本玩法在
// features/stack/ 内**完整定义**自己的 24 B `Block`，不复用、不依赖 L3。
// 24 B = 12 B Cell + 4 B color + 4 B stroke_id + 4 B flags，且必须 4 字节对齐
// 以保证"逐字节哈希稳定"（无隐式 padding）。
//
// ============================ 不做的事 ============================
//   - 不做自由浮点放置（Q1 裁定：网格吸附强制）
//   - 不做运行期体素网格（R-2 裁定：IVoxelizer 只用于一次性 OBJ 预处理）
//   - 不引入 unordered_map 遍历序做输出顺序（确定性铁律）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace magicblocks::features::stack {

// 整数格坐标。单位 = GridSpec::cell_size 的倍数。
//
// `y` 是层号（向上为正），其余两轴在水平面上。别名字段名刻意与设计 §4.1 逐字一致。
struct Cell {
  std::int32_t x = 0;
  std::int32_t y = 0;  // 层号(向上为正)
  std::int32_t z = 0;

  friend bool operator==(const Cell& a, const Cell& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
  }
  friend bool operator!=(const Cell& a, const Cell& b) { return !(a == b); }

  // 确定性全序（**不是**哈希序）：先 y、再 z、再 x。
  //
  // D-VX-5 契约：内面 pass 的实例必须按 (y, z, x) 升序写入实例缓冲。
  // 这个 `less` 就是该契约的唯一实现，别处不得另写一套比较。
  friend bool operator<(const Cell& a, const Cell& b) {
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.x < b.x;
  }
};

// 真源里的一块方块。
//
// 24 B 无 padding 是**契约**（逐字节哈希 + 实例派生都依赖它），用 static_assert 钉死。
struct Block {
  Cell cell;                 // 12 B
  std::uint32_t color;       // 8bit RGBA，直接喂 InstanceAttribs::base_color
  std::uint32_t stroke_id;   // 哪一笔画的(整笔撤销的依据)
  std::uint32_t flags;       // 预留:bit0 = 内面高亮,其余必须为 0
};
static_assert(sizeof(Block) == 24, "Block 必须 24B(12B Cell + 4B color + 4B stroke_id + 4B flags)");
static_assert(alignof(Block) == 4, "Block 必须 4 字节对齐,保证逐字节哈希稳定");
static_assert(sizeof(Cell) == 12, "Cell 必须 12B(3 x int32)");

// `Block.flags` 的已定义位。I3：未定义位必须为 0。
inline constexpr std::uint32_t kFlagInnerHighlight = 0x1u;
inline constexpr std::uint32_t kFlagDefinedMask = kFlagInnerHighlight;

// `stroke_id` 的保留值。0 表示"无效/未归属"，真实笔画从 1 起（I2：单调递增、永不复用）。
inline constexpr std::uint32_t kInvalidStrokeId = 0u;

// 网格规格（设计 §5.1）。
//
// 全部字段都是**构造期常量**：本玩法不允许运行期改 `cell_size` —— 改了会让已存
// `Block` 的物理位置整体漂移，而 `Block` 里不该存浮点。
struct GridSpec {
  float origin[3] = {0.0f, 0.0f, 0.0f};  // 网格原点(世界坐标)
  float cell_size = 0.5f;                // 一格边长(米)
  std::int32_t layers = 8;               // 层数 y ∈ [0, layers)
  std::int32_t half_extent = 16;         // x/z ∈ [-half_extent, half_extent)
};

// 错误码全集（设计 §6.6）。顺序即编号，不要重排：CLI 打印 [code=N]。
enum class StackCode : std::int32_t {
  Ok = 0,
  RayParallelToLayer,  // 射线平行层平面
  RayMissesLayer,      // 交点在相机背后
  OutOfGrid,           // 吸附后越出 half_extent / layers
  LayerOutOfRange,     // lock(layer) 越界
  LockLayerRequired,   // 锁层时未给 layer
  UnknownStroke,       // 撤销不存在的 stroke
  UnknownTemplate,     // 模板名未知
  TemplateOutOfGrid,   // 模板偏移把方块推到网格外
  EmptyStroke,         // 笔迹没有任何有效格
  ColorOutOfRange,     // 颜色模式参数越界
};

// 人类可读的错误码名（诊断/日志用，不参与哈希）。
const char* stack_code_name(StackCode code);

// 一次失败。带上 code + 人话 detail（设计 §6.6：所有错误都带 detail）。
//
// 刻意**不用** device::Status：那个的 StatusCode 是 I/O 级抽象（InvalidArgument /
// Unsupported ...），表达不了 OutOfGrid / UnknownStroke 这类玩法语义，
// 而 CLI 要求逐条打印 `[code=N] <detail>`。用独立类型把编号与语义钉死。
struct StackError {
  StackCode code = StackCode::Ok;
  std::string detail;

  bool ok() const { return code == StackCode::Ok; }
  explicit operator bool() const { return ok(); }
  // 具名访问器（不叫 code()/message()：`code` 是字段名，C++ 下同名字段与成员函数
  // 会互相遮蔽，`e.code()` 会编译成"对字段调用"，报一个极难读的错误）。
  StackCode error_code() const { return code; }
  const std::string& text() const { return detail; }

  static StackError Ok_() { return {}; }
  static StackError Fail(StackCode c, std::string d) { return {c, std::move(d)}; }
};

// `Result<T>`：要么值，要么 StackError。取 value() 前必须先看 ok()。
template <typename T>
class StackResult {
 public:
  StackResult(T value) : value_(std::move(value)) {}                       // NOLINT
  StackResult(StackError err) : error_(std::move(err)) {}                  // NOLINT

  bool ok() const { return error_.ok(); }
  explicit operator bool() const { return ok(); }

  const StackError& error() const { return error_; }
  StackCode error_code() const { return error_.code; }
  const std::string& text() const { return error_.detail; }

  T& value() { return value_; }
  const T& value() const { return value_; }
  T&& take() { return std::move(value_); }

 private:
  T value_{};
  StackError error_{};
};

// 无返回值的 StackResult 特化（结果只有成功/失败）。
class StackStatus {
 public:
  StackStatus() = default;
  StackStatus(StackError err) : error_(std::move(err)) {}  // NOLINT

  bool ok() const { return error_.ok(); }
  explicit operator bool() const { return ok(); }

  const StackError& error() const { return error_; }
  StackCode error_code() const { return error_.code; }
  const std::string& text() const { return error_.detail; }

  static StackStatus Ok_() { return {}; }
  static StackStatus Fail(StackCode c, std::string d) { return {StackError::Fail(c, std::move(d))}; }

 private:
  StackError error_{};
};

// ---------------------------------------------------------------- 确定性哈希

// FNV-1a 64 的累加步骤。用它而不是标准库 hash：std::hash 的实现**不保证跨
// 编译器/跨运行一致**，而这里的哈希要进 golden 文件名。
inline std::uint64_t hash_bytes(std::uint64_t h, const void* data, std::size_t n) {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(p[i]);
    h *= kPrime;
  }
  return h;
}
inline constexpr std::uint64_t kHashSeed = 1469598103934665603ull;

// 单个 Cell 的独立哈希（派生索引用，**不进**真源哈希口径）。
//
// 注意：`unordered_map` 只允许做**派生缓存**的键查找，任何输出遍历一律走
// 真源数组下标序（设计 §4.3 确定性铁律）。
struct CellHash {
  std::size_t operator()(const Cell& c) const noexcept {
    std::uint64_t h = kHashSeed;
    h = hash_bytes(h, &c.x, sizeof(c.x));
    h = hash_bytes(h, &c.y, sizeof(c.y));
    h = hash_bytes(h, &c.z, sizeof(c.z));
    return static_cast<std::size_t>(h);
  }
};

// ---------------------------------------------------------------- 实例顺序（D-VX-5）

// 内面 pass 实例缓冲的排序规则（设计 §10.6.1）。
//
// 为什么必须有这个东西：内面 pass 是 `depthTest=OFF`（§10.3），**深度测试
// 不参与**，所以"谁盖谁"完全由**绘制顺序**决定。若直接用真源插入序，那
// 顺序是**落笔顺序**而不是空间顺序 —— 同一个最终棋盘，换一种落笔先后就会
// 画出不同的内面。那不是确定性，是"确定地依赖历史"。
//
// 所以内面 pass 必须按空间序 `(y, z, x)` 升序排：完全由格子坐标决定，
// 与落笔历史无关。
enum class CellOrder : std::uint8_t {
  Index = 0,  // 真源下标序（表面 pass 用；它开深度测试，顺序无关）
  YZX = 1,    // 先 y 升序、再 z 升序、再 x 升序（内面 pass 用）
};

inline const char* cell_order_name(CellOrder o) {
  return o == CellOrder::YZX ? "YZX" : "Index";
}

// `(y, z, x)` 升序的严格弱序。**注意顺序**：与 `Cell::operator<` 的
// dy→dz→dx 一致，但这里单独写出来而不是复用 `operator<`，因为
// `operator<` 是"格子的全序"，而这里是"实例写入顺序"，两者语义不同，
// 将来任一方要变时不该互相牵连。
struct CellOrderLess {
  bool operator()(const Cell& a, const Cell& b) const noexcept {
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.x < b.x;
  }
};

}  // namespace magicblocks::features::stack
