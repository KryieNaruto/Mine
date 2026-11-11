// MagicBlocks L3 — 天空方向背景(sky)通道 + L1 立方体贴图像素级验收用例。
//
// ======================== 这个文件要回答的问题 ========================
//
//   「天空盒到底画出来了没有?画的是**对的东西**吗?」
//
// 不看 caps、不看日志、不信"管线建成功了"这类间接证据。判据全部落在**像素**上。
//
//   Sky.*.DiffersFromClear      开与关两张图必须不同,且差异像素覆盖背景区域
//   Sky.*.RotationChanges       只有相机**旋转**时才变化
//   Sky.*.TranslationInvariant  相机**平移**时逐字节不变(天空盒正确性的唯一硬判据)
//   Sky.*.Deterministic         同一相机 + 同一贴图两次渲染逐字节相
//   Sky.*.NotSolidColor         背景**不是一块纯色**(色种数 > 1,主色占比 < 1)
//   Sky.*.SixDirectionsDiffer   六个方向看到六个不同的面(证明面序没接错)
//
// 以及"失败即报错":
//   Sky.Errors.*                贴图缺失 / 面缺失 / 采样参数非法 → 明确 Status,不降级
//   SkyCubeMap.*                L1 立方体贴图的上传/整面校验/重复 flush
//
// ======================== 三条纪律 ========================
// 1) **缺能力是 FAIL 不是 SKIP**(与全仓像素用例同口径):没装 SwiftShader
//    六面图缺失时,用例失败并打印缺什么,不静默跳过。
// 2) 资源根由环境变量 MB_ASSET_ROOT 或资源清单里记录的位置决定。
// 3) **对象生命周期必须覆盖 submit**:命令流里存的是 SkyPass 的管线/采样器裸指针,
//    SkyPass 一旦在 submit 之前析构,设备帧提交就会回放到已销毁的 VkPipeline
//    (实测:vkCmdEndRenderPass 时命令缓冲失效 + SEH 0xc0000005)。
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/sky.h"
#include "magicblocks/primitives/types.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/memory.h"
#include "magicblocks/resources/texture.h"

#if defined(_WIN32)
#include "backend/vulkan/vulkan_cubemap.h"
#include "backend/vulkan/vulkan_render_device.h"
#endif

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace mbp = magicblocks::primitives;

namespace {

constexpr dev::u32 kW = 128;
constexpr dev::u32 kH = 96;

// 清屏色与天空盒的实际颜色刻意拉开(天空是灰蓝暖色,这里是纯黑)。
constexpr float kClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
// "是不是清屏色"的容差(RGBA8 量化)。
constexpr int kBgTol = 4;

dev::Result<std::unique_ptr<dev::IRenderDevice>> make_device() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks sky pixel regression";
  d.require_software_rasterizer = true;
  return dev::create_render_device(d);
}

// 资源根:MB_ASSET_ROOT 或资源清单里记录的位置。找不到就让用例失败(不 SKIP)。
std::string asset_cube_map_dir() {
  const char* env = std::getenv("MB_ASSET_ROOT");
  std::string root = (env != nullptr && env[0] != '\0')
                         ? std::string(env)
                         : std::string("E:/workspace/myspace/RayTracing/_install/bin/res");
  while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
  return root + "/cubeMap";
}

// 本机可读的默认场景:资源清单 §3 的裁剪,白天最均匀的一组。
constexpr const char* kDefaultScene = "qwantani_dusk_2_puresky_1k";

void reserve_pages(dev::IRenderDevice& device, dev::u32 n) {
  res::IMemoryAllocator* alloc = device.memory_allocator();
  // 注意:ASSERT_* 在 gtest 里对 void 函数没有"return"语义,但这里必须显式判空,
  // 否则 alloc 为 nullptr 时下面继续解引用 → SEH 0xc0000005(而不是一条干净的失败)。
  if (alloc == nullptr) {
    ADD_FAILURE() << "设备没有内存分配器";
    return;
  }
  ASSERT_TRUE(alloc->reserve_pages(res::MemoryKind::Texture, n).ok());
  // sky 通道的管线布局需要一块占位顶点缓冲(12 字节,内容不被读取),
  // 所以还要一页 Vertex 给它上传时用的 Staging 页。
  ASSERT_TRUE(alloc->reserve_pages(res::MemoryKind::Vertex, 1).ok());
  // 占位顶点流通过 upload() 走 Staging。L1 分页分配器的契约:
  // allocate() 不触发 VMA,调用方必须先预取整页。
  ASSERT_TRUE(alloc->reserve_pages(res::MemoryKind::Staging, 1).ok());
}

