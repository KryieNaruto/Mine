// MagicBlocks L3 —— 预设测试模型目录(spec §13 M4 第 19 项「网格导入」的配套)
//
// ============================================================================
// 它解决什么问题
// ============================================================================
//
// 「左侧测试模型列表」需要的是**一份可枚举的模型清单**:名字 + 尺寸指纹 + 加载结果。
// 但 UI 层不认识模型名(那是数据,不是 UI 逻辑),玩法层也不该去扫磁盘。
// 所以这里给 L3 一个**中立的目录接口**:
//
//   scan_model_dir(asset_root)  →  [{ name, path, bytes, ok, error, ... }, ...]
//
// 调用方(CLI / 壳层)把它变成 UI 的条目;UI 只显示 `name`,不知道 bunny/teapot 是什么。
//
// ============================================================================
// 三条纪律
// ============================================================================
//
// 1) **扫描不加载**:`scan_model_dir` 只列目录 + 读文件大小,不解析 OBJ。
//    加载是显式的第二次调用(`load_model`)。理由:一个资源根下可能有几十个
//    OBJ,列个清单就全解析一遍会白白吃掉几百毫秒;而且"列出来的东西能不能用"
//    本身就是列表要显示的信息。
//
// 2) **失败进清单,不进日志**:某个 OBJ 解析失败时,它**仍然出现在列表里**,
//    带着 `ok=false` 与明确的 `error`/`message`。静默跳过等于让用户以为
//    "这个模型不存在" —— 而那正是最该显示给用户的一类信息(见 D8)。
//
// 3) **不猜资源根**:调用方给什么就用什么;目录不存在 → 整个扫描返回错误,
//    而不是返回空列表(空列表与"目录没找到"是完全不同的两件事)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/device/result.h"
#include "magicblocks/primitives/mesh.h"
#include "magicblocks/primitives/mesh_loader.h"
#include "magicblocks/resources/buffer.h"

namespace magicblocks::primitives {

using device::Status;
using device::u32;
using device::u64;

// ---------------------------------------------------------------- 目录条目

// 一个候选模型。`ok` 是**扫描阶段**的结论(文件在不在、大小是否合理);
// OBJ 语义是否合法要 load_model() 之后才知道(见 ModelLoadResult)。
struct ModelEntry {
  std::string name;   // 不含扩展名的文件名 —— UI 只显示它
  std::string path;   // 完整路径(调试/日志用)
  u64 bytes = 0;      // 文件字节数
  bool ok = true;     // false = 扫描阶段就不可用(缺失/为空/过大)
  std::string message;  // ok=false 时的原因

