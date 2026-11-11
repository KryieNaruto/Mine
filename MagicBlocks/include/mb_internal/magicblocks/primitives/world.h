// MagicBlocks L3 —— `IWorld`:ECS 世界（spec §4.2 SOA / §7 / 决策 D-L3-9）
//
// 职责：**只做 ECS 存储**。运行时高频改动的实体/组件放在这里，
// 进命令流的权威快照仍然是 L2 的 `IScene`（spec §7.3 方案 B，D-L3-9）。
// 内部同时维护一份 `IScene` 镜像（方案 A）会让同一份状态有两个真源，
// 迟早漂移；而"每次都全量重建 scene"又会让 L2 的 id 单调不复用规则退化成
// 每次重播都是新 id，命令流哈希随之漂移。所以两边都不选，中间取增量同步。
//
// ============================ 存储布局（spec §4.2） ============================
//
// 实体 = {index, gen}。一组组件集合 → 一个 **archetype** → 每个组件一条**紧凑
// 列数组**。同一 archetype 内的实体，它们的每一列都分别连续存放：
//
//   Archetype{Transform, MeshRef}
//     index   : [0][1][2][3] ...      ← 行号 → EntityId.index
//     col[0]  : [T][T][T][T] ...      ← Transform 列（裸字节，按注册时的 size_bytes 排布）
//     col[1]  : [M][M][M][M] ...      ← MeshRef 列
//
// 这样遍历就是顺序访存，这是 L3 最大的性能杠杆（§4.2）。代价是**增删组件要迁移整行**：
// 行要从一个 archetype 搬到另一个（目标 archetype 不存在时还会新建一个）。
// 这条代价决定了"组件别拆太碎"，典型实体 3~5 个组件（§7.4）。
//
// ============================ 三条不变量（改动前先读） ============================
//
//   不变量 1：`EntityId.index` 恒等于"实体行号的稳定编号"，迁移 archetype 时
//             **不变**；只有组件列里的数据在搬。
//             → 命令流/DrawRequest::entity_id/DirtySet 全都拿 index 当键，
//               迁移如果换了 index，外部缓存的实体身份就全废了。
//
//   不变量 2：行内位置（archetype 列数组里的下标）**不是**实体身份。
//             删除用 swap-remove，会把末行挪到洞上，于是别的实体的行内位置会变。
//             要按实体存东西，键必须是 EntityId，不是 (archetype, row)。
//
//   不变量 3：gen 只在**复用一个空闲代理（free slot）时** +1；快照恢复与
//             despawn 都不加。这样同一条操作序列（含快照往返）产生的
//             (index, gen) 序列完全一致，命令回放才可复现（spec §11.4）。
//             底价：despawn 之后旧 {index, gen} 不会立刻失效，要等这个 index
//             被下一次 spawn 复用时才被 gen 顶掉。EXHAUSTED gen 是保留值，
//             用在"restore 恢复出的活实体"上 —— 它是标记而不是身份比较，
//             `alive()` 对它走 O(1) 的哈希查找，不会因为 gen 撞号误判悬垂句柄。
//
//   （附加）`component()` / `get()` 返回的指针**只写到下一次结构变更**为止
//   （spawn / despawn / add_component / remove_component / restore）。
//   迁移会搬行，指针就地失效 —— 这是列式存储的固有代价，缓存指针属于调用方的错。
//
// ============================ 为什么组件必须显式注册 ============================
//
// `register_component(kind, size_bytes, name)`：kind 的数值**就是列位置**，
// 注册顺序决定 archetype 位序与列数组顺序。显式注册让"新增一个功能 = 注册一个
// 组件 + 一个 System"，不用动本接口（§7.1）；同时世界在启动期就知道每列多宽，
// 可以一次性把迁移用的目标列缓冲算出来，不必在热路径上问"这个 kind 多大"。
//
// 组件必须是**平凡可拷贝 POD**（`static_assert(is_trivially_copyable)`）：
// 列数组用裸字节 + memcpy 搬行，快照也是原样字节序列化。带堆指针的组件
// 会让快照恢复变成深拷贝的泥潭，且与命令流的确定性不一致。
//
// ============================ 硬约束 ============================
//
//   - 零 `Vk*` 类型、零 `#include <vulkan/...>`（G3 扫 src/primitives/ 与 include/）；
//   - 零裸内存分配（G4）：不出现 malloc/free/realloc，不 include vk_mem_alloc.h。
//     所有内存都经 IFrameArena / IObjectPool / std::vector；
//   - 不读时钟（check_scene_no_clock 同口径）：本层没有任何时间戳；
//   - 错误类型复用 `device::Status` / `device::Result`，不另造；
//   - 接口方法名里不出现 `submit`（UIG2 只看注释与字符串之外的方法名，但为免误判
//     干脆整层不用这个词）。
#pragma once