// 一次离屏渲染的完整结果。
struct Render {
  std::vector<std::uint8_t> px;  // RGBA8,行序自上而下
  bool ok = false;
  std::string error;
  mbp::SkyRecordStats sky_stats{};
  dev::u32 validation_errors = 0;
};

// 用 CPU 画出来的六面色块:每面一个主色 + 一个内部渐变,便于断言"六面不同"
// 以及"背景不是一块纯色"。颜色选择避开清屏色(纯黑)与彼此。
struct SyntheticFace {
  std::uint8_t r0 = 0, g0 = 0, b0 = 0;  // 左上
  std::uint8_t r1 = 0, g1 = 0, b1 = 0;  // 右下
};

std::vector<std::uint8_t> make_face(std::uint32_t w, std::uint32_t h, const SyntheticFace& f) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4, 0);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const float t = (static_cast<float>(x) / static_cast<float>(w - 1) +
                       static_cast<float>(y) / static_cast<float>(h - 1)) * 0.5f;
      std::uint8_t* p = &out[(static_cast<std::size_t>(y) * w + x) * 4];
      p[0] = static_cast<std::uint8_t>(static_cast<float>(f.r0) * (1.0f - t) + f.r1 * t);
      p[1] = static_cast<std::uint8_t>(static_cast<float>(f.g0) * (1.0f - t) + f.g1 * t);
      p[2] = static_cast<std::uint8_t>(static_cast<float>(f.b0) * (1.0f - t) + f.b1 * t);
      p[3] = 255;
    }
  }
  return out;
}

mbp::CameraSnapshot make_camera(const mbp::Vec3& pos, const mbp::Vec3& target) {
  mbp::CameraSnapshot s;
  s.desc.kind = mbp::ProjectionKind::Perspective;
  s.desc.fov_y_degrees = 60.0f;
  s.desc.near_plane = 0.1f;
  s.desc.far_plane = 100.0f;
  s.desc.viewport = {kW, kH};
  s.pose.position = pos;
  s.pose.target = target;
  s.pose.up = mbp::Vec3{0.0f, 1.0f, 0.0f};
  return s;
}

