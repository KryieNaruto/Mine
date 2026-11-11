// MagicBlocks L3 —— 预设测试模型目录:扫描 / 加载 / 上传 / 失败语义
//
// ============================================================================
// 这个文件守的是什么
// ============================================================================
//
// 「左侧测试模型列表」这条需求里，真正容易出错的是**边界**，不是渲染：
//
//   1. **扫描不猜**：目录不存在 ≠ 目录为空。前者必须报错，后者才是空列表。
//   2. **失败进清单**：一个坏 OBJ 必须**仍然出现在列表里**并带原因，
//      静默跳过等于让用户以为文件不存在。
//   3. **归一化只有一个口径**：包围盒对角线 → target_diagonal。实测
//      res/models 五个模型尺寸差 1~2 个数量级（cube 0.167 / sphere 0.100 /
//      bunny 0.254 / teapot 0.212×0.104×0.132 / panel 1×1×0.0），
//      不归一化同屏就是"一个点 + 一块板"。
//   4. **上传不留半成品**：任何一步失败都不产出对象。
//
// ============================================================================
// 两条纪律
// ============================================================================
// 1) **不做 SKIP**。缺资源时用例失败并打印期望路径（全仓纪律）。
//    资源根由 MB_ASSET_ROOT 给，缺省取资源清单里记录的位置。
// 2) 不依赖工作目录：所有路径都由 asset root 拼出来。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/primitives/mesh_loader.h"
#include "mb_internal/magicblocks/primitives/model_catalog.h"
#include "mb_internal/magicblocks/primitives/types.h"
#include "mb_internal/magicblocks/resources/memory.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace mbp = magicblocks::primitives;
namespace fs = std::filesystem;

namespace {

std::string asset_root() {
  const char* env = std::getenv("MB_ASSET_ROOT");
  std::string root = (env != nullptr && env[0] != '\0')
                         ? std::string(env)
                         : std::string("E:/workspace/myspace/RayTracing/_install/bin/res");
  while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
  return root;
}

std::string models_dir() { return asset_root() + "/models"; }

dev::Result<std::unique_ptr<dev::IRenderDevice>> make_device() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks model catalog test";
  d.require_software_rasterizer = true;
  return dev::create_render_device(d);
}

void reserve_pages(dev::IRenderDevice& device) {
  res::IMemoryAllocator* alloc = device.memory_allocator();
  ASSERT_NE(alloc, nullptr);
  // 模型走 Staging 桶(CPU 可映射),与 L3 其他渲染器同口径。
  ASSERT_TRUE(alloc->reserve_pages(res::MemoryKind::Staging, 8).ok());
}

