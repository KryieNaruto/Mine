// MagicBlocks L3 —— `IWorld` 的实现（spec §4.2 SOA / §7 / D-L3-9）
//
// 本 TU 的全部结构都围绕一句话：**实体身份 (index, gen) 与实体位置 (archetype, row)
// 是两件事**。绝大多数 ECS bug 都来自把两者混为一谈 —— 迁移 archetype 改了行号，
// swap-remove 改了行号，而外部句柄、命令流、DrawRequest::entity_id 全都只认 index。
// 所以这里 index 一旦发出就不再变，行的搬运完全在内部完成。
//
// 关键数据结构（详见 world_storage.h）：
//   slots_      : index → {gen, archetype, row}。这是唯一的"实体 → 位置"索引，
//                 所以 alive/has_component/component 全是 O(1) 两次下标。
//   archetypes_ : 掩码 → archetype 的稠密数组 + 哈希索引 `archetype_lookup_`。
//   dirty_      : 实体 → DirtyKind 位图的去重集合（同步到 scene 的输入）。
//
// 关于 IObjectPool：§8.1 把它定位给"实体行 / Stroke / GizmoHandle"。本实现里
// 实体行是 archetype 列数组（会变长，见 world_storage.h 的说明），Stroke/Gizmo
// 属各自玩法；**但同步到 IScene 必需的 EntityId → NodeId 映射槽位**正好是
// "数量与实体数同阶、频繁 acquire/release、地址必须稳定"的典型，用它最合适。
// 因此 set_node_pool() 注入了 IObjectPool<NodeSlot>，槽位类型是 24 字节的 NodeSlot。
// 不注入时退回内部 std::deque（单元测试与冷路径；deque 的稳定性要 per-element）。
#include "mb_internal/magicblocks/primitives/world.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mb_internal/magicblocks/primitives/alloc.h"
#include "mb_internal/magicblocks/scene/scene.h"
#include "primitives/world_storage.h"

namespace magicblocks::primitives {

namespace {

using detail::Archetype;
// 空掩码 archetype 恒定在 0 号位：无组件实体不可能是"刚迁移出来的"，
// 固定住它可以让 spawn 少一次查表，也让快照里"只有 id 没有列"的实体有个着落。
constexpr u32 kEmptyArchetype = 0;

// 单个 index 的槽位：gen + 当前位置。
struct EntitySlot {
  u32 gen = 0;
  u32 archetype = kEmptyArchetype;
  u32 row = 0;
  bool live = false;
};

// NodeSlot 定义在公开头里（set_node_pool 的参数类型必须完整，见 world.h）。
// 它的尺寸约束 static_assert 也在那边。

// 64 位终混（与 batch.cpp 同款 splitmix finalizer），用于查表与去重。
inline std::uint64_t mix64(std::uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

// 每个实体一位的"能否按 20 位窄化 gen 比较"标记表。
//
// 已废弃并移除：早先试图用"代际未知的恢复实体"来表达快照恢复，需要一个
// 旁路标记表。后来发现快照本来就存了完整的 EntityId（含 gen），restore 精确
// 还原即可，不存在第三种代际状态 —— 这张表和它的旁路分支一并删掉了。
// 保留这段说明是为了让后来者不要重新发明它。

// ================================================================ 帧内临时区
//
// 迁移 archetype 时**不需要**临时列（目标列直接分配、直接拷贝），真正需要
// 帧内临时区的是 sync_to_scene：它要先把脏集合拷出来再清空世界侧的集合，
// 否则"同步过程中新标脏"会和正在处理的集合互相污染。
//
// 注入 arena 时从 arena 借（稳态帧 0 次堆分配）；没注入时退回成员 deque
// （元素地址稳定，容量跨调用复用）。arena 借不到就返回 nullptr 让调用方报错，
// 绝不偷偷退回堆分配 —— 那样"稳态帧 0 分配"的断言会在无形中失效。
class Scratch {
 public:
  void set_arena(IFrameArena* arena) { arena_ = arena; }
  void new_frame() {
    frame_used_ = 0;
    fallback_.clear();
  }

  u64 frame_bytes_used() const { return frame_used_; }

  void* borrow(std::size_t bytes, std::size_t align) {
    if (bytes == 0) return nullptr;
    if (arena_ != nullptr) {
      void* p = arena_->alloc(bytes, align);
      if (p != nullptr) frame_used_ += bytes;
      return p;
    }
    fallback_.emplace_back();
    fallback_.back().resize(bytes);
    return fallback_.back().data();
  }

 private:
  IFrameArena* arena_ = nullptr;
  std::deque<std::vector<std::uint8_t>> fallback_;
  u64 frame_used_ = 0;
};

// ================================================================ World
//
// 定义在**命名空间作用域**（对外不可见，因为只在 world.cpp 里存在）：
// ComponentView 的 `friend class WorldImpl;` 指的就是这个名字，而匿名命名空间
// 里的同名类是另一个类型，友元授权会失效（这正是上面先 `}  // namespace`、
// 下面再重新打开匿名命名空间的原因）。

}  // namespace

class WorldImpl final : public IWorld {
 public:
  WorldImpl() {
    // 0 号 archetype 恒为空掩码，先建好。
    Archetype empty;
    empty.mask = 0;
    archetypes_.push_back(std::move(empty));
  }

  ~WorldImpl() override {
    if (node_pool_ != nullptr) {
      for (NodeSlot* ns : node_slots_) {
        if (ns != nullptr) node_pool_->release(ns);
      }
    }
  }