// 在给定相机下渲染一帧
//
// enable_sky=false 时的对照帧**仍然走 submit_command_list()**(否则 begin_pass 的
// clear 值根本不会写进目标 —— submit_and_wait 只记录不执行),代价是那条通路拒绝
// "零 draw",所以用一条被 1×1 越界 scissor 完全裁掉的天空 draw:通路确实执行了,
// 但没有片元落到目标上 —— 正是纯清屏基线。
Render render_frame(dev::IRenderDevice& device, const res::ICubeMap* cube,
                    const mbp::CameraSnapshot& camera, bool enable_sky) {
  Render out;
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {kW, kH};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  auto tgt = device.create_offscreen_target(td);
  if (!tgt.ok()) {
    out.error = "create_offscreen_target: " + tgt.message();
    return out;
  }
  auto cmd = device.create_command_list();
  if (!cmd.ok()) {
    out.error = "create_command_list: " + cmd.message();
    return out;
  }
  if (cube == nullptr) {
    out.error = "cube 为空";
    return out;
  }

  // 天空通道必须在 begin_pass **之前**建好,且必须活到 submit 之后。
  // 天空通道必须在 begin_pass **之前**建好,且必须活到 submit 之后。
  //
  // ⚠️ 每次 sky->prepare() 都要一块 12B 占位顶点缓冲,而 L1 分页分配器的契约是
  // "桶满即 OOM、不自动要页"。所以这里**按需补页**:每次渲染保证至少有一页 Vertex
  // 和 Staging 可用,否则第二次 render_frame 就会撞 "当前页无空位"。
  // 把补页放在这里(而不是让每个调用方去数自己要渲染几次),调用方就不用猜。
  {
    auto* alloc = device.memory_allocator();
    if (alloc == nullptr) {
      out.error = "设备没有内存分配器";
      return out;
    }
    if (auto rs = alloc->reserve_pages(res::MemoryKind::Vertex, 1); !rs.ok()) {
      out.error = "reserve_pages(Vertex): " + rs.message;
      return out;
    }
    if (auto rs = alloc->reserve_pages(res::MemoryKind::Staging, 1); !rs.ok()) {
      out.error = "reserve_pages(Staging): " + rs.message;
      return out;
    }
  }
  auto sky = mbp::create_sky_pass(device);
  mbp::SkyPassDesc sd;
  sd.target_shape.color_format = dev::Format::RGBA8_UNORM;
  sd.target_shape.has_depth = false;
  sd.target_shape.samples = 1;
  sd.cube = cube;
  if (auto s = sky->prepare(sd); !s.ok()) {
    out.error = "sky->prepare: " + s.message;
    return out;
  }

  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  rp.clear_color = true;
  std::memcpy(rp.clear_color_rgba, kClear, sizeof(kClear));
  rp.clear_depth = false;
  if (!cmd.value()->begin_pass(rp).ok()) {
    out.error = "begin_pass 失败";
    return out;
  }

  if (!enable_sky) {
    // 对照帧:把 scissor 推到目标外(1×1),让下面的天空 draw 光栅化出零片元。
    // L1 的 set_scissor 拒绝 0×0(width/height 必须 > 0),所以用 1×1 + 越界坐标。
    res::ScissorRect out_of_bounds;
    out_of_bounds.x = kW;
    out_of_bounds.y = 0;
    out_of_bounds.width = 1;
    out_of_bounds.height = 1;
    if (auto s = cmd.value()->set_scissor(out_of_bounds); !s.ok()) {
      out.error = "set_scissor 失败: " + s.message;
      return out;
    }
  }

  // 与 begin_pass 共用一个 pass:只画,不清屏(清屏已经由上面那步做了)。
  mbp::SkyRecordOptions opt;
  opt.clear_color = false;
  opt.clear_depth = false;
  if (auto s = sky->record(*cmd.value(), camera, opt); !s.ok()) {
    out.error = "sky->record: " + s.message;
    return out;
  }
  out.sky_stats = sky->stats();

  if (!cmd.value()->end_pass().ok()) {
    out.error = "end_pass 失败";
    return out;
  }
  if (auto s = device.submit_command_list(*cmd.value()); !s.ok()) {
    out.error = "submit_command_list: " + s.message;
    return out;
  }
  if (out.sky_stats.draws == 0) {
    out.error = "天空盒通道没有下发任何 draw";
    return out;
  }

  out.px.assign(static_cast<std::size_t>(kW) * kH * 4u, 0);
  if (auto s = tgt.value()->readback_rgba8(out.px.data(), out.px.size()); !s.ok()) {
    out.error = "readback: " + s.message;
    return out;
  }
  // validation_error_count() 是 IRenderDevice 的公共虚函数 —— 直接调即可,
  // **不需要** dynamic_cast 到 backend 私有类型(那会让 L3 反向依赖 L0 实现,
  // 也违反 G8 分层;且一旦 VulkanRenderDevice 的 RTTI 布局变化,cast 结果不可信)。
  out.validation_errors = device.validation_error_count();
  out.ok = true;
  return out;
}

std::size_t diff_pixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                        int tol) {
  if (a.size() != b.size() || a.empty()) return 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
    const int dr = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
    const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
    if (dr > tol || dg > tol || db > tol) ++n;
  }
  return n;
}

// "是不是清屏色"的像素数(用来证明天空**覆盖了背景区域**)。
//
// ⚠️ 注意口径:diff_pixels 返回的是**差异**像素数,所以"等于清屏色的像素数"
// = 总数 − 差异数。早先这里直接返回 diff 结果,于是纯清屏图得到 0、被天空盖满
// 的图反而得到 12288 —— 断言整个反过来了。读起来像"天空没画"。
std::size_t count_clear_pixels(const std::vector<std::uint8_t>& px) {
  const std::size_t differing = diff_pixels(px, std::vector<std::uint8_t>(px.size(), 0), kBgTol);
  const std::size_t total = px.size() / 4u;
  return total - differing;
}

std::size_t distinct_colors(const std::vector<std::uint8_t>& px) {
  std::vector<std::uint32_t> keys;
  keys.reserve(px.size() / 4);
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    keys.push_back((static_cast<std::uint32_t>(px[i]) << 16) |
                   (static_cast<std::uint32_t>(px[i + 1]) << 8) | px[i + 2]);
  }
  std::sort(keys.begin(), keys.end());
  return static_cast<std::size_t>(std::unique(keys.begin(), keys.end()) - keys.begin());
}