  bool operator==(const ModelEntry& o) const {
    return name == o.name && path == o.path && bytes == o.bytes && ok == o.ok;
  }
};

// 扫描选项。
struct ModelScanOptions {
  // 扩展名过滤(大小写不敏感)。默认只要 .obj —— 本引擎只有 OBJ 加载器,
  // 把 .mtl/.png 列进"模型"里只会让列表出现点不动的条目。
  std::vector<std::string> extensions{".obj"};
  // 超过这个大小的文件标记为 ok=false(kTooLarge 风格),但仍列出来。
  // 默认 64 MiB:比任何测试模型都大一个数量级,同时挡住误放的巨型资源。
  u64 max_file_bytes = 64ull * 1024ull * 1024ull;
  // 是否按名字排序(默认是)。排序让列表稳定 —— UI 列表每次刷新都换顺序
  // 会被当成 bug,也让 golden 截图不可复现。
  bool sort_by_name = true;
};

// 扫描一个目录(不递归)。目录不存在/不可读 → 返回错误(**不是**空列表)。
Result<std::vector<ModelEntry>> scan_model_dir(const std::string& directory,
                                               const ModelScanOptions& options = {});

// 扫描并**筛选出可用的**名字(供 CLI/壳层直接喂给 UI)。
// 这里只做一次 scan_model_dir 的过滤,不重复实现扫描逻辑。
Result<std::vector<std::string>> list_available_models(const std::string& directory,
                                                       const ModelScanOptions& options = {});

// ---------------------------------------------------------------- 加载与上传

// 规格化参数。
//
// 为什么要它:res/models 下五个模型的包围盒差 1~2 个数量级
// (实测 cube 0.167 / sphere 0.100 / bunny 0.154 / teapot 0.212×0.104×0.132 /
//  panel 1×1×0),
// 同屏陈列时不做归一化的话,sphere 在 panel 上就是个像素点。
// 归一化的口径**只有一条**:按包围盒对角线缩放到 `target_diagonal`,
// 并把包围盒中心平移到原点。不做 Y-up 转换、不让单位失真。
struct ModelNormalize {
  bool enabled = true;
  // 归一化后的包围盒**对角线长度**。默认 1.0(单位立方体的对角线约 1.732,
  // 所以 1.0 的对角线大致等于"边长 0.577 的立方体",视觉上偏保守)。
  float target_diagonal = 1.0f;
};

// 加载结果。
struct ModelLoadResult {
  MeshData data;              // 归一化后的 CPU 网格(可直接上传)
  MeshLoadStats stats;        // 文件里的原始统计
  Box source_bounds;          // **原始**包围盒(归一化前,诊断/对齐用)
  float applied_scale = 1.0f; // 归一化实际使用的缩放系数(disabled 时为 1)
  MeshLoadError error = MeshLoadError::kNone;
  std::string message;

  bool ok() const { return error == MeshLoadError::kNone; }
};

// 从文件加载一个模型。失败原因**原样**来自 IMeshLoader(kFileNotFound /
// kParseFailed / kEmptyGeometry / ...),不做二次包装 —— 调用方已经要按枚举分支。
//
// 归一化在这里做(而不是交给调用方)的原因:它是"陈列"这件事的通用需求,
// 而每个调用方各写一遍缩放数学迟早会漂移(互相差个 1e-3,截图上就对不齐)。
//
// `loader` 传非 const 引用:IMeshLoader 的加载入口是可变接口(未来可能加缓存),
// 这里不替它假设无副作用。
ModelLoadResult load_model(const std::string& path, IMeshLoader& loader,
                           const ModelNormalize& normalize = {},
                           const MeshLoadOptions& options = {});

// 一个已上传到 GPU 的模型。句柄(IMesh)由 L3 持有,缓冲也归它管
// (create_mesh 约定:IMesh 不拥有缓冲,所以这里把两个 IBuffer 也包进来)。
class ILoadedModel {
 public:
  virtual ~ILoadedModel() = default;

  virtual const std::string& name() const = 0;
  virtual const IMesh* mesh() const = 0;
  virtual const MeshData& cpu_data() const = 0;
  virtual const ModelLoadResult& load() const = 0;
  // GPU 字节占用(顶点 + 索引),诊断/预算用。
  virtual u64 gpu_bytes() const = 0;
};

// 加载 + 上传。**任何一步失败都不产出对象**(不留半成品网格)。
//
// 需要的分配器页:Vertex / Index / Staging 各预取(与 L3 其他渲染器同口径)。
// 调用方应先 reserve_pages;没预取时返回 OOM 并带"需要/已有"的明确数字。
Result<std::unique_ptr<ILoadedModel>> load_and_upload_model(device::IRenderDevice& device,
                                                            const std::string& path,
                                                            const std::string& name,
                                                            const ModelNormalize& normalize = {},
                                                            const MeshLoadOptions& options = {});

// 同上,但从内存里的 OBJ 文本(单测/内嵌资源用,不碰磁盘)。
Result<std::unique_ptr<ILoadedModel>> upload_model_from_text(
    device::IRenderDevice& device, std::string_view obj_text, const std::string& name,
    const ModelNormalize& normalize = {}, const MeshLoadOptions& options = {});

}  // namespace magicblocks::primitives