// Windows 的 <windows.h> 会把 min/max 定义成宏,直接破坏
// std::numeric_limits<u32>::max() —— 表现为 "error C2589: '(':'::'右边的非法标记",
// 而且只有在**先 include 过 windows.h 的翻译单元**里才出现（world.h 自己
// 单测时完全正常）。也就是说，一个 header 是否"能编"取决于包含顺序，
// 这是最难查的一类构建问题。防御性地在自己的头里关掉宏。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/device/types.h"
#include "mb_internal/magicblocks/primitives/alloc.h"
#include "mb_internal/magicblocks/primitives/types.h"

// 前向声明：本头只用到 IScene 的引用，**不 include scene/scene.h**。
// 这样 include 本头的 TU（比如将来的测试）不必连带拖进 L2 的整套头；
// 实现侧 world.cpp 才 include 真身的接口。
namespace magicblocks::scene {
class IScene;
}  // namespace magicblocks::scene

namespace magicblocks::primitives {

using device::Result;
using device::Status;
using device::u32;
using device::u64;

// ================================================================ 实体句柄

// 实体标识。`index` 是行号**编号**（不随迁移改变、随 despawn 进空闲列表复用），
// `gen` 是代际号（复用同一个 index 时 +1，用来让旧句柄失效）。
//
// 为什么不用 64 位打包：调试器里 {index, gen} 两个字段一眼能看懂是哪一步出的错；
// 打包成 u64 之后，日志里 0x0000000100000003 这种数字没人愿意去拆。
struct EntityId {
  u32 index = std::numeric_limits<u32>::max();
  u32 gen = 0;

  // 默认构造即无效（index = kGenIndexMask），因此 EntityId{} 可以放心当"空值"用。
  static constexpr u32 kGenIndexMask = 0xFFFFFu;
  // gen 的保留值（保留号段，不分配给任何活实体）。
  //
  // 曾用于"restore 恢复出的实体代际未知"的旁路，但那条路已被证明是错的
  // （会让 restore 之前的任意旧句柄都判活，见 restore 的实现注释）。现在
  // 快照精确还原 gen，不再有"代际未知"状态；这个常量保留下来只是为了让
  // 号段不被误用，并且让 "gen == kExhaustedGen 一定是无效句柄" 成为可依赖性质。
  static constexpr u32 kExhaustedGen = std::numeric_limits<u32>::max();

