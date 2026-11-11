// MagicBlocks L3 —— 图元渲染器实现
//
// 这个文件是 **L3 唯一会调用 ICommandList 的地方**，所以两个纪律必须守住：
//   1. 只调录制接口，绝不 submit（一帧一次提交由调用方负责，UIG2 门控）；
//   2. 不读时钟、不分配大块内存（帧内数组用成员缓冲复用）。
#include "mb_internal/magicblocks/primitives/primitive_renderer.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

namespace magicblocks::primitives {
namespace {

class PrimitiveRenderer final : public IPrimitiveRenderer {
 public:
  bool register_mesh(const IMesh* mesh) override {
    if (mesh == nullptr) return false;
    const MeshId id = mesh->id();
    if (id == kInvalidMeshId) return false;
    // 同一 MeshId 不能对应两个网格：合批按 MeshId 排序，
    // 两个网格争用同一个键会导致"画出来的几何不是排到的那个"。
    auto it = meshes_.find(id);
    if (it != meshes_.end() && it->second != mesh) return false;
    meshes_[id] = mesh;
    return true;
  }

  void unregister_mesh(MeshId id) override { meshes_.erase(id); }

  const IMesh* find_mesh(MeshId id) const override {
    auto it = meshes_.find(id);
    return (it == meshes_.end()) ? nullptr : it->second;
  }

  bool register_pipeline(u64 pipeline_key, resources::IPipeline* pipeline) override {
    if (pipeline == nullptr) return false;
    if (pipeline_key == kUnresolvedPipelineHash) return false;
    pipelines_[pipeline_key] = pipeline;
    return true;
  }

  resources::IPipeline* find_pipeline(u64 pipeline_key) const override {
    auto it = pipelines_.find(pipeline_key);
    return (it == pipelines_.end()) ? nullptr : it->second;
  }

  void set_instance_buffer(resources::IBuffer* buffer) override { instance_buffer_ = buffer; }

  Status record_frame(resources::ICommandList& cmd, const IBatchBuilder& batches,
                      const PrimitiveFrameDesc& desc) override {
    stats_ = RenderStats{};

    // 实例属性上传路径：让合批器把 InstanceAttribs 写进 batch pool。
    // 这里在 build 之后接线太晚了 —— 所以调用方应当已经 set_instance_writer。
    // 本函数只消费 batches() 的结果。

    std::uint32_t drawn = 0;
    for (const BatchDraw& b : batches.batches()) {
      // 透明 pass 开关：如果 desc 说不要透明 pass，跳过透明批次。
      if (b.transparent && !desc.draw_transparent_pass) continue;

      const IMesh* mesh = find_mesh(b.mesh);
      if (mesh == nullptr) {
        // 网格没注册是接线错误，但一帧里可能有成百上千个这样的批次，
        // 每个都返回错误会淹没真正的失败原因。记状态，继续画其余的，
        // 由调用方在帧末检查 stats。
        ++stats_.missing_meshes;
        stats_.last_status = Status::Invalid("renderer: 批次引用了未注册的网格");
        continue;
      }

      resources::IPipeline* pipeline = find_pipeline(b.pipeline_hash);
      if (pipeline == nullptr) {
        ++stats_.missing_pipelines;
        stats_.last_status = Status::Invalid(
            "renderer: 批次 pipeline_hash 未注册（注意 kUnresolvedPipelineHash 必须先改写）");
        continue;
      }

      // 绑定管线（同管线连续时跳过重复绑定 —— 合批已经保证同管线相邻，
      // 这里再省一次 bind 的录制开销）
      if (pipeline != last_pipeline_) {
        Status s = cmd.bind_pipeline(pipeline, b.pipeline_hash);
        if (!s.ok()) return s;
        last_pipeline_ = pipeline;
        ++stats_.pipeline_binds;
      }

      // 绑定顶点/索引缓冲
      const resources::IBuffer* vb = mesh->vertex_buffer();
      const resources::IBuffer* ib = mesh->index_buffer();
      if (vb == nullptr || ib == nullptr) {
        ++stats_.missing_meshes;
        continue;
      }

      Status s = cmd.bind_vertex_buffer(vb->id(), 0, 0);
      if (!s.ok()) return s;
      ++stats_.vertex_binds;

      // 实例属性走 binding 1
      if (instance_buffer_ != nullptr && b.instance_count > 1) {
        Status si = cmd.bind_vertex_buffer(instance_buffer_->id(), 1, b.instance_offset);
        if (!si.ok()) return si;
        ++stats_.vertex_binds;
      }

      s = cmd.bind_index_buffer(ib->id(), resources::IndexFormat::UInt32, 0);
      if (!s.ok()) return s;
      ++stats_.index_binds;

      resources::DrawArgs args;
      args.index_count = b.index_count;
      args.first_index = b.first_index;
      args.vertex_offset = b.vertex_offset;
      args.vertex_count = b.vertex_count;
      args.instance_count = (b.instance_count == 0) ? 1 : b.instance_count;
      args.first_instance = b.instance_offset / static_cast<u32>(sizeof(InstanceAttribs));

      s = cmd.draw_indexed(args);
      if (!s.ok()) return s;
      ++drawn;
    }

    stats_.scene_draws = drawn;
    return stats_.last_status;
  }

