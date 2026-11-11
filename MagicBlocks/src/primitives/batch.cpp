// MagicBlocks L3 —— 合批器实现（spec §4.3）
//
// 排序 → 相邻合并 → 产出 BatchDraw。全部在预分配好的缓冲区上做，
// 稳态帧不做堆分配（§4.1）。
#include "mb_internal/magicblocks/primitives/batch.h"

#include <algorithm>
#include <cstring>

namespace magicblocks::primitives {
namespace {

// 哈希混合：把 (pipeline, material, mesh, entity) 压成一个 64 位指纹。
// 用于 order_hash —— 回放时两次合批若顺序不同，哈希就不同，能立刻发现。
inline std::uint64_t mix64(std::uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

// ---------------------------------------------------------------- 排序键
//
// spec §4.3 的键序（升序）：
//   [0] pipeline_hash      管线切换多少由它决定 → 必须最前
//   [1] material_key       同管线下换材质的次数
//   [2] mesh_id            几何缓冲切换
//   [3] transparency       不透明先画、透明后画（透明内部按距离降序，L3 简化：
//                          只保证透明在后，距离排序留给需要正确混合的调用方）
//   [4] entity_id          稳定 tie-break，保证回放确定性
//
// 为什么把 pipeline 放最前：
//   §4.1 要求"管线切换 ≤ 16 次/帧"。只要排序键的第一位是 pipeline，
//   同管线的绘制必然连续，切换次数 == 不同 pipeline 的**种类数**，
//   这是理论下界，不可能更少。
inline bool less_key(const DrawRequest& a, const DrawRequest& b) {
  if (a.pipeline_hash != b.pipeline_hash) return a.pipeline_hash < b.pipeline_hash;
  if (a.material_key != b.material_key) return a.material_key < b.material_key;
  if (a.mesh != b.mesh) return a.mesh < b.mesh;
  if (a.transparent != b.transparent) return !a.transparent;  // false(不透明) 在前
  return a.entity_id < b.entity_id;
}

// 两条请求能否合并进同一个 draw。
// 注意"透明"不是独立字段而由 ShaderKey 体现（D-L3-3），这里仍比较它是为了
// 防御：万一调用方给了 (相同 pipeline 但 transparent 不同) 的矛盾输入，
// 我们宁可多一次 drawcall 也不把不该混合的实例合到一起。
inline bool mergeable(const DrawRequest& base, const DrawRequest& other) {
  return base.pipeline_hash == other.pipeline_hash &&
         base.material_key == other.material_key &&
         base.mesh == other.mesh &&
         base.transparent == other.transparent &&
         base.index_count == other.index_count &&
         base.first_index == other.first_index &&
         base.vertex_offset == other.vertex_offset &&
         base.vertex_count == other.vertex_count;
}

class BatchBuilder final : public IBatchBuilder {
 public:
  void begin_frame(u32 frame_slot) override {
    frame_slot_ = frame_slot;
    requests_.clear();
    batches_.clear();
    order_hash_ = 0;
    stats_ = BatchStats{};
    inst_scratch_.clear();
    // 注意：不清 capacity。稳态帧靠"容量复用"做到 0 分配。
  }

  void push(const DrawRequest& req) override {
    if (!req.visible) {
      // 不可见的不进排序（省一次排序开销），但计数便于诊断。
      ++stats_.skipped_invisible;
      return;
    }
    requests_.push_back(req);
  }

  void push_batch(const DrawRequest* reqs, std::size_t count) override {
    if (reqs == nullptr) return;
    requests_.reserve(requests_.size() + count);
    for (std::size_t i = 0; i < count; ++i) push(reqs[i]);
  }

  void build() override {
    stats_.request_count = static_cast<std::uint32_t>(requests_.size());
    batches_.clear();
    order_hash_ = 0;
    if (requests_.empty()) {
      stats_.batch_count = 0;
      stats_.pipeline_switches = 0;
      stats_.merged_requests = 0;
      stats_.instance_bytes = 0;
      return;
    }

    // ---- 1. 稳定排序 ----
    // 用 std::stable_sort：相同键的输入保持相对顺序，
    // 这样相同的输入序列必然产出相同的输出序列（回放确定性）。
    std::stable_sort(requests_.begin(), requests_.end(), less_key);

    // ---- 2. 相邻合并 ----
    // 排序后凡 mergeable 的必然相邻，扫一遍即可，O(n)。
    std::size_t i = 0;
    while (i < requests_.size()) {
      const DrawRequest& first = requests_[i];
      BatchDraw batch;
      batch.pipeline_hash = first.pipeline_hash;
      batch.material_key = first.material_key;
      batch.mesh = first.mesh;
      batch.index_count = first.index_count;
      batch.first_index = first.first_index;
      batch.vertex_offset = first.vertex_offset;
      batch.vertex_count = first.vertex_count;
      batch.indexed = first.index_count > 0;
      batch.transparent = first.transparent;
      batch.first_entity = first.entity_id;
      batch.instance_count = 1;

      std::size_t j = i + 1;
      while (j < requests_.size() && mergeable(first, requests_[j])) {
        ++batch.instance_count;
        ++j;
      }
      stats_.merged_requests += static_cast<std::uint32_t>(batch.instance_count - 1);

      // ---- 3. 实例属性写出 ----
      // 单实例批次不需要实例属性（用 push constant / 默认属性即可），
      // 但为了语义一致，只要超过 1 个实例就必须写实例缓冲。
      if (batch.instance_count > 1) {
        write_instances(batch);
      } else {
        batch.instance_offset = 0;
      }

      batches_.push_back(batch);
      // 必须跳到 j（本批的结束位置），而不是 ++i。
      // 之前写成 ++i 是个隐蔽的 bug：合并循环已经把 [i+1, j) 都算进这一批了，
      // 但外层只前进 1，于是那些请求会被**再处理一遍**，产生重复的 batch
      // （100 条同键请求 → 100 个 batch + merged_requests=4950 的怪数字）。
      i = j;
    }

    // ---- 4. 统计 ----
    stats_.batch_count = static_cast<std::uint32_t>(batches_.size());
    std::uint32_t switches = 0;
    for (std::size_t k = 1; k < batches_.size(); ++k) {
      if (batches_[k].pipeline_hash != batches_[k - 1].pipeline_hash) ++switches;
    }
    stats_.pipeline_switches = switches;

    // ---- 5. 顺序指纹 ----
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
    for (const BatchDraw& b : batches_) {
      h = mix64(h ^ b.pipeline_hash);
      h = mix64(h ^ static_cast<std::uint64_t>(b.material_key));
      h = mix64(h ^ static_cast<std::uint64_t>(b.mesh));
      h = mix64(h ^ b.instance_count);
      h = mix64(h ^ b.first_entity);
    }
    order_hash_ = h;

    if (frame_stats_ != nullptr) {
      frame_stats_->add_drawcall(stats_.batch_count);
      frame_stats_->add_pipeline_switch(stats_.pipeline_switches);
      frame_stats_->add_instances_uploaded(stats_.instance_bytes / sizeof(InstanceAttribs));
    }
  }

  const std::vector<BatchDraw>& batches() const override { return batches_; }
  const BatchStats& stats() const override { return stats_; }
  std::uint64_t order_hash() const override { return order_hash_; }

  void reserve(std::size_t max_requests) override {
    requests_.reserve(max_requests);
    batches_.reserve(max_requests / 2 + 1);
    inst_scratch_.reserve(max_requests);
  }

  void set_allow_growth(bool allow) override { allow_growth_ = allow; }

  void set_frame_arena(IFrameArena* arena) override { arena_ = arena; }
  void set_batch_pool(IBatchPool* pool) override { pool_ = pool; }
  void set_stats(IFrameStats* stats) override { frame_stats_ = stats; }

  void set_instance_writer(
      bool (*write)(void* user, const InstanceAttribs* data, u32 count, u32* out_offset),
      void* user) override {
    writer_ = write;
    writer_user_ = user;
  }

 private:
  // 把 [i, i+instance_count) 的请求转成 InstanceAttribs 并交给 writer。
  //
  // 这里**不复用外部指针**：InstanceAttribs 是值，模型矩阵在提交时就该算好
  // （由调用方 / IWorld 的 system 填进 DrawRequest 关联的数组）。
  // L3 的第一版先只统计字节数，真实矩阵填充由 IPrimitiveRenderer 接线时完成。
  void write_instances(BatchDraw& batch) {
    const std::size_t count = batch.instance_count;
    inst_scratch_.resize(count);
    // 注意：inst_scratch_ 的 resize 在稳态下不分配（容量已在 reserve 里给足）。
    std::memset(inst_scratch_.data(), 0, count * sizeof(InstanceAttribs));
    stats_.instance_bytes += static_cast<std::uint32_t>(count * sizeof(InstanceAttribs));

    if (writer_ != nullptr) {
      u32 off = 0;
      if (writer_(writer_user_, inst_scratch_.data(), static_cast<u32>(count), &off)) {
        batch.instance_offset = off;
      }
    }
  }

  std::vector<DrawRequest> requests_;
  std::vector<BatchDraw> batches_;
  std::vector<InstanceAttribs> inst_scratch_;
  BatchStats stats_{};
  std::uint64_t order_hash_ = 0;
  u32 frame_slot_ = 0;
  bool allow_growth_ = true;
  IFrameArena* arena_ = nullptr;
  IBatchPool* pool_ = nullptr;
  // 注意:上面已有一个 BatchStats values_ 叫 stats_,所以帧统计指针必须改名,
  // 不能也叫 stats_(否则与值类型成员同名冲突,MSVC 报 C2040)。
  IFrameStats* frame_stats_ = nullptr;
  bool (*writer_)(void*, const InstanceAttribs*, u32, u32*) = nullptr;
  void* writer_user_ = nullptr;
};

}  // namespace

std::unique_ptr<IBatchBuilder> create_batch_builder() {
  return std::make_unique<BatchBuilder>();
}

}  // namespace magicblocks::primitives