  bool valid() const { return index != kGenIndexMask; }
  bool operator==(const EntityId& o) const { return index == o.index && gen == o.gen; }
  bool operator!=(const EntityId& o) const { return !(*this == o); }
};

inline constexpr EntityId kInvalidEntity{};

// EntityId → scene::NodeId 的映射槽位（同步到 L2 时用）。
//
// 放在公开头里是因为 `set_node_pool` 的参数类型必须是完整类型；实现细节
// （什么时候 acquire/release）仍然留在 world.cpp。24 字节满足 IObjectPool<T>
// 的 "T 至少放得下两个指针" 约束 —— 自由链表节点就存在槽位自己的内存里。
struct NodeSlot {
  std::uint64_t node = 0;  // scene::NodeId（0 = 尚未挂到 scene）
  std::uint32_t gen = 0;   // 建立映射时的 EntityId.gen（复用时据此判失效）
  bool valid = false;      // false = 这个 index 当前在 scene 里没有节点
  std::uint32_t reserved = 0;
};
static_assert(sizeof(NodeSlot) >= sizeof(void*) * 2,
              "NodeSlot 必须放得下 IObjectPool 的自由链表节点（两个指针）");

// ================================================================ 组件种类

// 组件种类。数值 = 该组件在 archetype 里的**位序**，由 `register_component` 的调用
// 顺序决定（spec §7.1）。上限 64 是因为 archetype 的组件集合用一个 u64 位图表示，
// 与 `view(mask)` 的参数类型一致 —— 多一种组件就要多一位，64 已经远多于"典型 3~5 个"。
using ComponentKind = u32;
inline constexpr ComponentKind kInvalidComponentKind = std::numeric_limits<ComponentKind>::max();
inline constexpr u32 kMaxComponents = 64;

// 组件掩码：第 k 位 = 包含第 k 种组件。
using ComponentMask = u64;

inline constexpr ComponentMask component_bit(ComponentKind kind) {
  return kind < kMaxComponents ? (ComponentMask{1} << kind) : ComponentMask{0};
}

// ================================================================ 脏标记

// 脏标记的粒度。决定"哪些缓存需要失效、哪些东西要重新同步到 scene"。
// 之所以不用一组 bool，是因为多系统会各自标脏，位图能一次 O(1) 合并查询。
enum class DirtyKind : std::uint8_t {
  Transform = 0,  // 世界矩阵 / 包围盒缓存失效（spec §4.4 的"世界矩阵缓存"）
  Bounds = 1,     // 包围盒 / 包围球缓存失效
  Material = 2,
  Mesh = 3,
  Visibility = 4,
  // 以上是"会被同步到 scene 的字段"（spec §7.3 的增量同步口径）。
};

inline constexpr u32 kDirtyKindCount = 5;
inline constexpr u32 kChangeDirtyBits = 0x1Fu;  // 低 5 位

inline constexpr u32 dirty_bit(DirtyKind kind) { return 1u << static_cast<u32>(kind); }

// 单实体的脏状态。用位图 + 计数而不是 std::set<EntityId>：
// 稳态帧里标脏的实体会被同步消费，容器清零比逐个 erase 便宜得多。
struct DirtySet {
  std::vector<EntityId> entities;  // 去重后的实体列表（顺序 = 首次标脏顺序）
  std::vector<u32> flags;          // 与 entities 一一对应的 DirtyKind 位图

  bool empty() const { return entities.empty(); }
  std::size_t size() const { return entities.size(); }
  EntityId entity(std::size_t i) const { return entities[i]; }
  u32 entity_flags(std::size_t i) const { return flags[i]; }
  void clear() {
    // 清内容但**保留容量**：稳态帧不分配（§4.1）。踩过 memset 之后忘了清零的坑，
    // 所以这里显式 clear() 而不是 memset 之后就当没事。
    entities.clear();
    flags.clear();
  }
};

// ================================================================ 快照（spec §7.2 / §7.3）

// 一个实体的快照。组件数据是各列原样字节的拼接，顺序与活体的列顺序一致
// （列顺序由 archetype 的掩码位序决定，所以"活世界 → 快照 → 活世界"的字节布局
// 完全可复现）。`size_bytes[i]` 只是**自校验**：restore 时按注册表重算每列宽度，
// 与这里对不上就说明快照来自另一个注册表（或顺序不同），必须整份拒绝。
struct EntitySnapshot {
  EntityId id{};
  ComponentMask mask = 0;
  std::vector<std::uint8_t> components;  // 各列按位序拼接
  std::vector<u32> size_bytes;           // 每列宽度（按位序，只有 mask 里的列）
};

// 整世界快照。**全或无**：restore 要么完整生效，要么世界保持原样。
//
// 为什么快照里也带上注册表：快照进命令流之后（§11），回放端的世界是
// 别的进程里新建的，光有实体数据没法解释字节布局。
struct WorldSnapshot {
  // 注册表镜像（按 kind 升序，kind == 下标）。restore 时用它判断"能否应用"：
  // 与当前注册表**逐项相同**才继续，不同就返回 Unsupported（不是悄悄覆盖）。
  std::vector<u32> component_sizes;
  std::vector<std::string> component_names;