double top_color_share(const std::vector<std::uint8_t>& px) {
  if (px.empty()) return 1.0;
  std::vector<std::uint32_t> keys;
  keys.reserve(px.size() / 4);
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    keys.push_back((static_cast<std::uint32_t>(px[i]) << 16) |
                   (static_cast<std::uint32_t>(px[i + 1]) << 8) | px[i + 2]);
  }
  std::sort(keys.begin(), keys.end());
  std::size_t best = 0;
  std::size_t run = 1;
  for (std::size_t i = 1; i <= keys.size(); ++i) {
    if (i < keys.size() && keys[i] == keys[i - 1]) {
      ++run;
    } else {
      best = std::max(best, run);
      run = 1;
    }
  }
  return static_cast<double>(best) / static_cast<double>(keys.size());
}

}  // namespace

// ================================================================ 合成贴图用例
//
// 不依赖任何外部资源:六面各用一段渐变,内容是确定的。这些用例测的是**通道本身**
// (方向、深度、确定性、覆盖),不测"贴图是不是好看"。
namespace {

constexpr dev::u32 kFaceSize = 16;

// 六个面的颜色:两两不同(用 R/G/B 三个通道的变化,避免灰度下也撞色)。
const SyntheticFace kFaces[6] = {
    {220, 20, 20, 120, 10, 10},    // +X:红
    {20, 220, 20, 10, 120, 10},    // -X:绿
    {20, 20, 220, 10, 10, 120},    // +Y:蓝
    {220, 220, 20, 120, 120, 10},  // -Y:黄
    {220, 20, 220, 120, 10, 120},  // +Z:品红
    {20, 220, 220, 10, 120, 120},  // -Z:青
};

struct SyntheticCube {
  std::vector<std::uint8_t> face[6];
  std::unique_ptr<res::ICubeMap> cube;
};

std::unique_ptr<SyntheticCube> make_synthetic_cube(dev::IRenderDevice& device) {
  auto s = std::make_unique<SyntheticCube>();
  const std::uint8_t* ptrs[6] = {};
  for (int i = 0; i < 6; ++i) {
    s->face[i] = make_face(kFaceSize, kFaceSize, kFaces[i]);
    ptrs[i] = s->face[i].data();
  }
  auto r = mbp::create_cube_map_from_pixels(device, kFaceSize, kFaceSize, ptrs);
  if (!r.ok()) {
    ADD_FAILURE() << "create_cube_map_from_pixels 失败: " << r.message();
    return nullptr;
  }
  s->cube = r.take();
  return s;
}

}  // namespace

// L1:立方体贴图上传统一口径 —— 面序、整面长度、未上传面的可观测性。
TEST(SkyCubeMap, UploadsSixFacesAndValidatesWholeFace) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);

  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);
  EXPECT_EQ(syn->cube->desc().extent.width, kFaceSize);
  EXPECT_EQ(syn->cube->desc().extent.height, kFaceSize);
  EXPECT_EQ(syn->cube->face_size(), static_cast<dev::u64>(kFaceSize) * kFaceSize * 4u);
  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(syn->cube->face_uploaded(static_cast<res::CubeFace>(i)))
        << "第 " << i << " 面必须处于已上传状态";
  }

  // 整面语义:少一个字节就是错误(绝不静默截断/补零)。
  const dev::Status bad_len =
      syn->cube->upload_face(res::CubeFace::PositiveX, syn->face[0].data(),
                             syn->cube->face_size() - 1);
  EXPECT_FALSE(bad_len.ok()) << "长度不符必须拒绝";
  EXPECT_EQ(bad_len.code, dev::StatusCode::InvalidArgument);

  // 面越界
  const dev::Status bad_face = syn->cube->upload_face(static_cast<res::CubeFace>(6),
                                                      syn->face[0].data(),
                                                      syn->cube->face_size());
  EXPECT_EQ(bad_face.code, dev::StatusCode::InvalidArgument);

  // 空数据
  EXPECT_EQ(syn->cube->upload_face(res::CubeFace::PositiveX, nullptr, 0).code,
            dev::StatusCode::InvalidArgument);

  // 再次整面上传 + flush 必须成功(图像已经处于可采样布局,要走一次转换到 TRANSFER_DST)。
  EXPECT_TRUE(syn->cube->upload_face(res::CubeFace::PositiveX, syn->face[1].data(),
                                     syn->cube->face_size())
                  .ok());
  const dev::Status reflush = syn->cube->flush();
  EXPECT_TRUE(reflush.ok()) << reflush.message;
}

