// MagicBlocks —— MSAA 像素级回归用例的共用工具（**只给测试用**）
//
// ============================ 为什么需要这个文件 ============================
//
// 2026-11-11 的根因定位（docs/superpowers/specs/2026-11-11-magicblocks-msaa-root-cause.md）
// 指出：MSAA 在 L1/L3 的"接线"是通的，但**用户能看到的那条路径上没有入口**，
// 于是"开了 MSAA 没效果"。此后本仓的纪律是：
//
//   1) **像素才是判据**：不看 caps、不看日志、不看"msaa_samples()==4"这类间接证据，
//      只看「同样的几何，4x 的边沿过渡像素必须显著多于 1x」。
//   2) **缺能力是 FAIL，不是 SKIP**：设备不支持某采样数 → 用例失败并打印原因，
//      绝不 GTEST_SKIP（全仓纪律见 MagicBlocks/docs/ARCHITECTURE.md）。
//   3) **每个用例都要能自己说明怎么坏的**：失败信息里必须带 1x/4x 的边沿计数差值、
//      覆盖率、非背景像素数 —— 只报一条 "4x 不比 1x 平滑" 等于没诊断。
//
// ============================ 这里有什么 ============================
//
//   - 几何生成：斜三角（最适合测 AA 的形状）、扁平立方体实例
//   - 像素统计：唯一颜色数、覆盖率、**几何覆盖率**（与着色/混合/裁剪全部无关）
//   - 诊断文本：一行把 1x 与 Nx 的全部指标摊开
//   - 像素转储：失败时把两张图落盘，方便人肉看（MB_MSAA_DUMP_DIR）
//
// 本头**不 include 任何 Vk* 类型**（G3 门控），只用 L2 公共面。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "magicblocks/device/types.h"
#include "magicblocks/primitives/batch.h"
#include "magicblocks/primitives/mesh.h"
#include "magicblocks/primitives/painter.h"
#include "magicblocks/resources/pipeline.h"

namespace mbp = magicblocks::primitives;
namespace mbres = ::magicblocks::resources;