  // 实体，按 EntityId 的 {index 升序, gen} 排序 —— 稳定顺序是快照可哈希的前提
  // （与 L2 的 SceneSnapshot 按 id 升序同口径）。
  std::vector<EntitySnapshot> entities;

  // 源世界自创建以来的变更版本号（诊断用；restore 不据此做任何决定）。
  u64 version = 0;

  bool empty() const { return entities.empty(); }
};

// ================================================================ 视图（spec §7.2 / §7.4）

// `view(mask)` 的一个分块：**同一 archetype 的连续一段行**。
// 每个组件列给出**基址**，第 r 行的第 k 列数据就在
//   `reinterpret_cast<const T*>(columns[k]) + row_offset + r`
// 处 —— 因为列数组是紧凑的，一列覆盖整块，不需要逐行拼。
//
// 为什么是"块"而不是"逐实体的 Span"：块是缓存友好的顺序访存单元，
// 而逐实体给 Span 等于把一次遍历拆成 count 次随机跳跃（§4.2 的反面）。
struct ComponentBlock {
  u32 archetype = 0;        // archetype 序号（调试/断言用）
  ComponentMask mask = 0;   // 本 archetype 的全部组件（可能是 view 掩码的超集）
  u32 count = 0;            // 本块行数
  const EntityId* ids = nullptr;      // 块首的 EntityId 数组
  u32 row_offset = 0;                 // 本块在 archetype 里的起始行
  // 按**位序**给出本块第一个实体的各列首地址：columns[k] 对应第 k 个组件列
  // （k 遍历 mask 的置位，从低位到高位）。has(component_bit(kind)) 为假时
  // 该槽位的指针无意义（nullptr），不要解引用。
  const void* const* columns = nullptr;
  const u32* column_sizes = nullptr;  // 与 columns 一一对应的列宽度（字节）
};

// 按 archetype 分块的遍历对象。
//
// **零分配**（§7.4"内部预分配 archetype 列表"）：`next()` 只是在
// 预分配好的列表上移动游标，指针都指向世界内部，不持有任何所有权。
// `archetypes_` 的拷贝是唯一的一次小分配，发生在 `view()` 里 ——
// 后者把一个缓存的列表直接 move 进来，稳态帧 0 次堆分配。
//
// **生命周期（两条，都是硬约束）**：
//   1. 结构变更后失效 —— 遍历期间不要 spawn/despawn/增删组件/restore；
//   2. **帧内一次性使用** —— `view()` 返回的列表在下次结构变更前有效，
//      **不要跨帧持有**、也不要把它存成成员。跨帧持有的代价是它悄悄延长了
//      "世界冻结"的窗口，且很容易在下一帧读到已经搬走的行。
//      （为什么不做成 world 持有游标的轻量句柄：那样两个 view 同时存在会
//      互相踩游标 —— `auto a = w.view(m); auto b = w.view(m);` 就是静默 bug。
//      拷贝一份 u32 数组的代价远小于这类错误。）
class ComponentView {
 public:
  ComponentView() = default;

  ComponentMask mask() const { return mask_; }
  // 还剩的块数（不是行数）。
  std::size_t remaining() const { return total_ - cursor_; }
  // 总行数 —— 只统计**被访问的 archetype**，这就是 `ViewOnlyVisitedArchetypes`
  // 这条测试要断言的数字：不含目标组件的 archetype 一次都不会被触碰。
  u64 total_entities() const { return total_entities_; }
  // 被访问的 archetype 数。
  std::size_t visited_archetypes() const { return total_; }

  // 取下个块，填进 *block。返回 false = 遍历结束。空 archetype 不会产生块。
  bool next(ComponentBlock* block);

