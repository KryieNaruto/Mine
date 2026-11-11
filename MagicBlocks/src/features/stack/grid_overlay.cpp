// MagicBlocks L4 —— 魔法叠叠乐：网格覆盖层实现
//
// 一律使用公共接口：建管线走 create_pipeline(desc)，收集走 IOverlay2D，
// 录制走 ICommandList 的标准序列。没有任何后端私有头。
#include "mb_internal/magicblocks/features/stack/grid_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "mb_internal/magicblocks/primitives/alloc.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"

namespace magicblocks::features::stack {

namespace {

namespace mbp = primitives;
namespace res = resources;

std::uint8_t to_u8(float v) {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

// 覆盖层顶点布局：position(float2)@0 + color(ubyte4norm)@8，stride 24。
// 与 overlay2d.vert 的 `layout(location=0) in vec2; layout(location=1) in vec4`
// 逐字对应。24 与 OverlayVertex 的 static_assert 一致（primitives/overlay2d.h）。
res::VertexLayout overlay_vertex_layout() {
  res::VertexLayout lay;
  {
    res::VertexInputBinding vb;
    vb.binding = 0;
    vb.stride = sizeof(mbp::OverlayVertex);
    vb.instanced = false;
    lay.bindings.push_back(vb);
  }
  {
    res::VertexAttribute a;
    a.location = 0;
    a.binding = 0;
    a.format = res::VertexFormat::Float2;
    a.offset = 0;
    lay.attributes.push_back(a);
  }
  {
    res::VertexAttribute a;
    a.location = 1;
    a.binding = 0;
    a.format = res::VertexFormat::UByte4Norm;
    a.offset = 8;
    lay.attributes.push_back(a);
  }
  return lay;
}

std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t seed) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint64_t h = seed;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(p[i]);
    h *= 1099511628211ull;
  }
  return h;
}

class GridOverlay final : public IGridOverlay {
 public:
  StackStatus create_passes(IRenderDevice* device, std::uint32_t samples) override {
    if (device == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "create_passes: device 为空");
    device_ = device;

    res::PipelineDesc d;
    d.vertexLayout = overlay_vertex_layout();
    {
      res::ShaderModule m;
      m.stage = res::ShaderStage::Vertex;
      m.spirv = reinterpret_cast<const std::uint8_t*>(shaders::kOverlay2dVert);
      m.size_bytes = shaders::kOverlay2dVertSize;
      d.stages.push_back(m);
    }
    {
      res::ShaderModule m;
      m.stage = res::ShaderStage::Fragment;
      m.spirv = reinterpret_cast<const std::uint8_t*>(shaders::kOverlay2dFrag);
      m.size_bytes = shaders::kOverlay2dFragSize;
      d.stages.push_back(m);
    }
    // 预乘 alpha（frag 里 outColor = vec4(rgb*a, a)），混合因子要配套。
    d.blend.enabled = true;
    d.blend.src_color = res::BlendFactor::One;
    d.blend.dst_color = res::BlendFactor::OneMinusSrcAlpha;
    d.blend.src_alpha = res::BlendFactor::One;
    d.blend.dst_alpha = res::BlendFactor::OneMinusSrcAlpha;
    // 覆盖层永远盖在积木之上：关深度测试、不写深度。
    // **不写 depthConstWrite**（D-VX-4′）。
    d.depth.depthTest = false;
    d.depth.depthWrite = false;
    d.raster.cull = res::CullMode::None;
    d.samples = samples > 1 ? samples : 1;
    d.primitive = res::PrimitiveType::TriangleList;
    d.push_constant_bytes = 8;  // vec2 viewport_size

    auto p = device->create_pipeline(d);
    if (!p.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建覆盖层管线失败: " + p.message());
    }
    pipeline_ = std::move(p).take();

    std::uint64_t h = 0x4F564C59ull;  // 'OVLY'
    h = fnv1a(&d.depth.depthTest, sizeof(d.depth.depthTest), h);
    h = fnv1a(&d.depth.depthWrite, sizeof(d.depth.depthWrite), h);
    h = fnv1a(&d.raster.cull, sizeof(d.raster.cull), h);
    h = fnv1a(&d.samples, sizeof(d.samples), h);
    h = fnv1a(&d.push_constant_bytes, sizeof(d.push_constant_bytes), h);
    hash_ = h;
    return StackStatus::Ok_();
  }