  // ------------------------------------------------------------ 组件注册

  Status register_component(ComponentKind kind, u32 size_bytes, const char* name) override {
    return register_component_impl(kind, size_bytes, name, nullptr);
  }

  Status register_component(ComponentKind kind, u32 size_bytes, const char* name,
                            const void* default_bytes) override {
    return register_component_impl(kind, size_bytes, name, default_bytes);
  }

  Status register_component_impl(ComponentKind kind, u32 size_bytes, const char* name,
                                 const void* default_bytes) {
    if (kind >= kMaxComponents || kind == kInvalidComponentKind) {
      return Status::Invalid("IWorld::register_component: kind 超出 [0, 64) 的范围");
    }
    if (size_bytes == 0) {
      return Status::Invalid("IWorld::register_component: 组件宽度必须 > 0");
    }
    if (size_bytes > kMaxComponentBytes) {
      return Status::Invalid(
          "IWorld::register_component: 组件宽度超过 64 KiB，大概率是 sizeof 写错");
    }
    if (registry_[kind].registered) {
      return Status::Invalid(
          "IWorld::register_component: kind 重复注册（注册是显式的，不静默覆盖）");
    }
    if (allocated_index_count() != 0) {
      // 已有 archetype 不会因为新注册而多出一列；此时注册会让新实体的位图
      // 与旧 archetype 撞掩码却宽度不同。启动期一次性注册才是契约。
      return Status::Invalid("IWorld::register_component: 必须在本世界 spawn 过实体之前注册");
    }
    registry_[kind].registered = true;
    registry_[kind].size = size_bytes;
    registry_[kind].name = (name != nullptr) ? name : "";
    if (default_bytes != nullptr) {
      registry_[kind].default_bytes.assign(static_cast<const std::uint8_t*>(default_bytes),
                                           static_cast<const std::uint8_t*>(default_bytes) +
                                               size_bytes);
      registry_[kind].default_size = size_bytes;
    }
    ++registered_count_;
    return Status::Ok_();
  }

  bool is_registered(ComponentKind kind) const override {
    return kind < kMaxComponents && registry_[kind].registered;
  }
  u32 component_size(ComponentKind kind) const override {
    return is_registered(kind) ? registry_[kind].size : 0u;
  }
  u32 registered_count() const override { return registered_count_; }

  // ------------------------------------------------------------ 实体

  Result<EntityId> spawn() override {
    EntityId id;
    if (!free_indices_.empty()) {
      // 复用：gen +1 让所有旧句柄失效。这是唯一会让 gen 变化的地方（不变量 3）。
      id.index = free_indices_.back();
      free_indices_.pop_back();
      EntitySlot& s = slots_[id.index];
      ++s.gen;
      id.gen = s.gen;
      s.live = true;
      s.archetype = kEmptyArchetype;
      s.row = static_cast<u32>(archetypes_[kEmptyArchetype].push_row());
    } else {
      if (slots_.size() >= static_cast<std::size_t>(EntityId::kGenIndexMask)) {
        return Status::OOM("IWorld::spawn: index 空间耗尽（上限 1M 个不同 entity index）");
      }
      id.index = static_cast<u32>(slots_.size());
      id.gen = 0;
      slots_.push_back(EntitySlot{0, kEmptyArchetype, 0, true});
      if (node_slots_.size() < slots_.size()) node_slots_.resize(slots_.size(), nullptr);
      id.index = static_cast<u32>(slots_.size() - 1);
      slots_[id.index].row = static_cast<u32>(archetypes_[kEmptyArchetype].push_row());
    }
    Archetype& empty = archetypes_[kEmptyArchetype];
    empty.ids[slots_[id.index].row] = id;
    ++live_entity_count_;
    ++version_;
    return id;
  }

  bool despawn(EntityId id) override {
    if (!alive(id)) return false;
    const u32 index = id.index;
    EntitySlot& s = slots_[index];
    const u32 a = s.archetype;
    const u32 row = s.row;
    Archetype& arch = archetypes_[a];

    // swap-remove：末行挪到洞上，行数 -1。被挪的实体 id 不变（不变量 1），
    // 只是它在列数组里的位置变了（不变量 2），所以必须把它的槽位 row 一起更新 ——
    // 漏掉这一行就是最经典的 ECS bug：之后所有对它的访问都读到别人那一行。
    const std::size_t last = arch.size() - 1;
    if (row != last) {
      const EntityId moved = arch.ids[last];
      arch.ids[row] = moved;
      for (std::size_t k = 0; k < arch.columns.size(); ++k) {
        std::memcpy(arch.column_row(k, row), arch.column_row(k, last), arch.column_sizes[k]);
      }
      slots_[moved.index].row = row;
    }
    arch.shrink_to(last);

    s.live = false;
    s.archetype = kEmptyArchetype;
    s.row = 0;
    free_indices_.push_back(index);
    --live_entity_count_;
    NodeSlot* ns = node_slot(index);
    if (ns != nullptr) ns->valid = false;  // scene 节点留待 sync 时删（删除要进命令流）
    ++version_;
    return true;
  }

  bool alive(EntityId id) const override {
    if (id.index >= slots_.size()) return false;
    const EntitySlot& s = slots_[id.index];
    if (!s.live) return false;
    // gen 只有 20 位有效（高位被 pack 格式占着），比较时也按 20 位窄化。
    // 快照恢复出的实体同样走这条路径 —— 它的 gen 是从快照精确还原的，
    // 与普通实体语义一致，不存在"代际未知"的第三种状态。
    return (id.gen & EntityId::kGenIndexMask) == s.gen;
  }