// L1:非法描述必须明确拒绝(不是"就近取一个合法的")。
TEST(SkyCubeMap, RejectsInvalidDescriptor) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);

  res::CubeMapDesc zero;
  zero.extent = {0, 0};
  EXPECT_EQ(d.value()->create_cube_map(zero).status().code, dev::StatusCode::InvalidArgument);

  res::CubeMapDesc rect;
  rect.extent = {8, 4};
  EXPECT_EQ(d.value()->create_cube_map(rect).status().code, dev::StatusCode::InvalidArgument)
      << "立方体六面必须同宽高";

  res::CubeMapDesc r8;
  r8.extent = {8, 8};
  r8.format = dev::Format::R8_UNORM;
  EXPECT_EQ(d.value()->create_cube_map(r8).status().code, dev::StatusCode::Unsupported)
      << "本轮只支持 RGBA8_UNORM";
}

// 开/关对照:关天空盒 = 纯清屏;开天空盒 = 背景被贴图覆盖。
TEST(Sky, DiffCoveringBackgroundAgainstClear) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);
  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);

  const auto cam = make_camera({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  const Render off = render_frame(*d.value(), syn->cube.get(), cam, false);
  ASSERT_TRUE(off.ok) << off.error;
  const Render on = render_frame(*d.value(), syn->cube.get(), cam, true);
  ASSERT_TRUE(on.ok) << on.error;

  const std::size_t total = static_cast<std::size_t>(kW) * kH;
  // 关:整屏都是清屏色(证明"关"确实是纯清屏)。
  EXPECT_EQ(count_clear_pixels(off.px), total) << "关掉天空盒后被覆盖的像素必须全是清屏色";
  // 开:背景区域被天空覆盖 —— 清屏色像素必须**消失**。
  const std::size_t cleared_after = count_clear_pixels(on.px);
  EXPECT_EQ(cleared_after, 0u) << "天空盒必须覆盖整个背景区域(仍有 " << cleared_after
                               << " 个清屏色像素)";
  EXPECT_EQ(diff_pixels(off.px, on.px, kBgTol), total)
      << "开/关两图必须在整块背景区域都不同";
  EXPECT_EQ(on.sky_stats.texture_binds, 1u) << "一次 pass 只绑一次立方体贴图";
}

// **天空盒正确性的唯一硬判据**:平移不变。
TEST(Sky, TranslationInvariant) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);
  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);

  // 两帧的**朝向完全相同**,只有位置不同。
  const auto a = make_camera({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  const auto b = make_camera({3.5f, -1.25f, 7.0f}, {3.5f, -1.25f, 6.0f});
  const Render ra = render_frame(*d.value(), syn->cube.get(), a, true);
  ASSERT_TRUE(ra.ok) << ra.error;
  const Render rb = render_frame(*d.value(), syn->cube.get(), b, true);
  ASSERT_TRUE(rb.ok) << rb.error;

  EXPECT_EQ(diff_pixels(ra.px, rb.px, 0), 0u)
      << "相机平移不得改变任何像素(天空盒只随朝向变化)。若这里红了,"
         "说明方向矩阵里混进了平移(逆视图矩阵没有丢平移列)。";
  EXPECT_EQ(std::memcmp(ra.sky_stats.inv_proj_rot, rb.sky_stats.inv_proj_rot,
                        sizeof(ra.sky_stats.inv_proj_rot)),
            0)
      << "平移时推上去的 inv_proj_rot 必须逐字节相同";
}

// **天空盒正确性的第二个硬判据**:旋转必须改变画面。
TEST(Sky, RotationChangesPixels) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  // 本用例渲染 3 帧;render_frame 每帧各补 1 页 Vertex + 1 页 Staging,
  // 所以这里必须按帧数预留(cube 自己还要 1 页 Texture)。
  reserve_pages(*d.value(), 3);
  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);

  const auto a = make_camera({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  const auto b = make_camera({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});      // yaw 90°
  // pitch 向上。这里刻意**不取接近 90°** 的视线:视线一旦与 up(0,1,0) 共线,
  // 视图矩阵就不可逆(SkyPass::record 会明确报错,而不是给出一张假图)。
  // 取 45° 已经足够看到与水平朝向不同的面。
  const auto c = make_camera({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, -1.0f});
  const Render ra = render_frame(*d.value(), syn->cube.get(), a, true);
  const Render rb = render_frame(*d.value(), syn->cube.get(), b, true);
  const Render rc = render_frame(*d.value(), syn->cube.get(), c, true);
  ASSERT_TRUE(ra.ok) << ra.error;
  ASSERT_TRUE(rb.ok) << rb.error;
  ASSERT_TRUE(rc.ok) << rc.error;

  const std::size_t total = static_cast<std::size_t>(kW) * kH;
  const std::size_t d_ab = diff_pixels(ra.px, rb.px, kBgTol);
  EXPECT_GT(d_ab, total / 2) << "yaw 90° 后应当看到完全不同的面(差异像素 " << d_ab << "/" << total
                             << ")";
  const std::size_t d_ac = diff_pixels(ra.px, rc.px, kBgTol);
  EXPECT_GT(d_ac, total / 2) << "pitch 向上后应当看到 +Y 面(差异像素 " << d_ac << "/" << total
                             << ")";
  EXPECT_GT(diff_pixels(rb.px, rc.px, kBgTol), 0u) << "两个不同朝向不得给出同一张图";
}

// 确定性:同一相机 + 同一贴图,两次渲染逐字节相同。
TEST(Sky, Deterministic) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 2);
  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);

  const auto cam = make_camera({1.0f, 2.0f, 3.0f}, {0.0f, 0.5f, 0.0f});
  const Render a = render_frame(*d.value(), syn->cube.get(), cam, true);
  const Render b = render_frame(*d.value(), syn->cube.get(), cam, true);
  ASSERT_TRUE(a.ok) << a.error;
  ASSERT_TRUE(b.ok) << b.error;
  ASSERT_EQ(a.px.size(), b.px.size());
  EXPECT_EQ(std::memcmp(a.px.data(), b.px.data(), a.px.size()), 0)
      << "两次渲染必须逐字节相同(离屏确定性是 golden 的前提)";
}