  IPipeline* pipeline() const override { return pipeline_.get(); }
  std::uint64_t pipeline_hash() const override { return hash_; }

  void set_style(const GridOverlayStyle& style) override { style_ = style; }
  const GridOverlayStyle& style() const override { return style_; }

  StackStatus collect(mbp::IOverlay2D& overlay, const GridSpec& grid,
                      const GridProjector& proj, const StackOverlayDesc& desc) override {
    stats_ = StackOverlayStats{};

    // ---- ③ 光标格（先收，覆盖在上，但绘制顺序由 push 顺序决定）----
    // 实际绘制顺序：① 网格线 → ② 锁定层 → ③ 光标。因为都是半透明混合，
    // 顺序会改变最终颜色，所以顺序本身是契约的一部分。

    // ---- ① 底层网格线：在 active_layer 的**顶面**铺一层细网格 ----
    if (desc.draw_grid_lines) {
      const float y_top = grid.origin[1] + (static_cast<float>(desc.active_layer) + 1.0f) * grid.cell_size;
      const int n = grid.half_extent;
      const float x0 = grid.origin[0] - static_cast<float>(n) * grid.cell_size;
      const float x1 = grid.origin[0] + static_cast<float>(n) * grid.cell_size;
      const float z0 = grid.origin[2] - static_cast<float>(n) * grid.cell_size;
      const float z1 = grid.origin[2] + static_cast<float>(n) * grid.cell_size;

      // 沿 x 方向的线（固定 z），共 2n+1 条
      for (int i = 0; i <= 2 * n; ++i) {
        const float z = z0 + static_cast<float>(i) * grid.cell_size;
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        const float pa[3] = {x0, y_top, z};
        const float pb[3] = {x1, y_top, z};
        if (!proj.project(pa, &ax, &ay) || !proj.project(pb, &bx, &by)) continue;
        mbp::LineDesc ln;
        ln.from = {ax, ay};
        ln.to = {bx, by};
        ln.width_px = style_.grid_line_px;
        ln.color = style_.grid_line;
        overlay.push_line(ln);
        ++stats_.line_count;
      }
      // 沿 z 方向的线（固定 x）
      for (int i = 0; i <= 2 * n; ++i) {
        const float x = x0 + static_cast<float>(i) * grid.cell_size;
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        const float pa[3] = {x, y_top, z0};
        const float pb[3] = {x, y_top, z1};
        if (!proj.project(pa, &ax, &ay) || !proj.project(pb, &bx, &by)) continue;
        mbp::LineDesc ln;
        ln.from = {ax, ay};
        ln.to = {bx, by};
        ln.width_px = style_.grid_line_px;
        ln.color = style_.grid_line;
        overlay.push_line(ln);
        ++stats_.line_count;
      }
    }

    // ---- ② 锁定层高亮：把整层铺一个半透明色板 + 描边 ----
    if (desc.draw_locked_layer) {
      const float y_top = grid.origin[1] + (static_cast<float>(desc.active_layer) + 1.0f) * grid.cell_size;
      const int n = grid.half_extent;
      const float x0 = grid.origin[0] - static_cast<float>(n) * grid.cell_size;
      const float x1 = grid.origin[0] + static_cast<float>(n) * grid.cell_size;
      const float z0 = grid.origin[2] - static_cast<float>(n) * grid.cell_size;
      const float z1 = grid.origin[2] + static_cast<float>(n) * grid.cell_size;

      std::vector<float> xs, ys;
      const float corners[4][3] = {{x0, y_top, z0}, {x1, y_top, z0}, {x1, y_top, z1}, {x0, y_top, z1}};
      bool all_ok = true;
      for (int i = 0; i < 4; ++i) {
        float px = 0.0f, py = 0.0f;
        if (!proj.project(corners[i], &px, &py)) {
          all_ok = false;
          break;
        }
        xs.push_back(px);
        ys.push_back(py);
      }
      if (all_ok) {
        mbp::RectDesc r;
        r.min = {*std::min_element(xs.begin(), xs.end()), *std::min_element(ys.begin(), ys.end())};
        r.max = {*std::max_element(xs.begin(), xs.end()), *std::max_element(ys.begin(), ys.end())};
        r.color = style_.locked_fill;
        r.border_px = 0.0f;
        overlay.push_rect(r);
        ++stats_.rect_count;

        mbp::RectDesc edge = r;
        edge.color = style_.locked_edge;
        edge.border_px = style_.locked_edge_px;
        overlay.push_rect(edge);
        ++stats_.rect_count;
      }
    }

    // ---- ③ 光标格 ----
    if (desc.has_cursor) {
      float corners[4][3];
      cell_top_quad(grid, desc.cursor, corners);
      float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      float py[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      bool ok = true;
      for (int i = 0; i < 4; ++i) {
        if (!proj.project(corners[i], &px[i], &py[i])) {
          ok = false;
          break;
        }
      }
      if (ok) {
        mbp::RectDesc r;
        r.min = {*std::min_element(px, px + 4), *std::min_element(py, py + 4)};
        r.max = {*std::max_element(px, px + 4), *std::max_element(py, py + 4)};
        r.color = style_.cursor_fill;
        r.border_px = 0.0f;
        overlay.push_rect(r);
        ++stats_.rect_count;

        mbp::RectDesc edge = r;
        edge.color = style_.cursor_edge;
        edge.border_px = style_.cursor_edge_px;
        overlay.push_rect(edge);
        ++stats_.rect_count;
      }
    }

    // 光标格中心再点一个十字，让"在哪一格"在小尺寸下也一眼可辨。
    if (desc.has_cursor) {
      float corners[4][3];
      cell_top_quad(grid, desc.cursor, corners);
      float cx3 = 0.0f, cy3 = 0.0f, cz3 = 0.0f;
      for (int i = 0; i < 4; ++i) {
        cx3 += corners[i][0];
        cy3 += corners[i][1];
        cz3 += corners[i][2];
      }
      cx3 *= 0.25f;
      cy3 *= 0.25f;
      cz3 *= 0.25f;
      float px = 0.0f, py = 0.0f;
      {
        const float c3[3] = {cx3, cy3, cz3};
        if (proj.project(c3, &px, &py)) {
          const float r_px = 5.0f;
          mbp::LineDesc h;
          h.from = {px - r_px, py};
          h.to = {px + r_px, py};
          h.width_px = style_.cursor_edge_px;
          h.color = style_.cursor_edge;
          overlay.push_line(h);
          mbp::LineDesc v;
          v.from = {px, py - r_px};
          v.to = {px, py + r_px};
          v.width_px = style_.cursor_edge_px;
          v.color = style_.cursor_edge;
          overlay.push_line(v);
          stats_.line_count += 2;
        }
      }
    }

    return StackStatus::Ok_();
  }

  StackStatus record(ICommandList& cmd, const mbp::OverlayGeometry& geo, std::uint32_t width,
                     std::uint32_t height, OverlayBuffers* out_buffers) override {
    stats_.vertex_count = static_cast<std::uint32_t>(geo.vertices.size());
    stats_.index_count = static_cast<std::uint32_t>(geo.indices.size());
    if (geo.vertices.empty() || geo.indices.empty()) {
      // 没东西画 → 不 draw。draw_calls 保持 0（drew=false），因为"空帧也画一次"
      // 是纯粹浪费，且会让"覆盖层是否真的画了"这个观测失去意义。
      return StackStatus::Fail(StackCode::EmptyStroke, "record: 覆盖层几何为空");
    }
    if (pipeline_ == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record: 尚未 create_passes");
    }
    if (device_ == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record: 没有设备");
    }

    // 顶点缓冲
    res::BufferDesc vbd;
    vbd.size = geo.vertices.size() * sizeof(mbp::OverlayVertex);
    vbd.usage = res::BufferUsage::Vertex;
    auto vb = device_->create_buffer(vbd);
    if (!vb.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建覆盖层顶点缓冲失败: " + vb.message());
    }
    std::unique_ptr<res::IBuffer> vown = std::move(vb).take();
    res::IBuffer* vbuf = vown.get();
    if (!vbuf->upload(geo.vertices.data(), vbd.size).ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "上传覆盖层顶点失败");
    }
    vbuf->flush_pending();

    res::BufferDesc ibd;
    ibd.size = geo.indices.size() * sizeof(std::uint32_t);
    ibd.usage = res::BufferUsage::Index;
    auto ib = device_->create_buffer(ibd);
    if (!ib.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建覆盖层索引缓冲失败: " + ib.message());
    }
    std::unique_ptr<res::IBuffer> iown = std::move(ib).take();
    res::IBuffer* ibuf = iown.get();
    if (!ibuf->upload(geo.indices.data(), ibd.size).ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "上传覆盖层索引失败");
    }
    ibuf->flush_pending();