  u32 entity_count() const override { return live_entity_count_; }
  u32 allocated_index_count() const override { return static_cast<u32>(slots_.size()); }

  // ------------------------------------------------------------ 组件

  Status add_component(EntityId id, ComponentKind kind) override {
    if (!alive(id)) return Status::Invalid("IWorld::add_component: 实体句柄无效");
    if (!is_registered(kind)) return Status::Invalid("IWorld::add_component: 组件未注册");
    EntitySlot& s = slots_[id.index];
    const ComponentMask bit = component_bit(kind);
    if ((archetypes_[s.archetype].mask & bit) != 0) {
      return Status::Ok_();  // 幂等：已有该组件就什么都不做（同掩码迁移是空操作）
    }
    migrate(id, archetypes_[s.archetype].mask | bit, kind, /*target_column_new=*/true);
    ++version_;
    return Status::Ok_();
  }

  Status remove_component(EntityId id, ComponentKind kind) override {
    if (!alive(id)) return Status::Invalid("IWorld::remove_component: 实体句柄无效");
    if (!is_registered(kind)) return Status::Invalid("IWorld::remove_component: 组件未注册");
    EntitySlot& s = slots_[id.index];
    const ComponentMask bit = component_bit(kind);
    if ((archetypes_[s.archetype].mask & bit) == 0) return Status::Ok_();  // 幂等
    migrate(id, archetypes_[s.archetype].mask & ~bit, kInvalidComponentKind,
            /*target_column_new=*/false);
    ++version_;
    return Status::Ok_();
  }

  bool has_component(EntityId id, ComponentKind kind) const override {
    if (!alive(id)) return false;
    return (archetypes_[slots_[id.index].archetype].mask & component_bit(kind)) != 0;
  }

  ComponentMask mask_of(EntityId id) const override {
    if (!alive(id)) return 0;
    return archetypes_[slots_[id.index].archetype].mask;
  }

  void* component(EntityId id, ComponentKind kind) override {
    if (!alive(id)) return nullptr;
    const EntitySlot& s = slots_[id.index];
    Archetype& a = archetypes_[s.archetype];
    const std::size_t k = a.slot_of(kind);
    if (k == kMaxComponents) return nullptr;
    return a.column_row(k, s.row);
  }

  const void* component(EntityId id, ComponentKind kind) const override {
    return const_cast<WorldImpl*>(this)->component(id, kind);
  }

  // ------------------------------------------------------------ 遍历

  ComponentView view(ComponentMask mask) override {
    auto it = visible_cache_.find(mask);
    if (it == visible_cache_.end()) {
      // 缓存"掩码 → archetype 序号列表"：新 archetype 出现时整表清空。
      // 不做位图交集之类的加速 —— 一个世界通常只有几十个 archetype，
      // 缓存之后这条路径每次只付一次哈希查找。
      std::vector<u32> list;
      for (u32 a = 0; a < archetypes_.size(); ++a) {
        if ((archetypes_[a].mask & mask) == mask) list.push_back(a);
      }
      it = visible_cache_.emplace(mask, std::move(list)).first;
    }
    u64 total = 0;
    for (u32 a : it->second) total += archetypes_[a].size();
    // 列表拷一份进视图（视图是值对象，可能活过本次调用）；拷的是 u32 序号，
    // 与缓存同尺寸。这里没有"稳态帧 0 分配"——view() 每次都会分配这一份，
    // 已在 spec §7.4 的"视图零分配"口径外：那是说 next() 不分配，而非 view()。
    ComponentView v(mask, it->second, this, &WorldImpl::fill_block);
    v.set_total_entities(total);
    return v;
  }

  // ------------------------------------------------------------ 脏标记

  void mark_dirty(EntityId id, DirtyKind kind) override {
    if (!alive(id)) return;  // 悬垂句柄标脏是调用方的错；同步时也推不动，静默忽略
    if (dirty_index_.size() < slots_.size()) dirty_index_.resize(slots_.size(), kNoDirty);
    u32& slot = dirty_index_[id.index];
    if (slot == kNoDirty) {
      slot = static_cast<u32>(dirty_.entities.size());
      dirty_.entities.push_back(id);
      dirty_.flags.push_back(0);
    }
    dirty_.flags[slot] |= dirty_bit(kind);
    // 只统计、不改 version_：脏标记是每帧被无差别刷新的瞬态信号，
    // 若它也 +1，version 每帧都变，命令流哈希就失去了判别力（见头文件说明）。
    ++dirty_mark_count_;
  }

  const DirtySet& take_dirty() override { return dirty_; }

  const DirtySet& dirty() const override { return dirty_; }

  u64 version() const override { return version_; }

  // ------------------------------------------------------------ L2 桥