  // ------------------------------------------------------------ 实例化绘制（R-SK-11 / A11）

  Status draw_mesh_instanced(resources::ICommandList& cmd, u64 pipeline_key, const IMesh* mesh,
                             const InstanceAttribs* instances, u32 instance_count) override {
    if (mesh == nullptr) {
      return Status::Invalid("draw_mesh_instanced: mesh 为空");
    }
    if (instance_count == 0) {
      return Status::Ok_();  // 空绘制是合法的（与 record_frame 的空批次同口径）
    }
    if (instances == nullptr) {
      return Status::Invalid("draw_mesh_instanced: instance_count=" +
                             std::to_string(instance_count) + " 但 instances 为空");
    }
    resources::IPipeline* pipeline = find_pipeline(pipeline_key);
    if (pipeline == nullptr) {
      return Status::Invalid("draw_mesh_instanced: pipeline_key 未注册（kUnresolvedPipelineHash "
                             "必须先由调用方解析成真实管线键）");
    }
    // 网格要么已注册，要么就是这次传进来的这个。两者都没有 = 接线错误。
    if (find_mesh(mesh->id()) == nullptr) {
      return Status::Invalid("draw_mesh_instanced: mesh id 未注册（先 register_mesh）");
    }
    const resources::IBuffer* vb = mesh->vertex_buffer();
    const resources::IBuffer* ib = mesh->index_buffer();
    if (vb == nullptr || ib == nullptr) {
      return Status::Invalid("draw_mesh_instanced: mesh 没有顶点/索引缓冲");
    }

    // ---- 1) 实例缓冲：容量不足才扩容，稳态帧零分配 ----
    Status gs = ensure_instance_buffer(instance_count);
    if (!gs.ok()) return gs;

    // ---- 2) 写实例数据（Host 可见桶 + write()，不新建资源、不 flush）----
    const u64 bytes = static_cast<u64>(instance_count) * sizeof(InstanceAttribs);
    Status ws = instance_buffer_res_->write(instances, 0, bytes);
    if (!ws.ok()) {
      return Status(ws.code, std::string("draw_mesh_instanced: 写实例缓冲失败: ") + ws.message);
    }

    // ---- 3) 录制（只绑定，不 submit）----
    Status s = cmd.bind_pipeline(pipeline, pipeline_key);
    if (!s.ok()) {
      return Status(s.code, std::string("draw_mesh_instanced: bind_pipeline: ") + s.message);
    }
    ++stats_.pipeline_binds;

    s = cmd.bind_vertex_buffer(vb->id(), 0, 0);
    if (!s.ok()) return s;
    ++stats_.vertex_binds;

    s = cmd.bind_vertex_buffer(instance_buffer_res_->id(), 1, 0);
    if (!s.ok()) return s;
    ++stats_.vertex_binds;

    s = cmd.bind_index_buffer(ib->id(), resources::IndexFormat::UInt32, 0);
    if (!s.ok()) return s;
    ++stats_.index_binds;

    resources::DrawArgs args;
    args.index_count = mesh->index_count();
    args.instance_count = instance_count;
    s = cmd.draw_indexed(args);
    if (!s.ok()) return s;
    ++stats_.scene_draws;
    return Status::Ok_();
  }