 private:
  friend class IWorld;
  friend class WorldImpl;  // 实现类型（定义在 world.cpp，不进公开头）

  // 用函数指针而不是虚基类：视图是纯值对象，加一层虚派发意味着每个视图
  // 多一个 vptr，且要被拷来拷去 —— 这里一次间接调用就够了。
  using BlockFiller = bool (*)(void* world, u32 archetype, const ComponentView& view,
                               ComponentBlock* out_block);

  ComponentView(ComponentMask mask, std::vector<u32> archetypes, void* world,
                BlockFiller filler)
      : mask_(mask), archetypes_(std::move(archetypes)), world_(world), filler_(filler) {
    total_ = archetypes_.size();
  }

  void set_total_entities(u64 n) { total_entities_ = n; }

  // 实现侧（world.cpp 的 fill_block）需要直接写下面的列指针数组。
  friend struct ComponentViewAccess;

  ComponentMask mask_ = 0;
  std::vector<u32> archetypes_;  // 与 view() 缓存共享同一份数据（move 进来）
  std::size_t cursor_ = 0;
  std::size_t total_ = 0;
  u64 total_entities_ = 0;
  void* world_ = nullptr;
  BlockFiller filler_ = nullptr;

  // 每次 next() 填块用的列指针数组，放进对象体内而不是堆上分配，
  // 保证整个遍历链路一次堆分配都没有。
  static constexpr u32 maxColumnSlots = kMaxComponents;
  const void* columns_[maxColumnSlots] = {};
  u32 column_sizes_[maxColumnSlots] = {};
};

// 实现侧的填块器需要访问 ComponentView 的私有列指针数组。单独立一个
// 只声明、不定值的访问桥，比把整个 World 类声明为 friend 更窄：
// 公开头里不必出现实现类型的名字。
struct ComponentViewAccess {
  static void write_columns(ComponentView& v, u32 slot, const void* data, u32 size) {
    if (slot >= ComponentView::maxColumnSlots) return;
    v.columns_[slot] = data;
    v.column_sizes_[slot] = size;
  }
  // 每次填块前把指针表清空：否则上一块的残留指针可能被误读成"这个 archetype
  // 也有那一列"。列的个数由视图掩码固定，残留不会发生，但显式清掉让
  // 越界读变成"读到 nullptr"而不是"读到别人的列"。
  static void reset_columns(ComponentView& v) {
    for (u32 i = 0; i < ComponentView::maxColumnSlots; ++i) {
      v.columns_[i] = nullptr;
      v.column_sizes_[i] = 0;
    }
  }
  static u32 max_slots() { return ComponentView::maxColumnSlots; }
  static const void* const* column_pointers(const ComponentView& v) { return v.columns_; }
  static const u32* column_size_pointers(const ComponentView& v) { return v.column_sizes_; }
};

// ================================================================ IWorld（spec §7.2）

class IWorld {
 public:
  IWorld(const IWorld&) = delete;
  IWorld& operator=(const IWorld&) = delete;
  virtual ~IWorld() = default;

  // ------------------------------------------------------------ 组件注册（启动期一次）

  // 注册一种组件。`kind` 若非法（>= 64 或已是 kInvalidComponentKind）→ InvalidArgument；
  // 重复注册同一个 kind → InvalidArgument（注册是显式的，静默覆盖只会掩盖笔误）；
  // `size_bytes` 为 0 或超过 64 KiB → InvalidArgument（列宽离谱基本是 sizeof 写错）。
  //
  // **必须在 spawn 任何实体之前完成注册**：已存在的 archetype 不会因为新注册而
  // 多出一列，而新实体的位图可能与旧 archetype 的掩码撞上却宽度不同。
  virtual Status register_component(ComponentKind kind, u32 size_bytes, const char* name) = 0;