namespace mb_msaa {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---------------------------------------------------------------- 采样数工具

// DeviceCaps 里最大的 >1 采样等级（没有就返回 1 = 设备不支持任何 MSAA）。
//
// 用例拿它当"要验证的等级"：设备支持 4x 就验 4x，支持 8x 就验 8x。
// 一格都不支持时由调用方 FAIL（不是 SKIP）。
inline u32 best_msaa_level(const std::vector<u32>& caps) {
  u32 best = 1;
  for (u32 lvl : caps) {
    if (lvl > 1 && lvl > best) best = lvl;
  }
  return best;
}

// 2 的幂检查（target 的 samples 必须满足）。
inline bool is_pow2(u32 v) { return v != 0 && (v & (v - 1)) == 0; }

// ---------------------------------------------------------------- 顶点 / 几何

// NDC 空间的 2D 位置顶点。配套 shaders/pixel_regression.vert(.frag)：
// 顶点着色器只做 `gl_Position = vec4(inNdc, 0, 1)`，片元只输出纯红。
//
// ⚠️ 纯红是硬契约：色块着色 + 自发光会掩盖 MSAA 的过渡色，
//    片元必须是**单一常量色**且无混合（blend.enabled=false）。
struct NdcVertex {
  float x = 0.0f;
  float y = 0.0f;
};

// 一条斜边横穿整个视口的三角形。
//
// 为什么不用轴对齐矩形：轴对齐边在 1 采样下也会落在整像素边界上，
// 「有锯齿」与「没锯齿」看起来一样，用例失去判别力。
// 三条边全部不与像素网格对齐，才是 MSAA 的照妖镜。
inline std::vector<NdcVertex> slanted_triangle_ndc() {
  return {
      {-0.82f, -0.74f},
      {0.61f, -0.31f},
      {-0.42f, 0.87f},
  };
}

// 铺满整个视口的两个三角形（用来测"整帧覆盖率必须 100%"，
// 以及"4x 的边沿不会把内部像素吃掉"）。
inline std::vector<NdcVertex> fullscreen_quad_ndc() {
  return {
      {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
      {-1.0f, -1.0f}, {1.0f, 1.0f},  {-1.0f, 1.0f},
  };
}

// 扁平立方体（y 方向压到 0.05）的实例属性：世界空间的 XY 是斜的，
// 投影到屏幕后**每一条边都是斜的**，正好把 1x/4x 的差别放大。
//
// 相机取正对 -Z（俯视），世界 XY 直接映到屏幕 XY，不需要考虑行长宽比之外的畸变。
inline mbp::InstanceAttribs flat_cube_instance() {
  mbp::InstanceAttribs a{};
  for (int i = 0; i < 16; ++i) a.model[i] = 0.0f;
  // 2026-11-11（t6）：之前 s=1.2 而 view_proj 只把 ±1.6 的世界范围映到屏幕，
  // 于是立方体只盖住视口约 27% 的一小块（屏幕中心），覆盖率 0.006 直接没过
  // "场景画出来了吗" 的 1% 门槛。改成 2.6 让它在两个方向上铺满大半屏,
  // 四条斜边都落在像素中间 —— 这才是测 AA 需要的形状。
  const float s = 2.6f;    // 世界尺度（view_proj 的 NDC 缩放是 1/1.6）
  // 列主序：model[0]=X 缩放, model[5]=Y 缩放, model[10]=Z 缩放。
  // y 不再压扁 —— 压扁后在世界里只有 0.13 高，投影到屏幕只剩 3 像素,
  // 场景等于没画出来（这正是 t6 之前 "覆盖率 0.006" 的真实原因）。
  // 绕 Z 轴旋转 30° —— **这是判定 MSAA 是否有效的关键**。
  //
  // 2026-11-11 实测：不旋转时，立方体在正交投影下是**轴对齐矩形**，四条边严格
  // 落在整像素边界上，4x 与 1x 的覆盖率完全相同（differing=0），会被误读成
  // "MSAA 没生效"。MSAA 只能平滑**亚像素覆盖**的斜边 —— 轴对齐边在两种采样下
  // 都是"整像素覆盖/整像素不覆盖"，本来就没有可混合的过渡像素。
  // 旋转 30° 后每条边都穿过像素中部，4x 的覆盖率才会与 1x 拉开。
  const float c = 0.8660254f;  // cos30
  const float sn = 0.5f;       // sin30
  a.model[0] = c * s;
  a.model[1] = sn * s;
  a.model[4] = -sn * s;
  a.model[5] = c * s;
  a.model[10] = s * 0.5f;  // Z 缩放（相机深度方向）
  a.model[12] = 0.0f;
  a.model[13] = 0.0f;
  a.model[14] = 0.0f;
  a.model[15] = 1.0f;
  // base_color = 纯白：l3_scene.frag 输出 lit * alpha，白基色让输出稳定、好判色。
  a.base_color[0] = 1.0f;
  a.base_color[1] = 1.0f;
  a.base_color[2] = 1.0f;
  a.base_color[3] = 1.0f;
  a.roughness = 0.55f;
  a.metallic = 0.0f;
  return a;
}

// ---------------------------------------------------------------- 管线描述（逐项对齐 l3_cli）

// 与 src/l3_cli/main.cpp 的 scene_pipeline_desc() 逐字段一致（布局、光照、混合、深度）。
// 复制而来而不是复用：l3_cli 是**可执行程序**的内部函数，测试不能反依赖它。
// 两处一旦分叉，表现是"测试绿了、CLI 出图不对"，所以下面每个偏移都按 batch.h 写死。
inline mbres::PipelineDesc l3_scene_pipeline_desc(u32 samples, const unsigned char* vs,
                                                      std::size_t vs_size,
                                                      const unsigned char* fs,
                                                      std::size_t fs_size,
                                                      bool cull_back = true) {
  namespace res = ::magicblocks::resources;
  mbres::PipelineDesc d;
  {
    mbres::VertexInputBinding vb;
    vb.binding = 0;
    vb.stride = sizeof(mbp::Vertex);
    vb.instanced = false;
    d.vertexLayout.bindings.push_back(vb);
  }
  {
    mbres::VertexInputBinding vb;
    vb.binding = 1;
    vb.stride = sizeof(mbp::InstanceAttribs);
    vb.instanced = true;
    d.vertexLayout.bindings.push_back(vb);
  }
  auto attr = [&d](u32 loc, u32 b, mbres::VertexFormat f, u32 off) {
    mbres::VertexAttribute a;
    a.location = loc;
    a.binding = b;
    a.format = f;
    a.offset = off;
    d.vertexLayout.attributes.push_back(a);
  };
  attr(0, 0, mbres::VertexFormat::Float3, 0);
  attr(1, 0, mbres::VertexFormat::Float3, 16);
  attr(2, 0, mbres::VertexFormat::UByte4Norm, 32);
  attr(3, 0, mbres::VertexFormat::Float2, 36);
  constexpr u32 kOffModel = 0;
  constexpr u32 kOffBaseColor = 64;
  constexpr u32 kOffEmissive = 80;
  attr(4, 1, mbres::VertexFormat::Float4, kOffModel + 0);
  attr(5, 1, mbres::VertexFormat::Float4, kOffModel + 16);
  attr(6, 1, mbres::VertexFormat::Float4, kOffModel + 32);
  attr(7, 1, mbres::VertexFormat::Float4, kOffModel + 48);
  attr(8, 1, mbres::VertexFormat::Float4, kOffBaseColor);
  attr(9, 1, mbres::VertexFormat::Float4, kOffEmissive);
  attr(10, 1, mbres::VertexFormat::Float4, 96);

  mbres::ShaderModule v;
  v.stage = mbres::ShaderStage::Vertex;
  v.spirv = vs;
  v.size_bytes = vs_size;
  d.stages.push_back(v);
  mbres::ShaderModule f;
  f.stage = mbres::ShaderStage::Fragment;
  f.spirv = fs;
  f.size_bytes = fs_size;
  d.stages.push_back(f);

  d.blend.enabled = true;
  d.blend.src_color = mbres::BlendFactor::One;
  d.blend.dst_color = mbres::BlendFactor::OneMinusSrcAlpha;
  d.blend.src_alpha = mbres::BlendFactor::One;
  d.blend.dst_alpha = mbres::BlendFactor::OneMinusSrcAlpha;
  d.depth.depthTest = true;
  d.depth.depthWrite = true;
  d.depth.depthOp = mbres::CompareOp::LessOrEqual;
  d.raster.cull = cull_back ? mbres::CullMode::Back : mbres::CullMode::None;
  d.samples = samples > 1 ? samples : 1u;
  d.primitive = mbres::PrimitiveType::TriangleList;
  d.push_constant_bytes = 128;  // view_proj(64) + normal_matrix(64)
  return d;
}

// stroke_ribbon 管线：屏幕空间、预乘 alpha、无深度、双面。
// 与 l3_cli 的 stroke_pipeline_desc() 逐字段一致。
inline mbres::PipelineDesc stroke_ribbon_pipeline_desc(u32 samples, const unsigned char* vs,
                                                           std::size_t vs_size,
                                                           const unsigned char* fs,
                                                           std::size_t fs_size) {
  namespace res = ::magicblocks::resources;
  mbres::PipelineDesc d;
  {
    mbres::VertexInputBinding vb;
    vb.binding = 0;
    vb.stride = sizeof(mbp::StrokeVertex);
    vb.instanced = false;
    d.vertexLayout.bindings.push_back(vb);
  }
  {
    mbres::VertexAttribute a;
    a.location = 0;
    a.binding = 0;
    a.format = mbres::VertexFormat::Float2;
    a.offset = 0;
    d.vertexLayout.attributes.push_back(a);
  }
  {
    mbres::VertexAttribute a;
    a.location = 1;
    a.binding = 0;
    a.format = mbres::VertexFormat::UByte4Norm;
    a.offset = 16;  // StrokeVertex::color 的偏移
    d.vertexLayout.attributes.push_back(a);
  }
  mbres::ShaderModule v;
  v.stage = mbres::ShaderStage::Vertex;
  v.spirv = vs;
  v.size_bytes = vs_size;
  d.stages.push_back(v);
  mbres::ShaderModule f;
  f.stage = mbres::ShaderStage::Fragment;
  f.spirv = fs;
  f.size_bytes = fs_size;
  d.stages.push_back(f);

  d.blend.enabled = true;
  d.blend.src_color = mbres::BlendFactor::One;
  d.blend.dst_color = mbres::BlendFactor::OneMinusSrcAlpha;
  d.blend.src_alpha = mbres::BlendFactor::One;
  d.blend.dst_alpha = mbres::BlendFactor::OneMinusSrcAlpha;
  d.depth.depthTest = false;
  d.depth.depthWrite = false;
  d.raster.cull = mbres::CullMode::None;
  d.samples = samples > 1 ? samples : 1u;
  d.primitive = mbres::PrimitiveType::TriangleList;
  d.push_constant_bytes = 16;  // viewport_size(8) + pad(8)
  return d;
}

// 纯色填充管线（自造，**不属于**既有三条管线；L1 层用例用它把
// "边沿 AA 是否生效"与 L3 的光照/混合/深度全部解耦）。
inline mbres::PipelineDesc solid_ndc_pipeline_desc(u32 samples, const unsigned char* vs,
                                                       std::size_t vs_size,
                                                       const unsigned char* fs,
                                                       std::size_t fs_size) {
  namespace res = ::magicblocks::resources;
  mbres::PipelineDesc d;
  {
    mbres::VertexInputBinding vb;
    vb.binding = 0;
    vb.stride = sizeof(NdcVertex);
    vb.instanced = false;
    d.vertexLayout.bindings.push_back(vb);
  }
  {
    mbres::VertexAttribute a;
    a.location = 0;
    a.binding = 0;
    a.format = mbres::VertexFormat::Float2;
    a.offset = 0;
    d.vertexLayout.attributes.push_back(a);
  }
  mbres::ShaderModule v;
  v.stage = mbres::ShaderStage::Vertex;
  v.spirv = vs;
  v.size_bytes = vs_size;
  d.stages.push_back(v);
  mbres::ShaderModule f;
  f.stage = mbres::ShaderStage::Fragment;
  f.spirv = fs;
  f.size_bytes = fs_size;
  d.stages.push_back(f);
  d.blend.enabled = false;
  d.depth.depthTest = false;
  d.depth.depthWrite = false;
  d.raster.cull = mbres::CullMode::None;
  d.samples = samples > 1 ? samples : 1u;
  d.primitive = mbres::PrimitiveType::TriangleList;
  d.push_constant_bytes = 0;
  return d;
}

// ---------------------------------------------------------------- 斜边的 CPU 参考几何
//
// 「几何覆盖率」是本用例族**最硬的判据**：它完全不看颜色。
//
//   每个像素 4x4 子采样点，点在三角形内 → 该点在理想光栅下会被覆盖。
//   真实 MSAA-Nx 的覆盖率应当落在两个数之间：
//     miss = 全部子点都在三角形外的像素数（1 采样光栅在这种像素上可能盖到一点）
//     hit  = 全部子点都在三角形内的像素数
//   于是：hit ≤ 覆盖率 ≤ hit + miss。
//
// 为什么这条比"过渡像素数"更可靠：
//   - 1x 光栅的像素中心命中规则会让覆盖率出现**系统性偏差**（视三角形走向而定，
//     可以是偏大也可以是偏小），单看"1x vs 4x 谁多"会误判；
//   - 理想 Nx 覆盖率的偏差只来自 4x4 子点与驱动的真实采样位置不同（约 1% 量级），
//     与「1x 规则光栅」的偏差不是一个数量级。
struct CoverageBand {
  std::size_t hit_full = 0;   // 全部子点在内部
  std::size_t miss_full = 0;  // 全部子点在外部
  std::size_t partial = 0;    // 部分子点在内部
};

struct Vec2f {
  float x = 0.0f;
  float y = 0.0f;
};

inline float tri_edge(const Vec2f& a, const Vec2f& b, float px, float py) {
  return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

// NDC（y 向上）→ 像素中心（y 向下，原点左上），与 readback_rgba8 的行序契约一致。
inline Vec2f ndc_to_pixel_center(const NdcVertex& v, u32 w, u32 h) {
  return Vec2f{(v.x + 1.0f) * 0.5f * static_cast<float>(w),
               (1.0f - v.y) * 0.5f * static_cast<float>(h)};
}

// 4x4 子采样格的中心偏移（[0.125, 0.375, 0.625, 0.875] 的相对位置）。
inline const float* sub_sample_offsets() {
  static const float kOff[4] = {0.125f, 0.375f, 0.625f, 0.875f};
  return kOff;
}

inline CoverageBand compute_coverage_band(const std::vector<NdcVertex>& tri_ndc, u32 w, u32 h) {
  CoverageBand band;
  if (tri_ndc.size() < 3) return band;
  const Vec2f p0 = ndc_to_pixel_center(tri_ndc[0], w, h);
  const Vec2f p1 = ndc_to_pixel_center(tri_ndc[1], w, h);
  const Vec2f p2 = ndc_to_pixel_center(tri_ndc[2], w, h);
  const float* off = sub_sample_offsets();
  for (u32 y = 0; y < h; ++y) {
    for (u32 x = 0; x < w; ++x) {
      int inside = 0;
      for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
          const float px = static_cast<float>(x) + off[sx];
          const float py = static_cast<float>(y) + off[sy];
          const float e0 = tri_edge(p0, p1, px, py);
          const float e1 = tri_edge(p1, p2, px, py);
          const float e2 = tri_edge(p2, p0, px, py);
          const bool all_pos = e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f;
          const bool all_neg = e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f;
          if (all_pos || all_neg) ++inside;
        }
      }
      if (inside == 16) {
        ++band.hit_full;
      } else if (inside == 0) {
        ++band.miss_full;
      } else {
        ++band.partial;
      }
    }
  }
  return band;
}

// ---------------------------------------------------------------- 像素统计
//
// 「覆盖率」= 非背景像素数 / 总像素数。
//
// 判据口径：**只看"是不是背景色"**（每个通道与清屏色相差 > tol）。
// 不要求"是不是前景色"，因为 L3 的光照会让同一个三角形出现很多亮度，
// 但它们**全都不是背景色**；这样统计口径对光照完全免疫。

inline bool px_is_background(const std::uint8_t* p, float br, float bg, float bb, int tol) {
  const int tr = static_cast<int>(br * 255.0f + 0.5f);
  const int tg = static_cast<int>(bg * 255.0f + 0.5f);
  const int tb = static_cast<int>(bb * 255.0f + 0.5f);
  return std::abs(static_cast<int>(p[0]) - tr) <= tol &&
         std::abs(static_cast<int>(p[1]) - tg) <= tol &&
         std::abs(static_cast<int>(p[2]) - tb) <= tol;
}

// 覆盖率按"像素被覆盖了多少"的连续口径算：
//   完全背景 = 0，非背景 = 1。1x 时每个像素非 0 即 1；4x 时边沿像素的
//   混合值仍然是非背景，所以覆盖率分母一致、只反映"哪些像素被碰到了"。
inline double coverage_ratio(const std::vector<std::uint8_t>& px, float br, float bg, float bb,
                             int tol) {
  if (px.empty()) return 0.0;
  std::size_t covered = 0;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    if (!px_is_background(&px[i], br, bg, bb, tol)) ++covered;
  }
  return static_cast<double>(covered) / static_cast<double>(px.size() / 4u);
}