    if (auto s = cmd.bind_pipeline(pipeline_.get(), hash_); !s.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "bind 覆盖层管线失败: " + s.message);
    }
    res::Viewport vp;
    vp.width = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    if (auto s = cmd.set_viewport(vp); !s.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "set_viewport 失败: " + s.message);
    }
    res::ScissorRect sr;
    sr.width = width;
    sr.height = height;
    if (auto s = cmd.set_scissor(sr); !s.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "set_scissor 失败: " + s.message);
    }

    struct OverlayPush {
      float viewport[2];
    } pc{};
    pc.viewport[0] = static_cast<float>(width);
    pc.viewport[1] = static_cast<float>(height);
    constexpr std::uint32_t kStageVertex = 0x1u;
    if (auto s = cmd.push_constants(kStageVertex, 0, sizeof(pc), &pc); !s.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "覆盖层 push_constants 失败: " + s.message);
    }

    if (auto s = cmd.bind_vertex_buffer(vbuf->id(), 0, 0); !s.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "bind 覆盖层顶点缓冲失败: " + s.message);
    }
    if (auto s = cmd.bind_index_buffer(ibuf->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "bind 覆盖层索引缓冲失败: " + s.message);
    }

    res::DrawArgs da;
    da.index_count = static_cast<std::uint32_t>(geo.indices.size());
    da.instance_count = 1;
    if (auto s = cmd.draw_indexed(da); !s.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "覆盖层 draw_indexed 失败: " + s.message);
    }
    ++stats_.draw_calls;
    stats_.drew = true;
    // 缓冲交给调用方：命令流只记句柄，缓冲必须活到 submit 之后。
    if (out_buffers != nullptr) {
      out_buffers->vertex = std::move(vown);
      out_buffers->index = std::move(iown);
    }
    return StackStatus::Ok_();
  }

  const StackOverlayStats& stats() const override { return stats_; }
  void reset_stats() override { stats_ = StackOverlayStats{}; }

 private:
  IRenderDevice* device_ = nullptr;
  std::unique_ptr<res::IPipeline> pipeline_;
  std::uint64_t hash_ = 0;
  GridOverlayStyle style_{};
  StackOverlayStats stats_{};
};

}  // namespace