// 一个临时目录(跑完自动删),用于"造坏文件/空目录"这类用例。
class TempDir {
 public:
  explicit TempDir(const char* tag) {
    static int counter = 0;
    path_ = fs::temp_directory_path() /
            (std::string("mb_model_catalog_") + tag + "_" + std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(path_, ec);
    fs::create_directories(path_, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }
  std::string write(const std::string& name, const std::string& text) const {
    const fs::path p = path_ / name;
    std::ofstream out(p, std::ios::binary);
    out << text;
    out.close();
    return p.string();
  }
  std::string empty_file(const std::string& name) const { return write(name, ""); }

 private:
  fs::path path_;
};

// 一个合法的最小 OBJ(三角形)。
constexpr const char* kTriangleObj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
// 一个语法上不可能被解析的 OBJ。
constexpr const char* kGarbageObj = "这不是 OBJ\nf 1 2 3 4 5 6 7 8 9 99\n";

}  // namespace

// ================================================================ 扫描语义

TEST(ModelCatalog, MissingDirectoryIsAnErrorNotAnEmptyList) {
  // "目录不存在"与"目录为空"是完全不同的两件事：前者是配置错误，
  // 后者可能是正常状态。混为一谈会让调用方无从判断。
  auto r = mbp::scan_model_dir(asset_root() + "/__no_such_dir__");
  EXPECT_FALSE(r.ok()) << "不存在的目录必须报错";
  EXPECT_EQ(r.status().code, dev::StatusCode::IoError);

  // 空字符串同样是错误(资源根没配)。
  EXPECT_FALSE(mbp::scan_model_dir("").ok()) << "空路径必须报错";
}

TEST(ModelCatalog, EmptyDirectoryYieldsEmptyList) {
  TempDir tmp("empty");
  auto r = mbp::scan_model_dir(tmp.path().string());
  ASSERT_TRUE(r.ok()) << r.message();
  EXPECT_TRUE(r.value().empty()) << "空目录应当是空列表(不是错误)";
}

TEST(ModelCatalog, ScansRealModelsSortedAndFilteredByExtension) {
  auto r = mbp::scan_model_dir(models_dir());
  ASSERT_TRUE(r.ok()) << "扫描 res/models 失败: " << r.message()
                      << "\n  资源根: " << asset_root()
                      << "\n  提示:设置 MB_ASSET_ROOT 指向 res/ 目录";

  const auto& entries = r.value();
  ASSERT_FALSE(entries.empty());
  // 只列 .obj：.mtl 不是模型（teapot.mtl 真实存在于 res/models 下）。
  for (const mbp::ModelEntry& e : entries) {
    EXPECT_FALSE(e.name.empty());
    EXPECT_GT(e.bytes, 0u) << e.name;
    EXPECT_NE(e.path.find(".obj"), std::string::npos) << "列出的必须是 .obj: " << e.path;
  }
  // 按名字排序(列表每次刷新顺序必须一致)。
  std::vector<std::string> names;
  for (const auto& e : entries) names.push_back(e.name);
  std::vector<std::string> sorted = names;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(names, sorted) << "扫描结果必须按名字排序";

  // 五个已知模型都在（资源清单 t8 实测过）。
  for (const char* want : {"bunny", "cube", "panel", "sphere", "teapot"}) {
    EXPECT_NE(std::find(names.begin(), names.end(), want), names.end())
        << "缺少模型: " << want;
  }
  // .mtl 不该出现。
  EXPECT_EQ(std::find(names.begin(), names.end(), "teapot.mtl"), names.end());
  EXPECT_EQ(std::find(names.begin(), names.end(), "mtl"), names.end());
}

TEST(ModelCatalog, ListAvailableModelsReturnsOnlyUsableNames) {
  auto r = mbp::list_available_models(models_dir());
  ASSERT_TRUE(r.ok()) << r.message();
  EXPECT_GE(r.value().size(), 5u);
  for (const std::string& n : r.value()) EXPECT_FALSE(n.empty());
}

TEST(ModelCatalog, BrokenEntriesStayInTheListWithAReason) {
  // 纪律 2：坏文件**仍然出现在列表里**，带 ok=false 与 message。
  // 静默跳过 = 让用户以为文件不存在。
  TempDir tmp("broken");
  tmp.write("good.obj", kTriangleObj);
  tmp.empty_file("empty.obj");
  tmp.write("notes.txt", "not a model");

  auto r = mbp::scan_model_dir(tmp.path().string());
  ASSERT_TRUE(r.ok()) << r.message();
  const auto& es = r.value();
  ASSERT_EQ(es.size(), 2u) << "应当只列出两个 .obj(notes.txt 被扩展名过滤掉)";

  bool saw_good = false;
  bool saw_empty = false;
  for (const mbp::ModelEntry& e : es) {
    if (e.name == "good") {
      saw_good = true;
      EXPECT_TRUE(e.ok);
    } else if (e.name == "empty") {
      saw_empty = true;
      EXPECT_FALSE(e.ok) << "空文件必须标记为不可用";
      EXPECT_FALSE(e.message.empty()) << "不可用的条目必须带原因";
    }
  }
  EXPECT_TRUE(saw_good);
  EXPECT_TRUE(saw_empty) << "空文件被静默丢弃了 —— 违反了'失败进清单'";
}

TEST(ModelCatalog, OversizedFileIsFlaggedButStillListed) {
  TempDir tmp("big");
  tmp.write("big.obj", kTriangleObj);
  mbp::ModelScanOptions opt;
  opt.max_file_bytes = 4;  // 故意设得比文件小
  auto r = mbp::scan_model_dir(tmp.path().string(), opt);
  ASSERT_TRUE(r.ok()) << r.message();
  ASSERT_EQ(r.value().size(), 1u);
  EXPECT_FALSE(r.value()[0].ok);
  EXPECT_NE(r.value()[0].message.find("过大"), std::string::npos) << r.value()[0].message;
}

// ================================================================ 加载与归一化

TEST(ModelCatalog, LoadNormalizesByBoundingBoxDiagonal) {
  auto loader = mbp::create_mesh_loader();
  ASSERT_NE(loader, nullptr);

  mbp::ModelNormalize norm;
  norm.enabled = true;
  norm.target_diagonal = 1.0f;

  struct Case {
    const char* name;
    float expect_scale_smaller_than_one;
  };
  // 五个真实模型的原始对角线都 != 1，归一化后必须**全部等于 1**。
  for (const char* name : {"cube", "panel", "sphere", "bunny", "teapot"}) {
    const std::string path = models_dir() + "/" + name + ".obj";
    auto r = mbp::load_model(path, *loader, norm);
    ASSERT_TRUE(r.ok()) << name << ": " << r.message;

    // 原始包围盒必须非空(loader 的 Box::empty 恒 true 这个坑在这里被实测)。
    EXPECT_FALSE(r.source_bounds.empty) << name << ": source_bounds 被标成了空";
    const float src_diag = mbp::length(r.source_bounds.max - r.source_bounds.min);
    EXPECT_GT(src_diag, 1e-4f) << name << ": 原始对角线必须 > 0";

    // 缩放系数 = target / 原始对角线。
    EXPECT_NEAR(r.applied_scale, 1.0f / src_diag, 1e-4f)
        << name << ": applied_scale 与包围盒对角线不符";

    // 归一化后的包围盒对角线必须**恰好**是 target。
    const float out_diag = mbp::length(r.data.bounds.max - r.data.bounds.min);
    EXPECT_NEAR(out_diag, 1.0f, 1e-3f) << name << ": 归一化后对角线应为 1.0";
    // 中心必须平移到原点。
    const mbp::Vec3 center = (r.data.bounds.min + r.data.bounds.max) * 0.5f;
    EXPECT_NEAR(center.x, 0.0f, 1e-3f) << name;
    EXPECT_NEAR(center.y, 0.0f, 1e-3f) << name;
    EXPECT_NEAR(center.z, 0.0f, 1e-3f) << name;
  }
}

TEST(ModelCatalog, DisabledNormalizationKeepsOriginalGeometry) {
  auto loader = mbp::create_mesh_loader();
  ASSERT_NE(loader, nullptr);
  mbp::ModelNormalize off;
  off.enabled = false;
  auto r = mbp::load_model(models_dir() + "/sphere.obj", *loader, off);
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_FLOAT_EQ(r.applied_scale, 1.0f) << "关掉归一化时缩放必须恰好是 1.0";
  // 原始 sphere 半径 0.05（资源清单实测）。
  EXPECT_NEAR(mbp::length(r.data.bounds.max - r.data.bounds.min), 0.1732f, 1e-3f);
}

TEST(ModelCatalog, LoadPropagatesLoaderErrorsVerbatim) {
  auto loader = mbp::create_mesh_loader();
  ASSERT_NE(loader, nullptr);

  // 缺失文件 → kFileNotFound(不是 kParseFailed)。
  auto missing = mbp::load_model(models_dir() + "/__nope__.obj", *loader);
  EXPECT_FALSE(missing.ok());
  EXPECT_EQ(missing.error, mbp::MeshLoadError::kFileNotFound);
  EXPECT_NE(missing.message.find("__nope__.obj"), std::string::npos)
      << "错误信息必须带路径: " << missing.message;

  // 坏语法 → kParseFailed(或 kEmptyGeometry，取决于哪条先触发)。
  TempDir tmp("garbage");
  const std::string bad = tmp.write("bad.obj", kGarbageObj);
  auto parsed = mbp::load_model(bad, *loader);
  EXPECT_FALSE(parsed.ok()) << "垃圾 OBJ 不能被当成成功";
  EXPECT_FALSE(parsed.message.empty());
}

// ================================================================ 上传

TEST(ModelCatalog, UploadProducesUsableMeshAndBuffers) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value());

  mbp::ModelNormalize norm;
  norm.target_diagonal = 1.0f;
  auto m = mbp::load_and_upload_model(*d.value(), models_dir() + "/cube.obj", "cube", norm);
  ASSERT_TRUE(m.ok()) << m.message();

  const mbp::IMesh* mesh = m.value()->mesh();
  ASSERT_NE(mesh, nullptr);
  EXPECT_EQ(mesh->vertex_count(), 24u) << "cube.obj 应产生 24 个输出顶点（资源清单实测）";
  EXPECT_EQ(mesh->index_count(), 36u);
  EXPECT_NE(mesh->id(), mbp::kInvalidMeshId);
  EXPECT_NE(mesh->vertex_buffer(), nullptr);
  EXPECT_NE(mesh->index_buffer(), nullptr);
  EXPECT_GT(m.value()->gpu_bytes(), 0u);

  // 缓冲上不该有"忘了 flush"的残留（Staging 直写路径恒为 0）。
  EXPECT_EQ(mesh->vertex_buffer()->pending_upload_bytes(), 0u);
  EXPECT_EQ(mesh->index_buffer()->pending_upload_bytes(), 0u);
}