  Status sync_to_scene(scene::IScene& scene_target) override {
    if (dirty_.empty()) return Status::Ok_();

    // 把脏集合**先拷进帧内临时区**再清空世界侧：同步过程中新产生的标脏
    // 留给下一帧，不会被本轮反复处理，也不会迭代中改容器。
    const std::size_t n = dirty_.entities.size();
    Scratch& sc = scratch_;
    auto* ids = static_cast<EntityId*>(sc.borrow(n * sizeof(EntityId), alignof(EntityId)));
    auto* flags = static_cast<u32*>(sc.borrow(n * sizeof(u32), alignof(u32)));
    if (ids == nullptr || flags == nullptr) {
      return Status::OOM("IWorld::sync_to_scene: 帧内临时区不足（IFrameArena 容量不够）");
    }
    std::memcpy(ids, dirty_.entities.data(), n * sizeof(EntityId));
    std::memcpy(flags, dirty_.flags.data(), n * sizeof(u32));
    // 必须调 clear()（保留容量），而不是 memset：dirty_index_ 的映射要靠
    // "集合清空 + 映射复位"一起重建，只 memset 不清映射就是"标脏一次后再也
    // 不重复入集合"的隐蔽 bug。
    dirty_.clear();
    std::fill(dirty_index_.begin(), dirty_index_.end(), kNoDirty);

    for (std::size_t i = 0; i < n; ++i) {
      const EntityId id = ids[i];
      const u32 f = flags[i];
      NodeSlot* ns = node_slot(id.index);

      if (!alive(id)) {
        // despawn 之后才被同步到：把 scene 里的节点删掉。这是"删除"能进
        // 命令流的唯一路径（IScene 没有实体概念，只认节点）。
        if (ns != nullptr && ns->valid && ns->node != 0) {
          scene_target.remove_node(static_cast<scene::NodeId>(ns->node));
          ns->valid = false;
          ns->node = 0;
        }
        continue;
      }

      ns = ensure_node_slot_value(id.index);
      if (ns == nullptr) return Status::OOM("IWorld::sync_to_scene: 没有可用的节点映射槽位");
      if (!ns->valid || ns->node == 0) {
        // 节点名给一个**可读的**标识而非空串：L2 的调试输出与命令流 diff 里，
        // 节点名是唯一能直接读懂的东西，空名会让"这个节点是谁"变成
        // "得翻 ECS 才知道"。拼接只在实体首次进 scene 时发生一次，不在热路径。
        char name_buf[32];
        const int n = std::snprintf(name_buf, sizeof(name_buf), "entity#%u", id.index);
        const std::string node_name =
            n > 0 ? std::string(name_buf, static_cast<std::size_t>(n)) : std::string();
        Result<scene::NodeId> node = scene_target.add_node(scene::kInvalidNodeId, node_name);
        if (!node.ok()) return node.status();
        ns->node = static_cast<std::uint64_t>(node.value());
        ns->valid = true;
      }
      const auto node_id = static_cast<scene::NodeId>(ns->node);

      // 推哪个字段**只看脏位**（§7.3 增量口径）：没标的位连 slot_of 都不做。
      const EntitySlot& s = slots_[id.index];
      const Archetype& a = archetypes_[s.archetype];
      if ((f & dirty_bit(DirtyKind::Transform)) != 0) {
        const std::size_t k = a.slot_of(TransformComp::kComponentKind);
        if (k != kMaxComponents) {
          const auto* tr = static_cast<const TransformComp*>(a.column_row(k, s.row));
          scene::Transform out;
          std::memcpy(out.translation, tr->translation, sizeof(out.translation));
          std::memcpy(out.rotation, tr->rotation, sizeof(out.rotation));
          std::memcpy(out.scale, tr->scale, sizeof(out.scale));
          scene_target.set_transform(node_id, out);
        }
      }
      if ((f & dirty_bit(DirtyKind::Bounds)) != 0) {
        const std::size_t k = a.slot_of(LocalBoundsComp::kComponentKind);
        if (k != kMaxComponents) {
          const Box& box = static_cast<const LocalBoundsComp*>(a.column_row(k, s.row))->box;
          scene::Aabb aabb;
          aabb.min[0] = box.min.x;
          aabb.min[1] = box.min.y;
          aabb.min[2] = box.min.z;
          aabb.max[0] = box.max.x;
          aabb.max[1] = box.max.y;
          aabb.max[2] = box.max.z;
          aabb.empty = box.empty;
          scene_target.set_local_bounds(node_id, aabb);
        }
      }
      if ((f & dirty_bit(DirtyKind::Visibility)) != 0) {
        const std::size_t k = a.slot_of(VisibleComp::kComponentKind);
        // 没有 VisibleComp 的实体按可见处理：ECS 不假设"不可见是默认"，
        // 否则新实体忘了挂组件就被静默剔除，排查成本极高。
        const bool visible = (k == kMaxComponents)
                                 ? true
                                 : static_cast<const VisibleComp*>(a.column_row(k, s.row))->visible;
        scene_target.set_visible(node_id, visible);
      }
      // Material / Mesh 不进 scene：它们是渲染侧的事（渲染器直接读组件列），
      // 塞进 L2 快照只会让命令流带上 L2 不认识的字段。
    }
    return Status::Ok_();
  }

  // ------------------------------------------------------------ 快照

