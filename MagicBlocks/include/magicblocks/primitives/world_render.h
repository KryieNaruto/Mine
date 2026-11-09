// MagicBlocks L3 —— IWorld → IBatchBuilder 的接线(spec §4.3 / §7.5)
//
// 职责:把 ECS 里"要画的东西"翻译成合批器能吃的 `DrawRequest`。
//
// 为什么单独一个头,而不是塞进 world.h:
//   world.h 是 ECS 的公共契约,合批器是渲染侧的契约。编进同一个头会让
//   "只想要 ECS 的测试"也不得不 include batch.h / material.h,依赖链无谓变宽。
//   这里只做**单向依赖**:world_render.h → {world.h, batch.h, camera.h},反过来不成立。
//
// 本头零 Vk*、不读时钟(G3 / check_scene_no_clock 口径)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "magicblocks/primitives/batch.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/world.h"

namespace magicblocks::primitives {

// 尚未解析的管线哈希哨兵值。
//
// world 侧**没有资格**填 `DrawRequest::pipeline_hash`：它派生自材质 + 顶点布局,
// 而 world 故意不持有材质表(见 world.h 的 MeshRef/MaterialRef 注释)。
// 所以这里填这个哨兵值,表示"待渲染器改写"。
//
// **合批前必须由渲染器改写。** 未改写就把这些请求交给 `build()`,所有请求会被
// 当成同一管线合并,`BatchStats::pipeline_switches` 恒为 0 —— 而它是 §4.1 的
// 门控指标(≤ 16),恒 0 等于这个指标失效、永远显示完美。换句话说:拿含哨兵值的
// 请求去断言 `pipeline_switches` 得到的是无意义的绿灯。
//
// 常量本身定义在 batch.h(合批排序键的归属地) —— 这样 IPrimitiveRenderer
// 不必依赖 world_render.h 就能识别哨兵值。

// 单个网格提交时的几何参数。下标即 MeshId,所以 0 号槽(kInvalidMeshId)
// 必须保持全 0 —— 它表示"没有网格",永远不会被提交。
struct MeshInfo {
  std::uint32_t index_count = 0;
  std::uint32_t first_index = 0;
  std::int32_t vertex_offset = 0;
  std::uint32_t vertex_count = 0;
};

// 为什么"索引数 / 顶点数"要由调用方传一张 (MeshId → MeshInfo) 表,
// 而不是让 world 自己去查 IMesh:
//   `IMesh` 是 GPU 侧对象,G3 要求 primitives 层不碰后端类型;更重要的是
//   world 一旦持有 IMesh 指针,实体和资源就耦合死了 —— 换网格库就得改 ECS。
//   所以这里只接受纯数值表,world 侧对渲染资源一无所知。
struct DrawRequestBuildDesc {
  // 视图矩阵来源。为 nullptr 时**不剔除**(全部视为可见)——
  // 单元测试和离线导出场景都不需要相机。
  const ICamera* camera = nullptr;

  // 视锥剔除开关。剔除纯属优化:关掉后结果必须完全一致(只是更慢),
  // 这条性质让剔除本身可以用"开/关结果相同"的测试来验证。
  bool frustum_cull = true;

  // 稀疏表(下标即 MeshId)。由渲染器在**网格上传时**填一次,帧内只读。
  // 为空或 mesh 越界时退到下面的 fallback 数量。
  const std::vector<MeshInfo>* mesh_info = nullptr;

  // 兜底几何数量:用于"只关心合批结构"的测试与预览路径。
  std::uint32_t fallback_index_count = 0;
  std::uint32_t fallback_vertex_count = 0;