TEST(ModelCatalog, UploadFailsLoudlyForMissingAndBrokenFiles) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value());

  auto missing = mbp::load_and_upload_model(*d.value(), models_dir() + "/__absent__.obj", "absent");
  EXPECT_FALSE(missing.ok()) << "缺失文件必须失败，不能产出空模型";
  EXPECT_EQ(missing.status().code, dev::StatusCode::IoError);

  TempDir tmp("broken_upload");
  const std::string bad = tmp.write("bad.obj", kGarbageObj);
  auto broken = mbp::load_and_upload_model(*d.value(), bad, "bad");
  EXPECT_FALSE(broken.ok()) << "坏 OBJ 必须失败";
  // 失败时不得留下对象。
  EXPECT_EQ(broken.value(), nullptr);
}

TEST(ModelCatalog, UploadWithoutReservedPagesReportsOutOfMemory) {
  // 没有预取 Staging 页时必须**明确报内存不足**，而不是产出空模型或崩溃。
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  // 故意不 reserve_pages。
  auto m = mbp::load_and_upload_model(*d.value(), models_dir() + "/cube.obj", "cube");
  EXPECT_FALSE(m.ok()) << "没有预取页时必须失败";
  EXPECT_TRUE(m.status().code == dev::StatusCode::OutOfMemory ||
              m.status().code == dev::StatusCode::InitializationFailed)
      << "期望 OOM/InitFailed，实际 code=" << static_cast<int>(m.status().code) << " msg="
      << m.message();
}

