// MagicBlocks L4 —— 魔法涂鸦：文档 → 3D 实体管线实现（F-SK 基座）
#include "magicblocks/features/sketch/sketch_pipeline.h"

#include <cmath>
#include <cstring>

namespace magicblocks::features::sketch {
namespace {

namespace pm = primitives;

std::uint64_t mix64(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

std::uint64_t hash_f(std::uint64_t seed, float v) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return mix64(seed ^ (static_cast<std::uint64_t>(bits) << 1));
}

// 网格指纹：顶点位置/法线/颜色 + 索引，全部按顺序。
// 数值用**位模式**参与，保证 -0.0f != 0.0f（回放要求逐位一致）。
std::uint64_t hash_mesh(const pm::MeshData& m) {
  std::uint64_t h = 0x5EEDull;
  h = mix64(h ^ static_cast<std::uint64_t>(m.vertices.size()));
  for (const pm::Vertex& v : m.vertices) {
    h = hash_f(h, v.position[0]);
    h = hash_f(h, v.position[1]);
    h = hash_f(h, v.position[2]);
    h = hash_f(h, v.normal[0]);
    h = hash_f(h, v.normal[1]);
    h = hash_f(h, v.normal[2]);
  }
  h = mix64(h ^ static_cast<std::uint64_t>(m.indices.size()));
  for (std::uint32_t i : m.indices) {
    h = mix64(h ^ static_cast<std::uint64_t>(i));
  }
  return h;
}

// 把一条 Region 笔画折线变成闭合环。
// 调用前必须已通过 check_style（首尾在容差内）。
std::vector<pm::Vec2> region_ring(const Stroke& s) {
  std::vector<pm::Vec2> ring = s.points;
  // 首尾极近 → 去掉末尾那个点，由 from_outline 隐式闭合。
  // 不去掉会让 clipper/tess 看到一条零长度边。
  if (ring.size() >= 2) {
    const pm::Vec2& a = ring.front();
    const pm::Vec2& b = ring.back();
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    if (dx * dx + dy * dy <= 1e-12f) ring.pop_back();
  }
  return ring;
}

}  // namespace

// 文档 → 每笔的 2D 轮廓。
//
// 注意：这个接口返回 SketchMeshSet 只是为了复用错误表类型；
// meshes 字段**恒为空**（此阶段还没有网格）。需要网格的调用方用
// build_meshes()。之所以不另立一个返回类型，是因为"轮廓 + 错误"
// 这个中间态只在 build_meshes 内部需要，不值得为它加一个公共类型。
SketchMeshSet build_outlines(const SketchDocument& doc, const BuildOptions& opts) {
  SketchMeshSet out;

  const bool region_op_ok = SketchDocument::supports_region_op(opts.region_op);

  for (const Stroke& s : doc.strokes()) {
    const StyleCheck sc = check_style(s, opts.close_tolerance);
    if (!sc.ok) {
      out.errors.push_back({s.style == PathStyle::Region ? SketchError::Code::NotClosed
                                                         : SketchError::Code::Degenerate,
                            s.id, sc.reason});
      continue;
    }

    if (s.style == PathStyle::Region && !region_op_ok) {
      out.errors.push_back(
          {SketchError::Code::UnsupportedRegionOp, s.id,
           "RegionOp 只实现了 Union;Difference/Intersect/Xor 属预留通道"});
      continue;
    }

    std::vector<pm::Vec2> ring;
    switch (s.style) {
      case PathStyle::Ribbon:
        ring = pm::stroke_outline(s.points, s.width, s.round_cap, opts.arc_steps);
        break;
      case PathStyle::Region:
        ring = region_ring(s);
        break;
    }

    if (ring.size() < 3) {
      out.errors.push_back({SketchError::Code::Degenerate, s.id, "轮廓点数不足,无法成面"});
    }
  }

  return out;
}

SketchMeshSet build_meshes(const SketchDocument& doc, const BuildOptions& opts) {
  SketchMeshSet out;

  const bool region_op_ok = SketchDocument::supports_region_op(opts.region_op);

  // ---- 第一遍：每笔 → 2D 轮廓。失败笔直接记错并跳过 ----
  struct Pending {
    std::vector<StrokeId> ids;
    pm::OutlineSet outline;
    pm::Color4 color;
  };
  std::vector<Pending> pending;
  pending.reserve(doc.stroke_count());

  // 合并模式下，所有 Ribbon 轮廓合流到这一个组里。
  // 用下标而不是指针：pending 会因 push_back 扩容而重分配，
  // 之前取到的 Pending* 会立刻悬空 —— 这是合批类代码最经典的野指针。
  constexpr std::size_t kNoGroup = static_cast<std::size_t>(-1);
  std::size_t ribbon_group = kNoGroup;

  for (const Stroke& s : doc.strokes()) {
    const StyleCheck sc = check_style(s, opts.close_tolerance);
    if (!sc.ok) {
      out.errors.push_back({s.style == PathStyle::Region ? SketchError::Code::NotClosed
                                                         : SketchError::Code::Degenerate,
                            s.id, sc.reason});
      continue;
    }

    if (s.style == PathStyle::Region && !region_op_ok) {
      out.errors.push_back(
          {SketchError::Code::UnsupportedRegionOp, s.id,
           "RegionOp 只实现了 Union;Difference/Intersect/Xor 属预留通道"});
      continue;
    }

    std::vector<pm::Vec2> ring;
    switch (s.style) {
      case PathStyle::Ribbon:
        ring = pm::stroke_outline(s.points, s.width, s.round_cap, opts.arc_steps);
        break;
      case PathStyle::Region:
        ring = region_ring(s);
        break;
    }

    if (ring.size() < 3) {
      out.errors.push_back({SketchError::Code::Degenerate, s.id, "轮廓点数不足,无法成面"});
      continue;
    }

    if (opts.merge_strokes && s.style == PathStyle::Ribbon) {
      // 合并模式：所有 Ribbon 轮廓塞进同一个 OutlineSet，
      // 交给 from_outline 的**非零环绕**一次三角化。
      // 重叠区在非零环绕下仍是实心，于是多个重叠胶囊自然并成一个实体 ——
      // 不需要引入 Clipper2 做显式布尔，也因此保持了确定性
      // （Clipper2 的并集会因精度参数产生实现相关的点）。
      if (ribbon_group == kNoGroup) {
        ribbon_group = pending.size();
        pending.push_back(Pending{});
        pending.back().color = s.color;
      }
      pending[ribbon_group].ids.push_back(s.id);
      pending[ribbon_group].outline.outer.push_back(std::move(ring));
    } else {
      Pending np;
      np.ids.push_back(s.id);
      np.color = s.color;
      np.outline.outer.push_back(std::move(ring));
      pending.push_back(std::move(np));
    }
  }

  // ---- 第二遍：轮廓 → 挤出网格 ----
  pm::ExtrudeParams ep;
  ep.thickness = opts.thickness;
  ep.cap_top = true;
  ep.cap_bottom = opts.thickness > 0.0f;
  ep.z_base = 0.0f;

  for (Pending& p : pending) {
    pm::MeshData mesh = pm::from_outline(p.outline, ep);
    if (mesh.vertices.empty() || mesh.indices.empty()) {
      // 轮廓非空但挤出为空 → 必须报错。这是"三角化静默失败"的唯一现场，
      // 放过去的话画面上就是"某笔凭空消失"且没有任何提示。
      out.errors.push_back({SketchError::Code::EmptyResult,
                            p.ids.empty() ? StrokeId{} : p.ids.front(),
                            "轮廓挤出后为空网格"});
      continue;
    }

    StrokeMesh sm;
    sm.id = p.ids.empty() ? StrokeId{} : p.ids.front();
    sm.source = p.ids;
    sm.color = p.color;
    sm.mesh = std::move(mesh);
    out.meshes.push_back(std::move(sm));
  }

  // ---- 汇总指纹 ----
  std::uint64_t h = 0xABCDEFull;
  h = mix64(h ^ static_cast<std::uint64_t>(out.meshes.size()));
  for (const StrokeMesh& m : out.meshes) {
    h = mix64(h ^ m.id.value);
    h = mix64(h ^ static_cast<std::uint64_t>(m.source.size()));
    h = mix64(h ^ hash_mesh(m.mesh));
  }
  out.mesh_hash = h;

  return out;
}

}  // namespace magicblocks::features::sketch
