// MagicBlocks L3 —— 网格实现
#include "mb_internal/magicblocks/primitives/mesh.h"

#include <cstring>

#include "mb_internal/magicblocks/resources/pipeline.h"

namespace magicblocks::primitives {

VertexLayout mesh_vertex_layout() {
  VertexLayout layout;

  // binding 0：每顶点，stride = sizeof(Vertex) = 48
  resources::VertexInputBinding vert_binding;
  vert_binding.binding = 0;
  vert_binding.stride = static_cast<u32>(sizeof(Vertex));
  vert_binding.instanced = false;
  layout.bindings.push_back(vert_binding);

  // location 必须从 0 连续，不能跳号（L1 明确禁止）。
  resources::VertexAttribute a{};
  a.binding = 0;

  a.location = 0;                       // position
  a.format = resources::VertexFormat::Float3;
  a.offset = static_cast<u32>(offsetof(Vertex, position));
  layout.attributes.push_back(a);

  a.location = 1;                       // normal
  a.format = resources::VertexFormat::Float3;
  a.offset = static_cast<u32>(offsetof(Vertex, normal));
  layout.attributes.push_back(a);

  a.location = 2;                       // color (RGBA8 归一化)
  a.format = resources::VertexFormat::UByte4Norm;
  a.offset = static_cast<u32>(offsetof(Vertex, color));
  layout.attributes.push_back(a);

  a.location = 3;                       // uv
  a.format = resources::VertexFormat::Float2;
  a.offset = static_cast<u32>(offsetof(Vertex, uv));
  layout.attributes.push_back(a);

  return layout;
}

namespace {

class MeshImpl final : public IMesh {
 public:
  MeshImpl(resources::IBuffer* vb, resources::IBuffer* ib, MeshId id, const MeshData& data)
      : vertex_buffer_(vb),
        index_buffer_(ib),
        id_(id),
        vertex_count_(static_cast<u32>(data.vertices.size())),
        index_count_(static_cast<u32>(data.indices.size())),
        bounds_(data.bounds) {}

  MeshId id() const override { return id_; }
  const resources::IBuffer* vertex_buffer() const override { return vertex_buffer_; }
  const resources::IBuffer* index_buffer() const override { return index_buffer_; }
  u32 vertex_count() const override { return vertex_count_; }
  u32 index_count() const override { return index_count_; }
  const Box& bounds() const override { return bounds_; }

  u64 gpu_bytes() const override {
    u64 bytes = 0;
    if (vertex_buffer_ != nullptr) bytes += vertex_buffer_->size();
    if (index_buffer_ != nullptr) bytes += index_buffer_->size();
    return bytes;
  }

 private:
  resources::IBuffer* vertex_buffer_ = nullptr;
  resources::IBuffer* index_buffer_ = nullptr;
  MeshId id_ = kInvalidMeshId;
  u32 vertex_count_ = 0;
  u32 index_count_ = 0;
  Box bounds_{};
};

}  // namespace

std::unique_ptr<IMesh> create_mesh(resources::IBuffer* vertex_buffer,
                                   resources::IBuffer* index_buffer, MeshId id,
                                   const MeshData& data) {
  if (vertex_buffer == nullptr || index_buffer == nullptr) return nullptr;
  if (data.empty()) return nullptr;

  // 上传：write 是 L1 的 CPU→GPU 路径（内部可能走 staging）。
  // 尺寸不匹配说明调用方给的缓冲和 MeshData 对不上，宁可失败也不静默截断。
  const u64 vbytes = static_cast<u64>(data.vertices.size()) * sizeof(Vertex);
  const u64 ibytes = static_cast<u64>(data.indices.size()) * sizeof(std::uint32_t);
  if (vertex_buffer->size() < vbytes || index_buffer->size() < ibytes) return nullptr;

  if (!vertex_buffer->write(data.vertices.data(), 0, vbytes).ok()) return nullptr;
  if (!index_buffer->write(data.indices.data(), 0, ibytes).ok()) return nullptr;

  return std::make_unique<MeshImpl>(vertex_buffer, index_buffer, id, data);
}

}  // namespace magicblocks::primitives