  void set_instance_device(device::IRenderDevice* device) override { instance_device_ = device; }

  u32 instance_capacity() const override { return instance_capacity_; }

  const RenderStats& stats() const override { return stats_; }
  void reset_stats() override { stats_ = RenderStats{}; }

 private:
  // 实例缓冲的懒创建 + 2 倍扩容。
  //
  // 为什么用 BufferUsage::Staging：`memory_kind_of(Staging)` = MemoryKind::Staging
  // → PageKind::HostVisible（MAPPED 常驻），于是 `write()` 可用（设备页桶的
  // write() 会明确返回 Unsupported）。这条路径**不触发 upload/flush**，
  // 也就没有每帧的 staging→GPU 拷贝。
  //
  // 为什么用 create_buffer 而不是自己 allocate()：L1 的规矩是
  // 「allocate() 从不自己触发 VMA，必须先 reserve_pages」——`create_buffer`
  // 走的正是这条：它在已预取的页里切子块。**稳态帧不会走到这里**，
  // 因为容量一旦够用就不再进入 if。新增的 every-frame create_buffer 会
  // 让 `instance_buffer_grows` 与分配器计数同时上涨。
  Status ensure_instance_buffer(u32 needed) {
    if (instance_buffer_res_ != nullptr && instance_capacity_ >= needed) {
      return Status::Ok_();
    }
    // 2 倍扩容（至少 kInitialCapacity 条），保证增长是摊还 O(1)。
    u32 new_cap = instance_capacity_ == 0 ? kInitialCapacity : instance_capacity_;
    while (new_cap < needed) new_cap *= 2;

    resources::BufferDesc bd;
    bd.size = static_cast<u64>(new_cap) * sizeof(InstanceAttribs);
    bd.usage = resources::BufferUsage::Staging;
    bd.persistent_map = true;  // 明确要常驻映射：落错桶时必须报错而不是给个不可写的缓冲

    // 注意：这里需要 device 才能创建缓冲。device 由 set_instance_device() 注入，
    // 不注入就明确报错 —— 绝不"先记下来以后再说"。
    if (instance_device_ == nullptr) {
      return Status::Invalid(
          "draw_mesh_instanced: 未注入设备（先调 set_instance_device(IRenderDevice*)，"
          "实例缓冲必须走 L1 的 create_buffer，不允许旁路分配）");
    }
    auto r = instance_device_->create_buffer(bd);
    if (!r.ok()) {
      return Status(r.status().code, "draw_mesh_instanced: 创建实例缓冲失败（容量 " +
                                         std::to_string(new_cap) + " 条, " +
                                         std::to_string(bd.size) + " 字节）: " +
                                         r.status().message);
    }
    instance_owned_buffer_ = std::move(r.value());
    instance_buffer_res_ = instance_owned_buffer_.get();
    instance_capacity_ = new_cap;
    ++stats_.instance_buffer_grows;
    return Status::Ok_();
  }

  std::unordered_map<MeshId, const IMesh*> meshes_;
  std::unordered_map<u64, resources::IPipeline*> pipelines_;
  resources::IBuffer* instance_buffer_ = nullptr;
  resources::IPipeline* last_pipeline_ = nullptr;

  // 实例化绘制的复用缓冲（R-SK-11）。
  static constexpr u32 kInitialCapacity = 64;
  device::IRenderDevice* instance_device_ = nullptr;
  std::unique_ptr<resources::IBuffer> instance_owned_buffer_;
  resources::IBuffer* instance_buffer_res_ = nullptr;
  u32 instance_capacity_ = 0;
  RenderStats stats_{};
};

}  // namespace

std::unique_ptr<IPrimitiveRenderer> create_primitive_renderer() {
  return std::make_unique<PrimitiveRenderer>();
}

}  // namespace magicblocks::primitives
