// MagicBlocks L3 —— 图元渲染器实现
//
// 这个文件是 **L3 唯一会调用 ICommandList 的地方**，所以两个纪律必须守住：
//   1. 只调录制接口，绝不 submit（一帧一次提交由调用方负责，UIG2 门控）；
//   2. 不读时钟、不分配大块内存（帧内数组用成员缓冲复用）。
#include "magicblocks/primitives/primitive_renderer.h"

#include <cstring>
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

  const RenderStats& stats() const override { return stats_; }
  void reset_stats() override { stats_ = RenderStats{}; }

 private:
  std::unordered_map<MeshId, const IMesh*> meshes_;
  std::unordered_map<u64, resources::IPipeline*> pipelines_;
  resources::IBuffer* instance_buffer_ = nullptr;
  resources::IPipeline* last_pipeline_ = nullptr;
  RenderStats stats_{};
};

}  // namespace

std::unique_ptr<IPrimitiveRenderer> create_primitive_renderer() {
  return std::make_unique<PrimitiveRenderer>();
}

}  // namespace magicblocks::primitives