// 六个面内容不同:六个方向看过去颜色不同(证明"一面贴六遍"这种接线错误会被抓到)。
TEST(Sky, SixDirectionsShowDifferentFaces) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);
  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);

  const mbp::Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  std::uint32_t centers[6] = {};
  for (int i = 0; i < 6; ++i) {
    mbp::CameraSnapshot s;
    s.desc.kind = mbp::ProjectionKind::Perspective;
    s.desc.fov_y_degrees = 10.0f;
    s.desc.near_plane = 0.1f;
    s.desc.far_plane = 100.0f;
    s.desc.viewport = {kW, kH};
    s.pose.position = {0.0f, 0.0f, 0.0f};
    s.pose.up = (i == 2 || i == 3) ? mbp::Vec3{0.0f, 0.0f, -1.0f} : mbp::Vec3{0.0f, 1.0f, 0.0f};
    s.pose.target = dirs[i];
    const Render r = render_frame(*d.value(), syn->cube.get(), s, true);
    ASSERT_TRUE(r.ok) << "方向 " << i << ": " << r.error;
    const std::size_t c = (static_cast<std::size_t>(kH / 2) * kW + kW / 2) * 4;
    centers[i] = (static_cast<std::uint32_t>(r.px[c]) << 16) |
                 (static_cast<std::uint32_t>(r.px[c + 1]) << 8) | r.px[c + 2];
  }
  for (int i = 0; i < 6; ++i) {
    for (int j = i + 1; j < 6; ++j) {
      EXPECT_NE(centers[i], centers[j])
          << "方向 " << i << " 与 " << j << " 看到的中心像素相同,说明立方体面序/朝向接错了";
    }
  }
}