inline std::size_t count_unique_colors(const std::vector<std::uint8_t>& px) {
  std::vector<std::uint32_t> seen;
  seen.reserve(px.size() / 4u);
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    seen.push_back((static_cast<std::uint32_t>(px[i]) << 16) |
                   (static_cast<std::uint32_t>(px[i + 1]) << 8) |
                   static_cast<std::uint32_t>(px[i + 2]));
  }
  std::sort(seen.begin(), seen.end());
  seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
  return seen.size();
}

// 两张图逐字节是否相同（"MSAA 到底有没有改变像素"的最直接证据）。
inline bool identical_pixels(const std::vector<std::uint8_t>& a,
                             const std::vector<std::uint8_t>& b) {
  return a == b;
}

inline std::size_t differing_pixel_count(const std::vector<std::uint8_t>& a,
                                         const std::vector<std::uint8_t>& b) {
  if (a.size() != b.size()) return static_cast<std::size_t>(-1);
  std::size_t n = 0;
  for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
    if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ++n;
  }
  return n;
}

// ---------------------------------------------------------------- 诊断文本

// 一行把 1x 与 Nx 的全部指标摊开。失败时由调用方塞进断言消息。
inline std::string describe_pair(const char* what, u32 base_samples, u32 msaa_samples,
                                 double cov_base, double cov_msaa, std::size_t uniq_base,
                                 std::size_t uniq_msaa, std::size_t differing) {
  char buf[768];
  std::snprintf(buf, sizeof(buf),
                "\n  [%s] 像素诊断（真实软件光栅 + 真实提交 + 真实读回）\n"
                "    覆盖率:   %ux=%.4f  %ux=%.4f   (%.4f - %.4f = %+.4f)\n"
                "    唯一色数: %ux=%zu    %ux=%zu    (%+lld)\n"
                "    差异像素: %zu / 总像素（两档输出是否逐字节相同: %s）\n"
                "    判据: MSAA 必须让边沿出现过渡色 —— 唯一色数↑ 且 覆盖率↑ 且 输出不同。",
                what, static_cast<unsigned>(base_samples), cov_base,
                static_cast<unsigned>(msaa_samples), cov_msaa, cov_msaa, cov_base,
                cov_msaa - cov_base, static_cast<unsigned>(base_samples), uniq_base,
                static_cast<unsigned>(msaa_samples), uniq_msaa,
                static_cast<long long>(uniq_msaa) - static_cast<long long>(uniq_base), differing,
                differing == 0 ? "是（MSAA 完全没生效！）" : "否");
  return std::string(buf);
}