bool GridProjector::project(const float world[3], float* out_x, float* out_y) const {
  const float* m = view_proj;
  const float x = world[0], y = world[1], z = world[2];
  // 列主序乘法
  const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
  const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
  const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
  if (!(cw > 1e-6f)) return false;  // 点在相机背后
  const float ndc_x = cx / cw;
  const float ndc_y = cy / cw;
  if (out_x) *out_x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(width);
  if (out_y) *out_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<float>(height);
  return true;
}

void cell_top_quad(const GridSpec& grid, const Cell& cell, float out_corners[4][3]) {
  const float cs = grid.cell_size;
  const float x0 = grid.origin[0] + static_cast<float>(cell.x) * cs;
  const float x1 = x0 + cs;
  const float z0 = grid.origin[2] + static_cast<float>(cell.z) * cs;
  const float z1 = z0 + cs;
  const float y = grid.origin[1] + (static_cast<float>(cell.y) + 1.0f) * cs;
  out_corners[0][0] = x0; out_corners[0][1] = y; out_corners[0][2] = z0;
  out_corners[1][0] = x1; out_corners[1][1] = y; out_corners[1][2] = z0;
  out_corners[2][0] = x1; out_corners[2][1] = y; out_corners[2][2] = z1;
  out_corners[3][0] = x0; out_corners[3][1] = y; out_corners[3][2] = z1;
}

std::unique_ptr<IGridOverlay> create_grid_overlay() {
  return std::make_unique<GridOverlay>();
}

}  // namespace magicblocks::features::stack