  WorldSnapshot snapshot() const override {
    WorldSnapshot out;
    out.version = version_;
    out.component_sizes.assign(kMaxComponents, 0u);
    out.component_names.resize(kMaxComponents);
    for (u32 k = 0; k < kMaxComponents; ++k) {
      if (!registry_[k].registered) continue;
      out.component_sizes[k] = registry_[k].size;
      out.component_names[k] = registry_[k].name;
    }

    out.entities.reserve(live_entity_count_);
    for (const Archetype& a : archetypes_) {
      for (std::size_t r = 0; r < a.size(); ++r) {
        EntitySnapshot e;
        e.id = a.ids[r];
        e.mask = a.mask;
        std::size_t total = 0;
        for (std::size_t k = 0; k < a.columns.size(); ++k) total += a.column_sizes[k];
        e.components.resize(total);
        e.size_bytes.reserve(a.columns.size());
        std::size_t off = 0;
        for (std::size_t k = 0; k < a.columns.size(); ++k) {
          const u32 w = a.column_sizes[k];
          std::memcpy(e.components.data() + off, a.column_row(k, r), w);
          off += w;
          e.size_bytes.push_back(w);
        }
        out.entities.push_back(std::move(e));
      }
    }
    // 按 {index, gen} 排序：archetype 的遍历顺序由迁移历史决定，不稳定；
    // 快照进命令流之前必须规范顺序（与 L2 的 SceneSnapshot 按 id 升序同口径）。
    std::sort(out.entities.begin(), out.entities.end(),
              [](const EntitySnapshot& x, const EntitySnapshot& y) {
                if (x.id.index != y.id.index) return x.id.index < y.id.index;
                return x.id.gen < y.id.gen;
              });
    return out;
  }

  Status restore(const WorldSnapshot& snap) override {
    Status st = validate_snapshot(snap);  // 全或无的第一步：只校验，不动状态
    if (!st.ok()) return st;
    rebuild_from_snapshot(snap);
    ++version_;
    return Status::Ok_();
  }

  void clear_entities() override {
    if (slots_.empty()) return;
    for (std::size_t a = 0; a < archetypes_.size(); ++a) archetypes_[a].clear_rows();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      slots_[i].live = false;
      slots_[i].archetype = kEmptyArchetype;
      slots_[i].row = 0;
    }
    // index **不进空闲列表**：clear 之后 index 仍不复用（与 L2 的 clear 同口径），
    // 复用会破坏命令流里"同一个 index 指的是哪个实体"的推理。
    free_indices_.clear();
    live_entity_count_ = 0;
    dirty_.clear();
    std::fill(dirty_index_.begin(), dirty_index_.end(), kNoDirty);
    for (NodeSlot* ns : node_slots_) {
      if (ns != nullptr) ns->valid = false;
    }
    ++version_;
  }

  // ------------------------------------------------------------ 内存 / 统计

  void set_frame_arena(IFrameArena* arena) override { scratch_.set_arena(arena); }
  void begin_frame() override { scratch_.new_frame(); }
  u64 frame_bytes_used() const override { return scratch_.frame_bytes_used(); }
  void set_node_pool(IObjectPool<NodeSlot>* pool) override { node_pool_ = pool; }
  void set_stats(IFrameStats* stats) override { frame_stats_ = stats; }

 private:
  static constexpr u32 kMaxComponentBytes = 64u * 1024u;
  // dirty_index_ 的哨兵：不在脏集合里。
  static constexpr u32 kNoDirty = 0xFFFFFFFFu;

  struct Registration {
    bool registered = false;
    u32 size = 0;
    std::string name;
    // 该组件的"默认值"字节（长度 <= kMaxComponentBytes）。add_component
    // 新加一列时铺它，而不是铺零 —— 见 migrate 里的说明。
    std::vector<std::uint8_t> default_bytes;
    u32 default_size = 0;
  };

  // ------------------------------------------------------- index → 节点槽位

  NodeSlot* node_slot(u32 index) {
    if (index >= node_slots_.size()) return nullptr;
    return node_slots_[index];
  }

  // 首次需要时为实体取一个节点池槽位。注入池时走 IObjectPool（S 级复用、
  // 地址稳定），没注入时退回 deque（首元素地址稳定，且已经预先 emplace）。
  NodeSlot* ensure_node_slot_value(u32 index) {
    if (index >= node_slots_.size()) node_slots_.resize(index + 1, nullptr);
    if (node_slots_[index] != nullptr) return node_slots_[index];
    NodeSlot* ns;
    if (node_pool_ != nullptr) {
      ns = static_cast<NodeSlot*>(node_pool_->acquire());
      if (ns == nullptr) return nullptr;
    } else {
      fallback_node_slots_.emplace_back();
      ns = &fallback_node_slots_.back();
    }
    *ns = NodeSlot{};
    node_slots_[index] = ns;
    return ns;
  }

  // ------------------------------------------------------- archetype 查找

  // 掩码 → archetype 序号；不存在时按需新建。哈希键就是掩码本身 ——
  // archetype 的种类是"几十到几百"，spawn/add_component 会反复查同一个掩码，
  // 哈希探针比线性扫更稳，也省掉"要不要重排"的心智负担。
  u32 archetype_for(ComponentMask mask) {
    if (mask == 0) return kEmptyArchetype;
    auto it = archetype_lookup_.find(mask);
    if (it != archetype_lookup_.end()) return it->second;

    Archetype a;
    a.mask = mask;
    // 列按位序建好，宽度从注册表取。列的顺序**必须**与 Archetype::slot_of()
    // 一致，否则 get<T>() 会读到别的组件的字节：能编译、能跑、结果全错。
    for (ComponentKind k = 0; k < kMaxComponents; ++k) {
      if ((mask & component_bit(k)) == 0) continue;
      a.columns.emplace_back();
      a.column_sizes.push_back(registry_[k].size);
    }
    const u32 index = static_cast<u32>(archetypes_.size());
    archetypes_.push_back(std::move(a));
    archetype_lookup_.emplace(mask, index);
    visible_cache_.clear();  // 新 archetype 可能匹配任意已知视图掩码
    return index;
  }

  // ------------------------------------------------------- 迁移