  // 可选的实例世界矩阵输出。非空时,第 i 条成功 push 的请求的世界矩阵写进
  // `out_matrices[instance_index]`,即 `out_matrices[req.instance_index]`。
  //
  // 为什么要由这里算:世界矩阵 = TransformComp 的 TRS,**只有 world 知道**
  // 一个实体的权威变换(父子关系、将来的层级缩放都在 ECS 侧)。让渲染器
  // 反查实体再算一遍等于把 TRS 规则复制第二份 —— 那必然在某个用例上分叉。
  //
  // 容量契约:调用方必须保证至少 `实体总数` 个元素(通常就是
  // `world.entity_count()`,或预先按上限 reserve 的帧内缓冲)。容量不足时
  // 本函数**不写越界**,而是返回 `stats.matrices_written < requests_pushed`
  // 让调用方发现 —— 静默截断会让一半物体用单位矩阵画在原点,比崩溃更难查。
  Mat4* out_matrices = nullptr;
  // out_matrices 的容量(元素数)。out_matrices 非空时必填。
  std::size_t out_matrix_capacity = 0;
};

// 一次接线的统计。与 BatchStats 互补:这里记的是**输入侧**丢掉了多少。
struct DrawRequestBuildStats {
  std::uint32_t entities_seen = 0;      // 带 Mesh+Material+Transform 的实体数
  std::uint32_t requests_pushed = 0;    // 真正交给合批器的条数
  std::uint32_t skipped_no_mesh = 0;    // MeshRefComp::mesh == kInvalidMeshId
  std::uint32_t skipped_culled = 0;     // 视锥剔除落掉
  // 实际写进 out_matrices 的矩阵数。**应当等于 requests_pushed**;
  // 小于它就是 out_matrix_capacity 不够(调用方 bug,不是本函数的错)。
  std::uint32_t matrices_written = 0;
};

namespace detail {

// 视图的列序是**位序**(掩码里的置位从低位到高位),不是本文件里书写掩码的顺序。
// 标准组件的号段是 Transform=0 / MeshRef=2 / MaterialRef=3,
// 因此在 `Mesh|Material|Transform` 这个掩码下:
//   kind 0(Transform) → 第 0 列
//   kind 2(MeshRef)   → 第 1 列
//   kind 3(Material)  → 第 2 列
// 下面三条 static_assert 把"号段没被人动过"钉死 —— 改动标准组件号段时
// 编译会在这里失败,而不是在运行时读到别的列。
inline constexpr u32 kColTransform = 0;
inline constexpr u32 kColMesh = 1;
inline constexpr u32 kColMaterial = 2;

static_assert(TransformComp::kComponentKind < MeshRefComp::kComponentKind &&
                  MeshRefComp::kComponentKind < MaterialRefComp::kComponentKind,
              "build_draw_requests 的列序按 Transform < MeshRef < MaterialRef 写死");

}  // namespace detail

// 把 world 里带 `MeshRefComp + MaterialRefComp + TransformComp` 的实体翻成
// `DrawRequest` 推进 `builder`。
//
// 行为约定(渲染器可以直接依赖):
//   - **只遍历一个 archetype 视图**:掩码是这三者的并集,所以不含它们的
//     archetype 一次都不会被访问(§7.4 "只走匹配 archetype");
//   - **不调用 builder.build() 也不调 begin_frame()**:排序/合批与帧边界
//     属于调用方 —— 它可能还要 push 别的来源(笔画、世界空间 UI);
//   - **不做分配**:内部只有视图遍历,视图本身零分配;
//   - 返回值是**本次调用**的增量,不是累计。
//
// 字段填充口径(渲染器需要知道的部分):
//   - `entity_id` 填 `EntityId::index` 而非 gen 参与的值。合批器拿它做排序
//     tie-break(见 batch.h `[4] 稳定 tie-break`),要求"帧内稳定且互不相同":
//     index 满足这两条且跨帧不变;把 gen 混进来会让实体换 gen 后排序位置
//     移动,反而破坏回放确定性。gen 的用途是判活着,不是排序。
//   - `material_key` 填**材质槽位号**而不是 `hash_material_desc()` 的结果。
//     槽位号已经是该材质的唯一稳定标识(IMaterialTable 槽位不回收,spec §9.4
//     规则 5),合批只需要"同一材质的绘制相邻";而哈希要查一次表,帧内每个
//     实例都查一遍太贵。
//   - `pipeline_hash` 填 `kUnresolvedPipelineHash`(值为 0):它派生自材质 +
//     顶点布局,而 world 故意不持有材质表。**合批前必须由渲染器改写**,
//     否则 `pipeline_switches` 指标失效(见该常量的说明)。
//   - `instance_index` 填"已 push 的条数",即世界侧生成的实例属性数组下标。
//
// 世界矩阵(desc.out_matrices 非空时):
//   每个通过过滤的实体算一次 `make_trs(translation, rotation, scale)` 并在
//   同一个下标写入 —— **列主序**(`Mat4::m[col*4 + row]`),与 Vulkan/GLSL
//   和 `InstanceAttribs::model[16]` 的期望一致,GPU 端直接 memcpy 即可。
inline DrawRequestBuildStats build_draw_requests(IWorld& world, IBatchBuilder& builder,
                                                 const DrawRequestBuildDesc& desc) {
  DrawRequestBuildStats stats;

  const ComponentMask mask = component_bit(MeshRefComp::kComponentKind) |
                             component_bit(MaterialRefComp::kComponentKind) |
                             component_bit(TransformComp::kComponentKind);

  ComponentView view = world.view(mask);
  const Mat4* view_matrix = desc.camera != nullptr ? &desc.camera->view_matrix() : nullptr;

  ComponentBlock block;
  while (view.next(&block)) {
    const EntityId* ids = block.ids;
    const TransformComp* xf =
        static_cast<const TransformComp*>(block.columns[detail::kColTransform]);
    const MeshRefComp* mesh_ref =
        static_cast<const MeshRefComp*>(block.columns[detail::kColMesh]);
    const MaterialRefComp* mat_ref =
        static_cast<const MaterialRefComp*>(block.columns[detail::kColMaterial]);
    if (xf == nullptr || mesh_ref == nullptr || mat_ref == nullptr) continue;

    for (u32 r = 0; r < block.count; ++r) {
      ++stats.entities_seen;

      if (mesh_ref[r].mesh == kInvalidMeshId) {
        ++stats.skipped_no_mesh;
        continue;
      }

      // 视锥剔除:**保守**判断,只看 transform 的平移点。
      // 保守方向必须是"宁可多画不可少画":漏掉一个可见物体是可见 bug,
      // 多提交一个只是浪费一点带宽。所以只剔除"明确在相机背后"的点,
      // 包围盒偏离平移点时也不会误删。
      if (desc.frustum_cull && view_matrix != nullptr) {
        const Vec3 p = {xf[r].translation[0], xf[r].translation[1], xf[r].translation[2]};
        const Vec4 clip = (*view_matrix) * Vec4{p.x, p.y, p.z, 1.0f};
        // 视图空间右手系、相机看向 -z:z >= 0 的点在相机背后。
        if (clip.z > 0.0f) {
          ++stats.skipped_culled;
          continue;
        }
      }

      const u32 instance_index = stats.requests_pushed;

      // 世界矩阵先算:它决定"这一条请求是否完整"。容量不够时**仍然 push**
      // 请求(几何还是要画),只是不写矩阵,并让 stats.matrices_written
      // 落后于 requests_pushed —— 调用方据此能发现自己的容量估算错了。
      if (desc.out_matrices != nullptr) {
        if (instance_index < desc.out_matrix_capacity) {
          desc.out_matrices[instance_index] =
              make_trs(xf[r].translation, xf[r].rotation, xf[r].scale);
          ++stats.matrices_written;
        }
      }

      DrawRequest req;
      req.mesh = mesh_ref[r].mesh;
      req.entity_id = ids[r].index;
      req.material_key = mat_ref[r].material;
      req.pipeline_hash = kUnresolvedPipelineHash;
      req.instance_index = instance_index;

      if (desc.mesh_info != nullptr && req.mesh < desc.mesh_info->size()) {
        const MeshInfo& info = (*desc.mesh_info)[req.mesh];
        req.index_count = info.index_count;
        req.first_index = info.first_index;
        req.vertex_offset = info.vertex_offset;
        req.vertex_count = info.vertex_count;
      } else {
        req.index_count = desc.fallback_index_count;
        req.vertex_count = desc.fallback_vertex_count;
      }

      builder.push(req);
      ++stats.requests_pushed;
    }
  }

  return stats;
}

// 便捷重载:不需要 MeshInfo 表、只关心合批结构的场景。
inline DrawRequestBuildStats build_draw_requests(IWorld& world, IBatchBuilder& builder,
                                                 const ICamera* camera) {
  DrawRequestBuildDesc desc;
  desc.camera = camera;
  return build_draw_requests(world, builder, desc);
}

}  // namespace magicblocks::primitives