// 深浅/纯色判据:背景**不是一块纯色**。
TEST(Sky, BackgroundIsNotSolidColor) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);
  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);

  const auto cam = make_camera({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  const Render r = render_frame(*d.value(), syn->cube.get(), cam, true);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_GT(distinct_colors(r.px), 8u) << "背景必须是带内容的贴图,不是一块纯色";
  EXPECT_LT(top_color_share(r.px), 0.98) << "背景主色占比过高(看起来像纯色填充)";
}

// 失败即报错:缺贴图 / 面未上传 / 采样参数非法。
TEST(Sky, Errors_RejectMissingCubeAndBadSampler) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);

  auto sky = mbp::create_sky_pass(*d.value());

  mbp::SkyPassDesc no_cube;
  no_cube.target_shape.color_format = dev::Format::RGBA8_UNORM;
  no_cube.target_shape.has_depth = false;
  EXPECT_EQ(sky->prepare(no_cube).code, dev::StatusCode::InvalidArgument)
      << "没有立方体贴图必须拒绝,而不是退化成纯色清屏";

  // 面未上传:建了但一面都没传 → Unsupported。
  res::CubeMapDesc cd;
  cd.extent = {8, 8};
  auto blank = d.value()->create_cube_map(cd);
  ASSERT_TRUE(blank.ok()) << blank.message();
  mbp::SkyPassDesc partial;
  partial.target_shape.color_format = dev::Format::RGBA8_UNORM;
  partial.target_shape.has_depth = false;
  partial.cube = blank.value().get();
  EXPECT_EQ(sky->prepare(partial).code, dev::StatusCode::Unsupported) << "面未上传必须拒绝";

  // 采样参数非法(Repeat 会把面边缘拉成条纹)。
  auto syn = make_synthetic_cube(*d.value());
  ASSERT_NE(syn, nullptr);
  mbp::SkyPassDesc bad_sampler;
  bad_sampler.target_shape.color_format = dev::Format::RGBA8_UNORM;
  bad_sampler.target_shape.has_depth = false;
  bad_sampler.cube = syn->cube.get();
  bad_sampler.sampler.wrap_u = res::WrapMode::Repeat;
  EXPECT_EQ(sky->prepare(bad_sampler).code, dev::StatusCode::InvalidArgument);

  // 合法参数必须能过
  mbp::SkyPassDesc good;
  good.target_shape.color_format = dev::Format::RGBA8_UNORM;
  good.target_shape.has_depth = false;
  good.cube = syn->cube.get();
  const dev::Status good_status = sky->prepare(good);
  EXPECT_TRUE(good_status.ok()) << "合法描述必须能建出管线;实际: " << good_status.message;
  EXPECT_EQ(sky->set_downscale(2).code, dev::StatusCode::Unsupported)
      << "本轮不支持降级档位,必须明确报错而不是假装生效";
}

// 失败即报错:六面 PNG 缺失。
TEST(Sky, Errors_MissingFaceFilesReportExpectedPath) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 1);

  const std::string dir = asset_cube_map_dir();
  auto r = mbp::create_cube_map_from_faces(*d.value(), dir + "/__does_not_exist__",
                                           "no_such_scene_1k");
  ASSERT_FALSE(r.ok()) << "不存在的场景必须失败";
  EXPECT_EQ(r.status().code, dev::StatusCode::IoError);
  EXPECT_NE(r.message().find("no_such_scene_1k_right.png"), std::string::npos)
      << "错误信息必须带**期望路径**,让调用方知道该往哪放文件。实际: " << r.message();
}

// ================================================================ 真实资源用例
//
// 用资源清单(t8)里实测可读的那组六面 PNG。资源缺失是 FAIL(不是 SKIP)。
TEST(Sky, RealAssets_CubeMapLoadsAndRenders) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  // 1024² RGBA8 每面 4 MiB,六面 = 一共 24 MiB 图像;Host 页 8 MiB → 预留 4 页。
  const std::string dir = asset_cube_map_dir();
  reserve_pages(*d.value(), 4);

  auto cube = mbp::create_cube_map_from_faces(*d.value(), dir, kDefaultScene);
  ASSERT_TRUE(cube.ok())
      << "六面 PNG 必须能加载(资源根 = " << dir << ")。错误: " << cube.message()
      << "\n  提示:设置环境变量 MB_ASSET_ROOT 指向 res/ 目录。";
  EXPECT_EQ(cube.value()->desc().extent.width, 1024u);
  EXPECT_EQ(cube.value()->face_size(), 1024ull * 1024ull * 4ull);
  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(cube.value()->face_uploaded(static_cast<res::CubeFace>(i)))
        << "第 " << i << " 面必须已上传";
  }

  // 内存证据:整块图像页
  res::IMemoryAllocator* alloc = d.value()->memory_allocator();
  ASSERT_NE(alloc, nullptr);
#if defined(_WIN32)
  if (auto* vk_cube = dynamic_cast<magicblocks::backend::VulkanCubeMap*>(cube.value().get())) {
    const dev::u32 page_id = vk_cube->image_page_id();
    EXPECT_NE(page_id, res::ResourceId::kInvalidPage) << "必须有一张整块图像页";
    res::PageDesc pd{};
    ASSERT_TRUE(alloc->page_desc(page_id, &pd)) << "图像页必须登记在分配器里";
    EXPECT_GE(pd.page_size, vk_cube->total_bytes());
    EXPECT_EQ(vk_cube->total_bytes(), 1024ull * 1024ull * 4ull * 6ull);
  }
