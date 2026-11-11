// MagicBlocks L3 —— 预设测试模型目录实现
//
// 契约见 include/magicblocks/primitives/model_catalog.h。
//
// 本文件**不碰 GPU 之外的东西**:扫描用 <filesystem>,加载用 IMeshLoader,
// 上传用 IBuffer。没有 Vk*(G3),也没有玩法名(G7)。
#include "mb_internal/magicblocks/primitives/model_catalog.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>

namespace magicblocks::primitives {

namespace {

namespace fs = std::filesystem;

// L3 的 MeshId 必须非 0 且进程内唯一(0 = kInvalidMeshId)。
MeshId next_mesh_id() {
  static u32 counter = 0;
  return ++counter;
}

std::string lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool matches_extension(const std::string& filename, const std::vector<std::string>& exts) {
  const std::string low = lower_ascii(filename);
  for (const std::string& e : exts) {
    const std::string el = lower_ascii(e);
    if (low.size() >= el.size() && low.compare(low.size() - el.size(), el.size(), el) == 0) {
      return true;
    }
  }
  return false;
}

// 把错误码翻成 ModelLoadResult(不丢原始 message)。
ModelLoadResult fail(MeshLoadError e, std::string msg) {
  ModelLoadResult r;
  r.error = e;
  r.message = std::move(msg);
  return r;
}

// 按包围盒对角线归一化:**只有这一种口径**(见头文件说明)。
void normalize_in_place(MeshData* data, Box* source_bounds, float* applied_scale,
                        const ModelNormalize& n) {
  // **不能信 data->bounds.empty**:IMeshLoader 的 compute_bounds() 从不把
  // `empty` 置 false(它只填 min/max),所以 loader 产出的网格即使有点,
  // `empty` 也恒为 true。这里按"顶点数 > 0"重算一份真实包围盒 ——
  // 早先直接判 `bounds.empty` 就会让归一化整段跳过(实测 scale 恒 1.0,
  // 五个模型同屏尺寸差 1~2 个数量级)。
  Box b;
  if (!data->vertices.empty()) {
    b.min = Vec3{data->vertices[0].position[0], data->vertices[0].position[1],
                 data->vertices[0].position[2]};
    b.max = b.min;
    for (const Vertex& v : data->vertices) {
      for (int k = 0; k < 3; ++k) {
        if (v.position[k] < (&b.min.x)[k]) (&b.min.x)[k] = v.position[k];
        if (v.position[k] > (&b.max.x)[k]) (&b.max.x)[k] = v.position[k];
      }
    }
    b.empty = false;
    // 顺手把 loader 的 bounds 补齐(下游 IMesh::bounds() 读的是这个)。
    data->bounds = b;
  } else {
    b = data->bounds;
  }

  *source_bounds = b;
  *applied_scale = 1.0f;
  if (!n.enabled || b.empty || n.target_diagonal <= 0.0f) return;

  const Vec3 ext = b.max - b.min;
  const float diag = length(ext);
  // 退化包围盒(所有点共点)无法归一化:保持原样并记 scale=1,
  // 不返回错误 —— 几何合法,只是没有尺寸可言。
  if (!(diag > 1e-9f)) return;

  const float s = n.target_diagonal / diag;
  const Vec3 center = (b.min + b.max) * 0.5f;
  *applied_scale = s;
  for (Vertex& v : data->vertices) {
    v.position[0] = (v.position[0] - center.x) * s;
    v.position[1] = (v.position[1] - center.y) * s;
    v.position[2] = (v.position[2] - center.z) * s;
  }
  // 法线只旋转不变(等比缩放不改变方向),保持原值 —— 不做归一化式改写,
  // 因为 loader 保证它们已经是单位向量。
  data->bounds.min = (b.min - center) * s;
  data->bounds.max = (b.max - center) * s;
  data->bounds.empty = false;
}

// 已上传模型:持有 IMesh + 两个缓冲 + CPU 数据 + 加载结果。
class LoadedModel final : public ILoadedModel {
 public:
  LoadedModel(std::string name, std::unique_ptr<resources::IBuffer> vb,
              std::unique_ptr<resources::IBuffer> ib, std::unique_ptr<IMesh> mesh,
              MeshData cpu, ModelLoadResult load)
      : name_(std::move(name)),
        vb_(std::move(vb)),
        ib_(std::move(ib)),
        mesh_(std::move(mesh)),
        cpu_(std::move(cpu)),
        load_(std::move(load)) {}