  // 带**默认值**的注册：之后 `add_component` 新加这一列时，新列铺的是这份
  // 字节，而不是全零。
  //
  // 为什么需要它：全零对某些组件是**语义错误**而不只是"没初始化"。最典型的是
  // TransformComp —— 它的 scale 默认是 (1,1,1)，若新列铺零就变成 (0,0,0)，
  // 实体被缩放成一个点、彻底看不见，且不崩不报错。这类 bug 只有画出来才
  // 发现得了。所以"组件的默认值"必须由注册方显式说出来。
  //
  // `default_bytes` 必须恰好 `size_bytes` 长（通常就是
  // `&T::kDefaultValue` 或一个 `T{}` 的地址）；长度不符 → InvalidArgument。
  virtual Status register_component(ComponentKind kind, u32 size_bytes, const char* name,
                                    const void* default_bytes) = 0;

  virtual bool is_registered(ComponentKind kind) const = 0;
  virtual u32 component_size(ComponentKind kind) const = 0;
  virtual u32 registered_count() const = 0;

  // ------------------------------------------------------------ 实体

  // 新建实体（无组件，落在空掩码 archetype 里）。优先复用 despawn 留下的空闲行，
  // 复用时 gen 加一 —— 旧句柄因此失效。
  virtual Result<EntityId> spawn() = 0;

  // 删除实体。返回 false = 句柄已失效（悬垂/从未存在）。
  // 实现是 swap-remove：把本 archetype 的最后一行挪到洞上，行数是 O(1) 减少，
  // 但被挪的实体行内位置变了（不变量 2）。
  virtual bool despawn(EntityId id) = 0;

  virtual bool alive(EntityId id) const = 0;

  virtual u32 entity_count() const = 0;
  // 本世界有史以来发出的**不同 index** 数（含已销毁、待复用）。不是 gen 计数。
  virtual u32 allocated_index_count() const = 0;

  // ------------------------------------------------------------ 组件（O(1)，不在同一 archetype 会自动迁移）

  virtual Status add_component(EntityId id, ComponentKind kind) = 0;
  virtual Status remove_component(EntityId id, ComponentKind kind) = 0;
  virtual bool has_component(EntityId id, ComponentKind kind) const = 0;
  virtual ComponentMask mask_of(EntityId id) const = 0;

  // 类型擦除指针：指向该实体在这条列里的那一行。没这个组件 / 实体已死 → nullptr。
  // **只写到下一次结构变更**（见头部不变量说明）。
  virtual void* component(EntityId id, ComponentKind kind) = 0;
  virtual const void* component(EntityId id, ComponentKind kind) const = 0;

  // 便捷包装：按 `T::kComponentKind` 静态映射组件种类。模板写在这里是因为
  // 它不需要虚派发 —— 只做一次 static_cast 到 T*，类型安全由调用方保证
  // （组件宽度已在 register_component 时注册，错了会在 first use 处断言）。
  template <typename T>
  T* get(EntityId id) {
    return static_cast<T*>(component(id, T::kComponentKind));
  }
  template <typename T>
  const T* get(EntityId id) const {
    return static_cast<const T*>(component(id, T::kComponentKind));
  }
  template <typename T>
  Status add(EntityId id) {
    return add_component(id, T::kComponentKind);
  }
  template <typename T>
  Status remove(EntityId id) {
    return remove_component(id, T::kComponentKind);
  }
  template <typename T>
  bool has(EntityId id) const {
    return has_component(id, T::kComponentKind);
  }

  // ------------------------------------------------------------ 遍历（性能核心）

  // 按掩码遍历。只走"包含掩码全部位"的 archetype（§4.2 / §7.4）。
  // 返回的视图零分配，但**在结构变更后失效**。
  virtual ComponentView view(ComponentMask mask) = 0;

  // ------------------------------------------------------------ 脏标记

  virtual void mark_dirty(EntityId id, DirtyKind kind) = 0;
  // 取出并**清空**当前脏集合（增量同步的消费点）。
  virtual const DirtySet& take_dirty() = 0;
  virtual const DirtySet& dirty() const = 0;