#endif

  const auto cam = make_camera({0.0f, 1.0f, 0.0f}, {0.0f, 1.5f, -1.0f});
  const Render sky = render_frame(*d.value(), cube.value().get(), cam, true);
  ASSERT_TRUE(sky.ok) << sky.error;

  // 真实天空盒:**带内容**的贴图。色种数远大于 1,主色占比远小于 1。
  const std::size_t colors = distinct_colors(sky.px);
  const double share = top_color_share(sky.px);
  EXPECT_GT(colors, 200u) << "真实天空盒的背景色种数应当很多(实测 " << colors << ")";
  EXPECT_LT(share, 0.5) << "真实天空盒的主色占比不应接近 1(实测 " << share << ")";
  EXPECT_EQ(count_clear_pixels(sky.px), 0u) << "真实天空必须铺满背景";

  // 确定性
  const Render sky2 = render_frame(*d.value(), cube.value().get(), cam, true);
  ASSERT_TRUE(sky2.ok) << sky2.error;
  EXPECT_EQ(std::memcmp(sky.px.data(), sky2.px.data(), sky.px.size()), 0)
      << "真实贴图路径两次渲染也必须逐字节相同";

  // 平移不变
  const auto moved = make_camera({7.0f, -2.0f, 5.0f}, {7.0f, -1.5f, 4.0f});
  const Render sky3 = render_frame(*d.value(), cube.value().get(), moved, true);
  ASSERT_TRUE(sky3.ok) << sky3.error;
  EXPECT_EQ(diff_pixels(sky.px, sky3.px, 0), 0u) << "真实贴图同样必须平移不变";

  // 旋转必须变
  const auto turned = make_camera({0.0f, 1.0f, 0.0f}, {1.0f, 1.5f, 0.0f});
  const Render sky4 = render_frame(*d.value(), cube.value().get(), turned, true);
  ASSERT_TRUE(sky4.ok) << sky4.error;
  EXPECT_GT(diff_pixels(sky.px, sky4.px, kBgTol), 0u) << "真实贴图转 90° 后画面必须变化";
}

// 开关差异必须**覆盖背景区域**而不只是角落:差异包围盒要接近整图。
TEST(Sky, RealAssets_ToggleDiffCoversBackgroundBox) {
  auto d = make_device();
  ASSERT_TRUE(d.ok()) << d.message();
  reserve_pages(*d.value(), 4);
  auto cube = mbp::create_cube_map_from_faces(*d.value(), asset_cube_map_dir(), kDefaultScene);
  ASSERT_TRUE(cube.ok()) << cube.message();

  const auto cam = make_camera({0.0f, 1.0f, 0.0f}, {0.0f, 1.5f, -1.0f});
  const Render off = render_frame(*d.value(), cube.value().get(), cam, false);
  ASSERT_TRUE(off.ok) << off.error;
  const Render on = render_frame(*d.value(), cube.value().get(), cam, true);
  ASSERT_TRUE(on.ok) << on.error;

  const std::size_t total = static_cast<std::size_t>(kW) * kH;
  std::size_t x0 = kW, y0 = kH, x1 = 0, y1 = 0, n = 0;
  for (dev::u32 y = 0; y < kH; ++y) {
    for (dev::u32 x = 0; x < kW; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 4;
      const int dr = std::abs(static_cast<int>(off.px[i]) - static_cast<int>(on.px[i]));
      const int dg = std::abs(static_cast<int>(off.px[i + 1]) - static_cast<int>(on.px[i + 1]));
      const int db = std::abs(static_cast<int>(off.px[i + 2]) - static_cast<int>(on.px[i + 2]));
      if (dr > kBgTol || dg > kBgTol || db > kBgTol) {
        ++n;
        x0 = std::min<std::size_t>(x0, x);
        y0 = std::min<std::size_t>(y0, y);
        x1 = std::max<std::size_t>(x1, x + 1);
        y1 = std::max<std::size_t>(y1, y + 1);
      }
    }
  }
  ASSERT_GT(n, 0u) << "开/关两图必须不同";
  const double bbox_area_ratio =
      static_cast<double>((x1 - x0) * (y1 - y0)) / static_cast<double>(total);
  EXPECT_GT(bbox_area_ratio, 0.95)
      << "差异包围盒必须接近整图面积(实测 bbox " << x0 << "," << y0 << " -> " << x1 << ","
      << y1 << " 占 " << bbox_area_ratio << ")。只变一角说明天空盒没铺满背景。";
  EXPECT_EQ(n, total) << "开/关两图必须在**每一个**背景像素上都不同";
}