// ---------------------------------------------------------------- 失败时转储 PNG
//
// 设了 MB_MSAA_DUMP_DIR 就把 1x/Nx 两张图写成 .bmp（不依赖 image_io，测试不引入额外依赖），
// 方便失败时用画图软件人肉对比。设置环境变量即可，默认不写盘。

inline bool dump_bmp(const std::string& path, const std::vector<std::uint8_t>& rgba, u32 w, u32 h) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const std::uint32_t row = (w * 4u);
  const std::uint32_t image_size = row * h;
  const std::uint32_t file_size = 54u + image_size;
  std::uint8_t header[54] = {0};
  header[0] = 'B';
  header[1] = 'M';
  std::memcpy(&header[2], &file_size, 4);
  const std::uint32_t off = 54;
  std::memcpy(&header[10], &off, 4);
  const std::uint32_t dib = 40;
  std::memcpy(&header[14], &dib, 4);
  std::memcpy(&header[18], &w, 4);
  std::memcpy(&header[22], &h, 4);
  const std::uint16_t planes = 1;
  std::memcpy(&header[26], &planes, 2);
  const std::uint16_t bpp = 32;
  std::memcpy(&header[28], &bpp, 2);
  std::memcpy(&header[34], &image_size, 4);
  std::fwrite(header, 1, sizeof(header), f);
  // BMP 行序自下而上；RGBA→BGRA。
  for (std::int32_t y = static_cast<std::int32_t>(h) - 1; y >= 0; --y) {
    for (u32 x = 0; x < w; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
      std::uint8_t bgra[4] = {rgba[i + 2], rgba[i + 1], rgba[i + 0], rgba[i + 3]};
      std::fwrite(bgra, 1, 4, f);
    }
  }
  std::fclose(f);
  return true;
}

// 若设置了 MB_MSAA_DUMP_DIR，把一对图写盘并返回写到的目录（否则返回空串）。
inline std::string maybe_dump(const char* tag, const std::vector<std::uint8_t>& base,
                              const std::vector<std::uint8_t>& msaa, u32 w, u32 h) {
  const char* dir = std::getenv("MB_MSAA_DUMP_DIR");
  if (dir == nullptr || dir[0] == '\0') return std::string();
  const std::string d(dir);
  std::string out = d + "/" + tag + "_1x.bmp";
  dump_bmp(out, base, w, h);
  out = d + "/" + tag + "_msaa.bmp";
  dump_bmp(out, msaa, w, h);
  char buf[512];
  std::snprintf(buf, sizeof(buf), "\n    已转储: %s\\%s_1x.bmp 与 %s_msaa.bmp", dir, tag, tag);
  return std::string(buf);
}

}  // namespace mb_msaa




