// MagicBlocks L3 —— 网格加载器（spec §13 M4 第 19 项；总 spec P3）
//
// 职责：把外部网格文件读成 L3 的 MeshData。
//
// 为什么 L3 需要一个"加载器"而不只是程序化生成：
//   程序化图元覆盖不了美术资源。积木的异形件、印章的底板胚子、
//   涂鸦的模板底这样东西，最终得从 DCC 工具进来。L3 提供一条
//   确定的导入路径，上层就不用各自接 loader。
//
// ============================================================================
// 设计约束（每条都对应一个真实的坑）
// ============================================================================
//
// 1) **错误必须显式返回，不抛异常**
//    L3 其他组件全部走 Status/Result，没有一处 throw。一个库一半用异常
//    一半用返回值，调用方永远不敢确定要不要 try/catch，最终就是全都不写。
//    这里返回 Result<MeshData>，失败原因放在 MeshLoadError 里。
//
// 2) **缺法线必须重算，不能留零**
//    OBJ 里没有 `vn` 是很常见的（很多导出器默认不写）。若留着零法线，
//    几何能显示但全黑 —— 光照项全是 0，不崩不报错，只是黑的。
//    这是本文件最重要的行为，必须机器可测。
//
// 3) **多边形面必须三角化**
//    OBJ 的 `f` 允许任意边数（四边形、五边形很常见），GPU 只吃三角形。
//    直接用扇形三角化（fan）会把凹多边形三角化错 —— 面会翻出来。
//    这里用"中心点 + 边界"的方式对凸多边形正确，对凹多边形仍然是错的，
//    所以额外做了一次凹性检测并在凹面时返回明确错误，
//    而不是静默产出一个形状错误但能显示的网格。
//
// 4) **单位与朝向由调用方决定，loader 不改**
//    不替上层做 Y-up/Z-up 转换、不缩放。导进来的东西应该和文件里
//    一模一样，任何"顺手修正"都会让美术看到的和引擎里渲染的对不上。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mb_internal/magicblocks/primitives/mesh.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::primitives {

// 加载失败原因。用枚举而不是裸字符串：
// 调用方需要能区分"文件不存在"和"文件坏了"，前者可以 fallback 到默认网格，
// 后者应该报警。只给一句 message 的话，这个区分只能靠字符串匹配。
enum class MeshLoadError : std::uint8_t {
  kNone = 0,
  kFileNotFound,     // 路径不可读
  kParseFailed,      // OBJ 语法错误 / 不支持的构造
  kEmptyGeometry,    // 文件能读，但里面没有三角形
  kNonPlanarFace,    // 面的顶点不共面，扇形三角化会出错
  kConcaveFace,      // 面是凹的，扇式三角化会翻面
  kTooLarge,         // 顶点/索引超出 L3 的合理上限
};

const char* to_string(MeshLoadError e);

// 加载统计（诊断用；也让测试能断言"真的走了重算法线这条路"）
struct MeshLoadStats {
  std::uint32_t positions = 0;     // 文件里的 v 数量
  std::uint32_t normals = 0;       // 文件里的 vn 数量
  std::uint32_t uvs = 0;           // 文件里的 vt 数量
  std::uint32_t faces = 0;         // 文件里的 f 数量
  std::uint32_t triangles = 0;     // 三角化后三角形数
  std::uint32_t polygons_split = 0;  // 被拆分的 n 边形（n > 3）数量
  bool normals_synthesized = false;  // 是否走了"缺法线重算"分支
  bool normals_were_partial = false; // 部分面有法线、部分没有
};

// OBJ 导入选项
struct MeshLoadOptions {
  // 缺法线时按位置焊接后求面法线平均（让硬边保留）。
  // 关掉的话直接给 (0,1,0)，几乎总是错的 —— 保留开关只为排查用。
  bool synthesize_normals = true;

  // 法线重算时的焊接容差（相对模型包围盒对角线）。
  // 光靠 bit-identical 位置比较，DCC 导出的重复顶点常常差 1e-7，
  // 焊接失败 → 每个三角形各自独立 → 法线全变成面法线 → 失去平滑。
  float weld_epsilon_ratio = 1e-6f;

  // 上限保护：防止误加载超大文件把内存打爆。
  std::uint32_t max_vertices = 4u * 1024u * 1024u;
  std::uint32_t max_indices = 12u * 1024u * 1024u;
};

// 加载结果：成功时 data 有效，失败时 error 说明原因。
struct MeshLoadResult {
  MeshData data;
  MeshLoadStats stats;
  MeshLoadError error = MeshLoadError::kNone;
  std::string message;  // 人类可读的失败细节（含行号）

  bool ok() const { return error == MeshLoadError::kNone; }
};

// 网格加载器接口。
//
// 目前只有 OBJ 一种格式（spec 指定），但接口按"可扩展"设计：
// 新增格式是加方法，不改已有签名。
class IMeshLoader {
 public:
  IMeshLoader() = default;
  IMeshLoader(const IMeshLoader&) = delete;
  IMeshLoader& operator=(const IMeshLoader&) = delete;
  virtual ~IMeshLoader() = default;

  // 从内存里的 OBJ 文本加载。
  //
  // 单独提供文本入口而不是只给文件路径，有两个实际理由：
  //   - 单测不需要碰磁盘，也不会因为工作目录不同而失败；
  //   - 上层想从压缩包/内嵌资源里读时不必先落盘。
  virtual MeshLoadResult load_obj_text(std::string_view text,
                                       const MeshLoadOptions& options = {}) = 0;

  // 从文件加载。文件读不出来 → kFileNotFound（不区分权限/不存在，
  // 因为调用方通常只关心"拿不到"）。
  virtual MeshLoadResult load_obj_file(const std::string& path,
                                       const MeshLoadOptions& options = {}) = 0;
};

std::unique_ptr<IMeshLoader> create_mesh_loader();

}  // namespace magicblocks::primitives