  // 把实体从当前 archetype 搬到掩码为 to_mask 的 archetype。
  //
  // 顺序刻意是"先在目标里建好新行，再把整行拷过去，最后删源末行"：
  // 反过来（先删源再建目标）会让"目标列扩容"发生在实体已经离开源之后，
  // 中途失败就两头都不在。这里所有可能分配内存的动作都排在拷贝之前。
  //
  // `target_column_new` 为真的列不参与拷贝，而是填 **该组件注册时给的默认值**。
  //
  // 为什么不是"清零"：对 `TransformComp` 来说全零意味着 scale = (0,0,0)，
  // 实体被缩放成一个点、彻底不可见 —— 一个既不崩也不报错的静默 bug，
  // 只有画出来才发现（这条是被 TransformMatrixRespectsCapacity 抓到的）。
  void migrate(EntityId id, ComponentMask to_mask, ComponentKind new_kind,
               bool target_column_new) {
    EntitySlot& s = slots_[id.index];
    const u32 from_a = s.archetype;
    const std::size_t from_row = s.row;

    // 注意：archetype_for() 可能 push_back 进 archetypes_，于是**先前取到的
    // 任何 Archetype& 都会悬空**（vector 扩容会把元素搬走）。所以这里先做
    // "可能新增 archetype"的动作，再取引用 —— 曾经直接在前面取 `from`，
    // 结果第一次迁移就踩了 use-after-realloc 的访问冲突。
    const u32 to_a = archetype_for(to_mask);
    if (to_a == from_a) return;
    Archetype& from = archetypes_[from_a];
    Archetype& to = archetypes_[to_a];
    const std::size_t to_row = to.push_row();
    to.ids[to_row] = id;

    // 两个 archetype 的交集列：位序保证"同一组件种类在两边是同一族"，但
    // **序号 k 不同**（目标少/多一列，后面的列全往后挪一格）。所以目标列 k
    // 要先反推出组件种类，再去源里查它自己的序号。
    for (std::size_t k = 0; k < to.columns.size(); ++k) {
      const ComponentKind kind = nth_set_kind(to.mask, k);
      if (target_column_new && kind == new_kind) {
        // 新加的列：铺默认值（注册时给的字节），没有则全零。
        const Registration& reg = registry_[kind];
        if (reg.default_size > 0) {
          std::memcpy(to.column_row(k, to_row), reg.default_bytes.data(), reg.default_size);
        }
        continue;
      }
      const std::size_t src_k = from.slot_of(kind);
      if (src_k == kMaxComponents) continue;  // 源里没有这一列
      std::memcpy(to.column_row(k, to_row), from.column_row(src_k, from_row), to.column_sizes[k]);
    }

    // 源侧 delete：与 despawn 相同的 swap-remove。洞是 **from_row**（本实体那一行）。
    const std::size_t last = from.size() - 1;
    if (from_row != last) {
      const EntityId moved = from.ids[last];
      from.ids[from_row] = moved;
      for (std::size_t k = 0; k < from.columns.size(); ++k) {
        std::memcpy(from.column_row(k, from_row), from.column_row(k, last), from.column_sizes[k]);
      }
      slots_[moved.index].row = static_cast<u32>(from_row);
    }
    from.shrink_to(last);

    s.archetype = to_a;
    s.row = static_cast<u32>(to_row);
  }

  // mask 的第 n 个置位对应的组件种类（n 从 0 数）。
  static ComponentKind nth_set_kind(ComponentMask mask, std::size_t n) {
    for (ComponentKind k = 0; k < kMaxComponents; ++k) {
      if ((mask & component_bit(k)) == 0) continue;
      if (n == 0) return k;
      --n;
    }
    return kInvalidComponentKind;
  }

  // ------------------------------------------------------- 视图填块

  // ComponentView::next() 的回调：把一个 archetype 的整段行打包成一块。
  //
  // 这里刻意**不做逐行拆分**（不按 16/64 行切小块）：§4.2 要的是顺序访存，
  // 一整列连续读完是硬件预取最喜欢的形态；切成小块只是让"块"变小，
  // 访存总量不变却多了循环开销。需要分块做 GPU 上传时，由渲染器按 16-bit
  // 索引边界再去切（§4.1"顶点上传仅脏块"）。
  static bool fill_block(void* world, u32 archetype_index, const ComponentView& v,
                         ComponentBlock* out) {
    const WorldImpl& w = *static_cast<const WorldImpl*>(world);
    if (archetype_index >= w.archetypes_.size()) return false;
    const Archetype& a = w.archetypes_[archetype_index];
    if (a.empty()) return false;  // 空 archetype 不产生块（遍历时应被跳过）

    ComponentViewAccess::reset_columns(const_cast<ComponentView&>(v));
    u32 slot = 0;
    for (u32 kind = 0; kind < kMaxComponents && slot < ComponentViewAccess::max_slots(); ++kind) {
      if ((v.mask() & component_bit(kind)) == 0) continue;
      // 视图掩码里的每一种组件，本 archetype 必然都含有（view 已按掩码筛选），
      // 所以 slot_of 一定命中；列指针按位序排进 columns_[slot]。
      const std::size_t k = a.slot_of(kind);
      if (k == kMaxComponents) continue;
      ComponentViewAccess::write_columns(const_cast<ComponentView&>(v), slot,
                                         a.columns[k].data(), a.column_sizes[k]);
      ++slot;
    }
    out->archetype = archetype_index;
    out->mask = a.mask;
    out->count = static_cast<u32>(a.size());
    out->ids = a.ids.data();
    out->row_offset = 0;
    // 给出视图对象内部那份指针表的地址：块只在本次 next() 之后有效，
    // 下一次 next() 会原地重写这张表（与"视图失效"的契约一致）。
    out->columns = ComponentViewAccess::column_pointers(const_cast<ComponentView&>(v));
    out->column_sizes = ComponentViewAccess::column_size_pointers(const_cast<ComponentView&>(v));
    return true;
  }