TEST(ModelCatalog, UploadFromTextWorksWithoutFilesystem) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value());

  mbp::ModelNormalize norm;
  norm.target_diagonal = 1.0f;
  auto m = mbp::upload_model_from_text(*d.value(), kTriangleObj, "triangle", norm);
  ASSERT_TRUE(m.ok()) << m.message();
  EXPECT_EQ(m.value()->mesh()->vertex_count(), 3u);
  EXPECT_EQ(m.value()->mesh()->index_count(), 3u);
  EXPECT_NEAR(mbp::length(m.value()->cpu_data().bounds.max - m.value()->cpu_data().bounds.min),
              1.0f, 1e-3f)
      << "归一化同样作用于内存文本路径";

  auto bad = mbp::upload_model_from_text(*d.value(), kGarbageObj, "bad");
  EXPECT_FALSE(bad.ok());
}

// ================================================================ 真实模型全量

TEST(ModelCatalog, AllRealModelsLoadAndUpload) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value());

  struct Expect {
    const char* name;
    dev::u32 tris;
  };
  // 资源清单 t8 的实测值（三角形数）。
  const Expect expected[] = {{"cube", 12}, {"panel", 2},   {"sphere", 2208},
                             {"bunny", 4968}, {"teapot", 6320}};

  for (const Expect& e : expected) {
    const std::string path = models_dir() + "/" + e.name + ".obj";
    auto m = mbp::load_and_upload_model(*d.value(), path, e.name);
    ASSERT_TRUE(m.ok()) << e.name << ": " << m.message();
    EXPECT_EQ(m.value()->load().stats.triangles, e.tris) << e.name << " 三角形数不符";
    EXPECT_EQ(m.value()->mesh()->index_count(), e.tris * 3u) << e.name << " 索引数不符";
    // 实测这些文件的 vn 齐全（normals_synthesized = 0），所以法线来自文件。
    EXPECT_FALSE(m.value()->load().stats.normals_synthesized)
        << e.name << ": 不该走缺法线重算分支（文件里有 vn）";
    EXPECT_GT(m.value()->load().source_bounds.radius(), 0.0f) << e.name;
  }
}