  // 任何变更（spawn/despawn/增删组件/restore）使版本号 +1；mark_dirty **不加**版本号。
  // 理由是两者的语义不同：version 是**结构性**指纹（进命令流哈希）,脏标记是
  // 每帧都会被 system 无差别刷新的瞬态信号，如果它也 +1，version 每帧都变，
  // 命令流哈希就失去了"输入没变 → 输出没变"的判别力。
  virtual u64 version() const = 0;

  // ------------------------------------------------------------ L2 桥（spec §7.3）

  // 把脏实体**增量**推给 IScene：只处理 take_dirty() 里的实体，不碰其它节点。
  //
  // 映射规则（由本实现定死，L2 侧不需要知道 ECS 的存在）：
  //   - 实体首次需要出现在 scene 里时 → add_node 挂到根下，
  //     节点名为 `"entity#" + index`（L2 的调试输出/命令流 diff 里节点名是
  //     唯一直接可读的标识；空名会让"这个节点是谁"必须回翻 ECS 才能回答）；
  //   - 实体 despawn → remove_node（被连带删掉的子树也在 scene 侧）；
  //   - Transform 脏 → set_transform（由 TransformComp 的 t/q/s 填 scene::Transform）；
  //   - Bounds 脏     → set_local_bounds（由 LocalBoundsComp 的 Box 填 scene::Aabb）；
  //   - Visibility 脏 → set_visible（由 VisibleComp；没有该组件按可见处理）。
  // 其余 DirtyKind（Material/Mesh）**不进 scene** —— 它们是渲染侧的事,
  // 塞进 L2 快照只会让命令流带上不该有的字段。
  //
  // **调用方契约：despawn 之后必须至少调用一次 sync_to_scene。**
  // despawn 只把内部节点槽位标成失效，**删除本身不在 despawn 那一刻发生**
  // （删除必须进命令流，而命令流由 sync 产出）。所以 despawn 之后若一直不再同步，
  // scene 里会留下一个永不消失的节点 —— 表现为"删不掉的幽灵物体"。
  //
  // 一个实体一次同步时只会消耗它的一位脏标记，标志从 take_dirty() 取的
  // 那一份干净副本上读，因此重复 sync 不会重复推送。
  virtual Status sync_to_scene(scene::IScene& scene_target) = 0;

  // ------------------------------------------------------------ 快照

  virtual WorldSnapshot snapshot() const = 0;
  // 全或无：任何一条校验失败都返回错误，且**世界保持原样**（不是"恢复了一半"）。
  virtual Status restore(const WorldSnapshot& snapshot) = 0;

  // 清空所有实体（保留注册表；已发出的 index 仍不复用，与 L2 的 clear 同口径——
  // 复用会破坏命令流里"同一个 index 指的是哪个实体"的推理）。
  virtual void clear_entities() = 0;

  // ------------------------------------------------------------ 内存来源（可选，热路径零分配）

  // 注入帧内临时缓冲：同步到 scene、迁移时的临时数组从这里借。为 nullptr 时
  // 退回内部 std::vector（单元测试与冷路径允许，热路径不允许 —— 见 §4.1）。
  virtual void set_frame_arena(IFrameArena* arena) = 0;
  // 每帧开局调一次：把帧内临时区归还（arena->reset() 由调用方负责，
  // 本方法只把世界侧的借用游标归零）。
  virtual void begin_frame() = 0;
  // 自上次 begin_frame 以来从外部借用（arena）的字节数，诊断用。
  virtual u64 frame_bytes_used() const = 0;

  // EntityId → scene::NodeId 的映射槽位池。类型由注入方持有、由实现侧定义
  // （见 world.cpp 的 NodeSlot，24 字节，满足 IObjectPool<T> 的"至少两个指针"）。
  // 池可以为空：为空时退回内部 deque（单元测试与冷路径）。传入的池应当
  // reserve 到"实体数上限"——这是启动期一次性动作，热路径不该扩池。
  virtual void set_node_pool(IObjectPool<NodeSlot>* pool) = 0;

  virtual void set_stats(IFrameStats* stats) = 0;