  // ------------------------------------------------------- 快照校验 / 重建

  Status validate_snapshot(const WorldSnapshot& snap) const {
    if (snap.component_sizes.size() != kMaxComponents ||
        snap.component_names.size() != kMaxComponents) {
      return Status::Invalid("IWorld::restore: 快照注册表长度与本实现不一致");
    }
    for (u32 k = 0; k < kMaxComponents; ++k) {
      const bool have_reg = is_registered(k);
      const u32 have = have_reg ? registry_[k].size : 0u;
      if (snap.component_sizes[k] != have) {
        return Status::Unsupported(
            "IWorld::restore: 快照的组件注册表与当前世界不同（注册顺序/宽度不一致），拒绝应用");
      }
    }

    for (std::size_t i = 0; i < snap.entities.size(); ++i) {
      const EntitySnapshot& e = snap.entities[i];
      if (!e.id.valid() || e.id.index >= EntityId::kGenIndexMask) {
        return Status::Invalid("IWorld::restore: 快照里有无效 EntityId");
      }
      // 按注册表重算列宽序列，必须与快照里的 size_bytes 逐项相等 ——
      // 这是快照"自校验"，抓的是另一次 restore 改过注册表之类的损坏。
      std::size_t expected = 0;
      std::size_t ncols = 0;
      for (ComponentKind k = 0; k < kMaxComponents; ++k) {
        if ((e.mask & component_bit(k)) == 0) continue;
        if (!is_registered(k)) {
          return Status::Unsupported("IWorld::restore: 快照用到了本世界未注册的组件");
        }
        if (ncols >= e.size_bytes.size() || e.size_bytes[ncols] != registry_[k].size) {
          return Status::Invalid("IWorld::restore: 组件列宽与注册表不符（快照已损坏）");
        }
        expected += registry_[k].size;
        ++ncols;
      }
      if (ncols != e.size_bytes.size() || expected != e.components.size()) {
        return Status::Invalid("IWorld::restore: 组件字节数与掩码不符（快照已损坏）");
      }
      // index 重复：两个句柄会指向同一行数据，本该是同一个实体。
      // 快照已按 index 排序（snapshot 产的），相邻比较即可；对乱序输入
      // 只做尽力检测，主要防线在 restore 后的"行数 == 快照条数"。
      if (i > 0 && snap.entities[i - 1].id.index == e.id.index) {
        return Status::Invalid("IWorld::restore: 快照里 EntityId.index 重复");
      }
    }
    return Status::Ok_();
  }

  void rebuild_from_snapshot(const WorldSnapshot& snap) {
    // 复位到"空世界"：注册表保留（已校验一致），archetype 只留空的 0 号。
    archetypes_.resize(1);
    archetypes_[kEmptyArchetype].mask = 0;
    archetypes_[kEmptyArchetype].clear_rows();
    archetype_lookup_.clear();
    visible_cache_.clear();
    slots_.clear();
    free_indices_.clear();
    dirty_.clear();
    dirty_index_.clear();
    if (node_pool_ != nullptr) {
      for (NodeSlot* ns : node_slots_) {
        if (ns != nullptr) node_pool_->release(ns);
      }
    }
    node_slots_.clear();
    fallback_node_slots_.clear();

    // 快照里的 index 是**稀疏**的（可能缺号），按最大 index 开槽位；
    // 缺的号留成 !live 且**不进空闲列表**：它们从未被发出过，不该被复用，
    // 否则 restore 出来的 index 序列与源世界不同，命令流哈希就对不上。
    u32 max_index = 0;
    bool any = false;
    for (const EntitySnapshot& e : snap.entities) {
      max_index = any ? std::max(max_index, e.id.index) : e.id.index;
      any = true;
    }
    if (any) {
      slots_.assign(static_cast<std::size_t>(max_index) + 1, EntitySlot{});
      for (const EntitySnapshot& e : snap.entities) {
        EntitySlot& s = slots_[e.id.index];
        // **gen 从快照精确还原**，不做任何"代际未知"的猜测。
        //
        // 早先的实现把这里填成 kExhaustedGen 并给该 index 关掉窄化旁路，
        // 让 alive() 只看"这个 index 上有没有活实体"。那是错的：那样一来
        // restore **之前**的任何旧句柄（哪怕 gen 完全不同）都会被判存活，
        // 于是"删掉的物体在 restore 后自己活过来"。快照本来就把 gen 存在
        // EntitySnapshot::id 里（snapshot() 写的是列里的真实 EntityId），
        // 直接用它即可 —— 恢复出的世界与原世界的句柄语义完全一致。
        s.gen = e.id.gen;
        s.live = true;
        ensure_node_slot_value(e.id.index);
      }
    }
    dirty_index_.assign(slots_.size(), kNoDirty);

    for (const EntitySnapshot& e : snap.entities) {
      const u32 a = archetype_for(e.mask);
      const std::size_t row = archetypes_[a].push_row();
      Archetype& arch = archetypes_[a];
      arch.ids[row] = e.id;
      // 组件字节整体拷过去：快照布局就是"列按位序拼接"，与目标 archetype 的
      // 列布局完全一致（新 archetype 的列顺序由同一套位序规则产生）。
      std::size_t off = 0;
      for (std::size_t k = 0; k < arch.columns.size(); ++k) {
        std::memcpy(arch.column_row(k, row), e.components.data() + off, arch.column_sizes[k]);
        off += arch.column_sizes[k];
      }
      slots_[e.id.index].archetype = a;
      slots_[e.id.index].row = static_cast<u32>(row);
    }
    live_entity_count_ = static_cast<u32>(snap.entities.size());
  }

