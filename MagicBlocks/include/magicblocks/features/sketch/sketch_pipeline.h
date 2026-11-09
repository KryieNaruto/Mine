// MagicBlocks L4 —— 魔法涂鸦：文档 → 3D 实体管线（F-SK 基座）
//
// ============================ 管线的三个契约 ============================
//
// 1) **单向**：SketchDocument → SketchMeshSet。绝不回写文档。
//    文档是真源，网格是派生缓存（见 sketch_document.h 顶部说明）。
//
// 2) **确定**：同样的文档必然产出**逐位相同**的 MeshData。
//    这是 golden PNG 与命令回放校验的前提。为此：
//      - 不使用任何随机数、时间、地址、线程数；
//      - 不依赖容器遍历顺序（std::unordered_* 一律不用）；
//      - 轮廓点序由采样点顺序决定，不经排序。
//
// 3) **失败必响**：任何一笔几何生成失败（不支持的 RegionOp、Region 未闭合、
//    三角化失败）都**不产生该笔的网格**，并把原因记进 Errors。
//    绝不静默跳过、也绝不退化成别的 PathStyle（D8）。
//    调用方拿到 errors 非空就必须把它当失败处理 —— 而不是"部分成功"。
//
// ============================ 为什么合并放在这一层 ============================
// 多笔的并集/差集是**玩法手感**（"画三笔连成一个形状"vs"三个独立形状"），
// 不是几何库的职责。L3 的 from_outline 只负责"一组轮廓 → 一个实体"，
// 由谁组成这一组，是玩法定的。所以合并策略写在这里。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "magicblocks/features/sketch/sketch_document.h"
#include "magicblocks/primitives/mesh.h"
#include "magicblocks/primitives/mesh_factory.h"

namespace magicblocks::features::sketch {

// 挤出的网格 + 它来自哪一笔。
struct StrokeMesh {
  StrokeId id{};
  // 多笔合并时可能有多笔共用一个网格（并集后无法拆回单笔），
  // 这时 source 里放全部参与者的 id。
  std::vector<StrokeId> source;
  primitives::MeshData mesh;
  primitives::Color4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// 一次管线运行的错误。
struct SketchError {
  enum class Code : std::uint8_t {
    UnsupportedStyle = 1,   // 该 style 在此版本不支持
    UnsupportedRegionOp = 2,  // RegionOp 未实现（Difference/Intersect/Xor）
    NotClosed = 3,          // Region 笔画首尾未闭合
    Degenerate = 4,         // 点数/笔宽不合法
    TessellationFailed = 5, // 三角化失败
    EmptyResult = 6,        // 轮廓非空但挤出结果为空
  };
  Code code = Code::Degenerate;
  StrokeId stroke{};  // 相关笔画（可能无效）
  std::string detail; // 人类可读说明（回放比对时**不**参与哈希）
};

// 管线选项。
struct BuildOptions {
  // 沿 +Z 的挤出厚度（画布平面是 XY，+Z 朝向观察者）。
  float thickness = 0.06f;

  // 多笔是否合并成一个实体。
  //
  // false（默认）：每笔各自成实体，互不影响。语义"我在画几根线"。
  // true           ：所有 Ribbon 笔画的轮廓先并集再挤出。语义"我在连一个形状"。
  //                  代价：并集后无法再按笔拆分，撤销粒度仍是笔（因为撤销
  //                  动的是文档，重建网格是整体重算）。
  bool merge_strokes = false;

  // Region 语义的布尔运算。首轮只支持 Union；
  // 其余值会让管线返回 UnsupportedRegionOp 而不是静默当 Union。
  RegionOp region_op = RegionOp::Union;

  // Region 判闭合的容差（世界单位）。
  float close_tolerance = 1e-3f;

  // 每段圆弧离散段数（圆头笔画的端帽）。
  std::uint32_t arc_steps = 6;
};

// 管线结果：网格 + 错误 + 用于回放校验的指纹。
struct SketchMeshSet {
  std::vector<StrokeMesh> meshes;
  std::vector<SketchError> errors;

  // 全部网格的几何指纹。与文档 hash 一起构成回放校验的"两个眼睛"：
  // 文档 hash 保证输入一致，mesh hash 保证**派生结果**一致。
  // 只比文档 hash 会漏掉"算法改了但输入没变"的回归。
  std::uint64_t mesh_hash = 0;

  bool ok() const { return errors.empty(); }
  std::size_t vertex_count() const {
    std::size_t n = 0;
    for (const StrokeMesh& m : meshes) n += m.mesh.vertices.size();
    return n;
  }
  std::size_t triangle_count() const {
    std::size_t n = 0;
    for (const StrokeMesh& m : meshes) n += m.mesh.indices.size() / 3;
    return n;
  }
};

// 1) 文档 → 校验每笔能否生成几何，把失败原因收集进 errors。
//    ⚠️ 返回值的 meshes 字段**恒为空**（此阶段尚未挤出）。
//    它的用途只有一个：在不做挤出、不建 GPU 资源的前提下
//    预检"这份文档会不会失败"（CLI 的 --dry-run、界面上的即时校验）。
//    要真正的网格请用 build_meshes()。
SketchMeshSet build_outlines(const SketchDocument& doc, const BuildOptions& opts);

// 2) 文档 → 最终的挤出网格。内部先 build_outlines 再挤出。
SketchMeshSet build_meshes(const SketchDocument& doc, const BuildOptions& opts);

}  // namespace magicblocks::features::sketch