 protected:
  IWorld() = default;
};

std::unique_ptr<IWorld> create_world();

// ================================================================ spec §7.1 的组件（POD）
//
// 这些类型是 spec 里点名的"玩法常用组件"。它们放在 world.h 而不是各自的玩法
// 头里，是因为它们跨玩法共享（Transform / Visible 三个玩法都要），
// 且都是平凡可拷贝 POD —— 只有 POD 才能进列式存储与快照。
//
// 每个组件自带 `kComponentKind`：`register_standard_components(world)` 按这个
// 顺序给它们排号，`world.get<TransformComp>(id)` 因此不需要调用方记住数字。
// 这不是"把组件种类写死在接口里"—— 接口仍然是按 ComponentKind 排号的，
// 只是给标准组件一个**约定的**号段（0..7），自增组件从 8 往后自由排。

// 与 scene::Transform 同序（t[3] / q[4] / s[3]），迁移到 L2 时直接逐字段拷。
struct TransformComp {
  static constexpr ComponentKind kComponentKind = 0;
  float translation[3] = {0.0f, 0.0f, 0.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct LocalBoundsComp {
  static constexpr ComponentKind kComponentKind = 1;
  Box box;
};

struct MeshRefComp {
  static constexpr ComponentKind kComponentKind = 2;
  MeshId mesh = kInvalidMeshId;
};

struct MaterialRefComp {
  static constexpr ComponentKind kComponentKind = 3;
  // MaterialTable 槽位。0 是保留的默认材质（spec §9）。
  u32 material = kDefaultMaterialSlot;
};

struct StrokeComp {
  static constexpr ComponentKind kComponentKind = 4;
  u32 stroke_id = 0;  // 一个实体 = 一笔（spec §12 魔法涂鸦）
};

struct LayerComp {
  static constexpr ComponentKind kComponentKind = 5;
  u32 layer = 0;
};

struct VisibleComp {
  static constexpr ComponentKind kComponentKind = 6;
  bool visible = true;
};

struct GizmoTargetComp {
  static constexpr ComponentKind kComponentKind = 7;
  u32 handle = 0;
};

// 标准组件的数量与宽度表（kind → size），与上面的 kComponentKind 一一对应。
// 新加标准组件时：在枚举后追加、调大 kStandardComponentCount、在 world.cpp
// 的 register_standard_components 里登记 —— 三处一起改，故意不自动化，
// 因为"多加一个标准组件"本来就是需要设计评审的事。
inline constexpr u32 kStandardComponentCount = 8;

// 列式存储的前提：组件必须平凡可拷贝，否则 memcpy 搬行与快照都会漏掉所有权。
static_assert(std::is_trivially_copyable_v<TransformComp>, "组件必须是平凡可拷贝的 POD");
static_assert(std::is_trivially_copyable_v<LocalBoundsComp>, "组件必须是平凡可拷贝的 POD");
static_assert(std::is_trivially_copyable_v<MeshRefComp>, "组件必须是平凡可拷贝的 POD");
static_assert(std::is_trivially_copyable_v<MaterialRefComp>, "组件必须是平凡可拷贝的 POD");
static_assert(std::is_trivially_copyable_v<StrokeComp>, "组件必须是平凡可拷贝的 POD");
static_assert(std::is_trivially_copyable_v<LayerComp>, "组件必须是平凡可拷贝的 POD");
static_assert(std::is_trivially_copyable_v<VisibleComp>, "组件必须是平凡可拷贝的 POD");
static_assert(std::is_trivially_copyable_v<GizmoTargetComp>, "组件必须是平凡可拷贝的 POD");

// 把上面 8 个标准组件按 kComponentKind 排号注册到 world。**启动期调用一次**，
// 之后才能 spawn（见 register_component 的约束）。返回第一个失败的 Status，
// 成功返回 Ok。之所以做成自由函数而不是 IWorld 的虚方法：这样"要不要用标准组件"
// 是使用方的选择，IWorld 的接口不必知道有哪些标准组件。
Status register_standard_components(IWorld& world);

}  // namespace magicblocks::primitives