  const std::string& name() const override { return name_; }
  const IMesh* mesh() const override { return mesh_.get(); }
  const MeshData& cpu_data() const override { return cpu_; }
  const ModelLoadResult& load() const override { return load_; }
  u64 gpu_bytes() const override {
    return mesh_ == nullptr ? 0 : mesh_->gpu_bytes();
  }

 private:
  std::string name_;
  std::unique_ptr<resources::IBuffer> vb_;
  std::unique_ptr<resources::IBuffer> ib_;
  std::unique_ptr<IMesh> mesh_;
  MeshData cpu_;
  ModelLoadResult load_;
};

// 网格 → GPU 缓冲 → IMesh。任何一步失败都返回错误且不留半成品。
Result<std::unique_ptr<ILoadedModel>> upload_mesh(device::IRenderDevice& device,
                                                  const std::string& name, MeshData cpu,
                                                  ModelLoadResult load) {
  if (cpu.vertices.empty() || cpu.indices.empty()) {
    return Status::Invalid("upload_mesh: 网格为空(" + name + ")");
  }
  // 缓冲用 **Staging** 用途:它们落在 HostVisible 页(CPU 可写),
  // 既满足 create_mesh 内部的 IBuffer::write() 直写路径,又能被当作
  // 顶点/索引流绑定给管线(Vulkan 不区分缓冲的"用途",只区分字节内容)。
  //
  // 为什么不用 Vertex/Index 用途:那两个桶是 Device 页,**CPU 不可映射**,
  // 而 create_mesh() 恰恰用 write() 往缓冲里写字 —— 走 Vertex 桶会在
  // write() 处返回 Unsupported("请改用 upload()"),整个加载就失败了。
  const u64 vb_bytes = static_cast<u64>(cpu.vertices.size()) * sizeof(Vertex);
  const u64 ib_bytes = static_cast<u64>(cpu.indices.size()) * sizeof(std::uint32_t);

  resources::BufferDesc vbd;
  vbd.size = vb_bytes;
  vbd.usage = resources::BufferUsage::Staging;
  vbd.persistent_map = true;
  auto vb = device.create_buffer(vbd);
  if (!vb.ok()) {
    return Status(vb.status().code,
                  "upload_mesh(" + name + "): 创建顶点缓冲失败: " + vb.message());
  }
  resources::BufferDesc ibd;
  ibd.size = ib_bytes;
  ibd.usage = resources::BufferUsage::Staging;
  ibd.persistent_map = true;
  auto ib = device.create_buffer(ibd);
  if (!ib.ok()) {
    return Status(ib.status().code, "upload_mesh(" + name + "): 创建索引缓冲失败: " + ib.message());
  }

  // create_mesh 内部会 write() 顶点/索引进缓冲,并做尺寸校验(缓冲必须 >= 数据)。
  auto mesh = create_mesh(vb.value().get(), ib.value().get(), next_mesh_id(), cpu);
  if (!mesh) {
    return Status::InitFailed("upload_mesh(" + name +
                              "): create_mesh 失败(缓冲尺寸不足或网格为空)");
  }
  // 数据已经通过 write() 直接进了映射页(Staging 桶 MAPPED 常驻),
  // 不需要也不再走 upload()/flush —— 这条路径上没有待提交字节。
  if (vb.value()->pending_upload_bytes() != 0 || ib.value()->pending_upload_bytes() != 0) {
    return Status::InitFailed("upload_mesh(" + name +
                              "): 直写路径上出现了待提交字节(缓冲不是 HostVisible 页)");
  }

  return std::unique_ptr<ILoadedModel>(new LoadedModel(
      name, std::move(vb.value()), std::move(ib.value()), std::move(mesh), std::move(cpu),
      std::move(load)));
}

}  // namespace

// ---------------------------------------------------------------- 扫描

Result<std::vector<ModelEntry>> scan_model_dir(const std::string& directory,
                                               const ModelScanOptions& options) {
  if (directory.empty()) {
    return Status::Invalid("scan_model_dir: 目录为空(资源根缺失?)");
  }
  std::error_code ec;
  const fs::path dir(directory);
  if (!fs::exists(dir, ec) || ec) {
    return Status::Io("scan_model_dir: 目录不存在: " + directory);
  }
  if (!fs::is_directory(dir, ec) || ec) {
    return Status::Io("scan_model_dir: 不是目录: " + directory);
  }

  std::vector<ModelEntry> out;
  fs::directory_iterator it(dir, ec);
  if (ec) {
    return Status::Io("scan_model_dir: 无法遍历目录: " + directory + " (" + ec.message() + ")");
  }
  for (const fs::directory_entry& e : it) {
    std::error_code fec;
    if (!e.is_regular_file(fec) || fec) continue;
    const std::string filename = e.path().filename().string();
    if (!matches_extension(filename, options.extensions)) continue;

    ModelEntry m;
    m.path = e.path().string();
    m.name = e.path().stem().string();
    const auto size = e.file_size(fec);
    m.bytes = fec ? 0 : static_cast<u64>(size);
    if (fec) {
      m.ok = false;
      m.message = "无法读取文件大小: " + fec.message();
    } else if (m.bytes == 0) {
      m.ok = false;
      m.message = "文件为空";
    } else if (m.bytes > options.max_file_bytes) {
      m.ok = false;
      m.message = "文件过大(" + std::to_string(m.bytes) + " > " +
                  std::to_string(options.max_file_bytes) + " 字节)";
    }
    out.push_back(std::move(m));
  }

  if (options.sort_by_name) {
    // 稳定排序让列表每次刷新顺序一致(golden 截图/UI 列表都依赖这一点)。
    std::sort(out.begin(), out.end(),
              [](const ModelEntry& a, const ModelEntry& b) { return a.name < b.name; });
  }
  return out;
}

Result<std::vector<std::string>> list_available_models(const std::string& directory,
                                                       const ModelScanOptions& options) {
  auto scanned = scan_model_dir(directory, options);
  if (!scanned.ok()) return scanned.status();
  std::vector<std::string> names;
  for (const ModelEntry& m : scanned.value()) {
    if (m.ok) names.push_back(m.name);
  }
  return names;
}

// ---------------------------------------------------------------- 加载与上传

ModelLoadResult load_model(const std::string& path, IMeshLoader& loader,
                           const ModelNormalize& normalize, const MeshLoadOptions& options) {
  ModelLoadResult out;
  const MeshLoadResult loaded = loader.load_obj_file(path, options);
  if (!loaded.ok()) {
    // 原样透传枚举 + message(loader 的 message 已经带行号/具体原因)。
    out.error = loaded.error;
    out.message = loaded.message;
    return out;
  }
  if (loaded.data.vertices.empty() || loaded.data.indices.empty()) {
    // loader 报告成功却没有几何 —— 这不该发生,但不能静默当成"空模型"继续走。
    out.error = MeshLoadError::kEmptyGeometry;
    out.message = "loader 返回成功但没有顶点/索引: " + path;
    return out;
  }
  out.stats = loaded.stats;
  out.data = loaded.data;
  normalize_in_place(&out.data, &out.source_bounds, &out.applied_scale, normalize);
  return out;
}

Result<std::unique_ptr<ILoadedModel>> load_and_upload_model(
    device::IRenderDevice& device, const std::string& path, const std::string& name,
    const ModelNormalize& normalize, const MeshLoadOptions& options) {
  auto loader = create_mesh_loader();
  if (loader == nullptr) return Status::InitFailed("load_and_upload_model: 无法创建网格加载器");
  ModelLoadResult loaded = load_model(path, *loader, normalize, options);
  if (!loaded.ok()) {
    // 把 loader 的枚举翻成 Status 码:缺失 → IoError,其余 → InvalidArgument。
    // message 里保留原始枚举名,便于日志直接看。
    const std::string msg = "load_and_upload_model(" + name + "): [" +
                            to_string(loaded.error) + "] " + loaded.message;
    if (loaded.error == MeshLoadError::kFileNotFound) return Status::Io(msg);
    if (loaded.error == MeshLoadError::kTooLarge) return Status::OOM(msg);
    return Status::Invalid(msg);
  }
  MeshData cpu = loaded.data;
  return upload_mesh(device, name, std::move(cpu), std::move(loaded));
}

Result<std::unique_ptr<ILoadedModel>> upload_model_from_text(
    device::IRenderDevice& device, std::string_view obj_text, const std::string& name,
    const ModelNormalize& normalize, const MeshLoadOptions& options) {
  auto loader = create_mesh_loader();
  if (loader == nullptr) return Status::InitFailed("upload_model_from_text: 无法创建网格加载器");
  MeshLoadResult loaded = loader->load_obj_text(obj_text, options);
  ModelLoadResult out;
  if (!loaded.ok()) {
    out.error = loaded.error;
    out.message = loaded.message;
  } else {
    out.stats = loaded.stats;
    out.data = loaded.data;
    normalize_in_place(&out.data, &out.source_bounds, &out.applied_scale, normalize);
  }
  if (!out.ok()) {
    return Status::Invalid("upload_model_from_text(" + name + "): [" + to_string(out.error) +
                           "] " + out.message);
  }
  MeshData cpu = out.data;
  return upload_mesh(device, name, std::move(cpu), std::move(out));
}

}  // namespace magicblocks::primitives