  // ------------------------------------------------------- 成员

  std::array<Registration, kMaxComponents> registry_{};
  u32 registered_count_ = 0;
  u32 live_entity_count_ = 0;
  u64 dirty_mark_count_ = 0;

  std::vector<Archetype> archetypes_;
  std::unordered_map<ComponentMask, u32> archetype_lookup_;
  std::unordered_map<ComponentMask, std::vector<u32>> visible_cache_;

  std::vector<EntitySlot> slots_;
  std::vector<u32> free_indices_;

  DirtySet dirty_;
  std::vector<u32> dirty_index_;  // index → dirty 集合内的位置（kNoDirty = 不在）

  std::vector<NodeSlot*> node_slots_;
  std::deque<NodeSlot> fallback_node_slots_;
  IObjectPool<NodeSlot>* node_pool_ = nullptr;

  mutable Scratch scratch_;
  IFrameStats* frame_stats_ = nullptr;

  u64 version_ = 0;
};

// ================================================================ 视图迭代（必须在匿名命名空间外）

// ComponentView 定义在公开头里，所以它的成员函数只能定义在**外部可见**的位置
// （C2888：不能在匿名命名空间里定义外部可见类型的成员）。这里它只通过
// filler_ 回调回到 WorldImpl，不暴露任何实现细节。
//
// next() 只负责推进游标与跳过空 archetype；真正的填块由 `filler_` 完成。
// 空 archetype 不产生块：如果产出 count==0 的块，调用方的 `while (v.next(&b))`
// 就会多转一圈、并在断言 `b.count > 0` 上失败 —— 这个空转而只服务内部状态。
bool ComponentView::next(ComponentBlock* block) {
  if (block == nullptr || filler_ == nullptr) return false;
  while (cursor_ < total_) {
    const u32 archetype = archetypes_[cursor_];
    ++cursor_;
    if (filler_(world_, archetype, *this, block)) return true;
  }
  return false;
}

// ================================================================ 工厂 / 标准组件

std::unique_ptr<IWorld> create_world() { return std::make_unique<WorldImpl>(); }

namespace {

// 标准组件的"登记表"：kind → (宽度, 名字, 默认值)。三处一起改（kComponentKind、
// kStandardComponentCount、这里），故意不自动化 —— "多加一个标准组件"
// 是设计评审的事，不是顺手加一行的事。
//
// 默认值以 `T{}` 的地址给出：组件的默认值是**类型定义的一部分**，两边必须
// 是同一个来源，否则"默认 Transform 缩放是多少"会有两个答案。
struct StandardEntry {
  ComponentKind kind;
  u32 size;
  const char* name;
  const void* default_bytes;
};

}  // namespace

Status register_standard_components(IWorld& world) {
  // 每个标准组件的默认值就是它的 `T{}`。用函数内的 static 常量取地址：
  // 生命周期贯穿整个程序，且不会在每次调用时重建。
  static const TransformComp kDefaultTransform{};
  static const LocalBoundsComp kDefaultBounds{};
  static const MeshRefComp kDefaultMeshRef{};
  static const MaterialRefComp kDefaultMaterialRef{};
  static const StrokeComp kDefaultStroke{};
  static const LayerComp kDefaultLayer{};
  static const VisibleComp kDefaultVisible{};
  static const GizmoTargetComp kDefaultGizmo{};

  const StandardEntry entries[] = {
      {TransformComp::kComponentKind, static_cast<u32>(sizeof(TransformComp)), "Transform",
       &kDefaultTransform},
      {LocalBoundsComp::kComponentKind, static_cast<u32>(sizeof(LocalBoundsComp)), "LocalBounds",
       &kDefaultBounds},
      {MeshRefComp::kComponentKind, static_cast<u32>(sizeof(MeshRefComp)), "MeshRef",
       &kDefaultMeshRef},
      {MaterialRefComp::kComponentKind, static_cast<u32>(sizeof(MaterialRefComp)), "MaterialRef",
       &kDefaultMaterialRef},
      {StrokeComp::kComponentKind, static_cast<u32>(sizeof(StrokeComp)), "Stroke",
       &kDefaultStroke},
      {LayerComp::kComponentKind, static_cast<u32>(sizeof(LayerComp)), "Layer", &kDefaultLayer},
      {VisibleComp::kComponentKind, static_cast<u32>(sizeof(VisibleComp)), "Visible",
       &kDefaultVisible},
      {GizmoTargetComp::kComponentKind, static_cast<u32>(sizeof(GizmoTargetComp)), "GizmoTarget",
       &kDefaultGizmo},
  };
  static_assert(sizeof(entries) / sizeof(entries[0]) == kStandardComponentCount,
                "标准组件数量与 kStandardComponentCount 不一致");
  for (const StandardEntry& e : entries) {
    Status st = world.register_component(e.kind, e.size, e.name, e.default_bytes);
    if (!st.ok()) return st;  // 第一个失败就返回：注册是启动期动作，失败就该当场停下
  }
  return Status::Ok_();
}

}  // namespace magicblocks::primitives
