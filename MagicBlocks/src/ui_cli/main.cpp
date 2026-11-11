// MagicBlocks UI 消费层 CLI —— 离屏出图 / 统计打印(spec §10 验收门 5)
//
// 用法:
//   magicblocks-ui-cli render --width W --height H [--feature ID] --output out.png
//       走 spec §5.5 离屏路径:HomeScreen(或指定玩法)+ HUD → 读回 → PNG。
//       这是 golden 基线(`tests/data/golden_ui.png`)的产出方式。
//
//   magicblocks-ui-cli stats --width W --height H [--frames N] [--feature ID]
//       打印 HUD 合批统计 / 图元数 / 帧阶段序列(JSON),供 CTest 断言。
//       不落盘,不出图。
//
//   magicblocks-ui-cli present --width W --height H [--feature ID]
//       走**主路径**(TargetMode::Swapchain,无窗口 headless surface)驱动 Presenter
//       状态机,打印逐帧阶段与状态。用于验证"每帧恰一次 submit"这条约束在
//       链路上真的被记账(spec §10 验收门 2 的 CLI 侧旁证)。
//
// 退出码:0 成功 · 1 运行失败 · 2 参数错误。
//
// 注意:本 CLI 是**离屏/统计工具**,不是壳层;它不含任何窗口或输入循环。
// 真正的壳层(Android NativeActivity / Win32)另起,复用同一个 `ui::UiApp`。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_target.h"
#include "backend/vulkan/vulkan_ui_overlay.h"
#include "core/image_io.h"
#include "magicblocks/device/render_device.h"
#include "magicblocks/primitives/model_catalog.h"
#include "magicblocks/resources/memory.h"
#include "magicblocks/ui/hud.h"
#include "magicblocks/ui/router.h"
#include "magicblocks/ui/ui_app.h"
#include "render_backend.h"
#include "ui/offscreen_target.h"
#include "ui/ui_app_internal.h"

namespace {

namespace ui = magicblocks::ui;
namespace dev = magicblocks::device;
namespace mbp = magicblocks::primitives;
namespace res = magicblocks::resources;
namespace ui_cli = magicblocks::ui_cli;

void print_usage() {
  std::fprintf(stderr,
               "用法:\n"
               "  magicblocks-ui-cli render  --width W --height H [--feature ID] --output FILE\n"
               "                             [--left-items a,b,c] [--right-items x,y]\n"
               "                             [--left-selected N] [--right-selected N]\n"
               "                             [--right-first-row N] [--enter-stub]\n"
               "  magicblocks-ui-cli stats   --width W --height H [--frames N] [--feature ID]\n"
               "  magicblocks-ui-cli present --width W --height H [--feature ID]\n"
               "\n"
               "  --msaa N   离屏 UI 目标的采样数(1=关闭,缺省;可用设备支持的 2/4/8)。\n"
               "             这是 UI 路径上唯一的 MSAA 入口:界面边缘的抗锯齿由它决定。\n"
               "\n"
               "侧栏条目(--left-items/--right-items)由本 CLI 作为『外部』注入:\n"
               "  UI 层只负责画字符串并回调下标,不认识任何模型名或玩法名(spec §2)。\n");
}

// 逗号分隔 → 条目文案列表(空段丢弃)。
std::vector<std::string> split(const std::string& s, char sep = ',') {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == sep) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// 侧栏默认条目:仅用于 CLI 出图演示。**这里出现名字是可接受的**:CLI 是
// "外部注入方",UI 库本身(src/ui、include/magicblocks/ui)一行都没有这些词。
ui::SidebarConfig default_sidebar() {
  ui::SidebarConfig sb;
  sb.left_title = "models";
  sb.right_title = "play";
  sb.back_button_label = "back";
  sb.left_items = {"cube", "quad", "sphere", "cylinder"};
  sb.right_items = {"slot0", "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7"};
  return sb;
}

// HomeScreen 的入口:注册一个空玩法(FeatureHost 骨架,不实现任何玩法内容,
// spec §4「本轮只建立 HomeScreen 入口 + 空 FeatureHost 骨架」)。
// 玩法 ID 在这里由 CLI 决定(UI 层不认识具体玩法名),UI 源码里不出现任何玩法名。
class EmptyFeatureHost final : public ui::IFeatureHost {
 public:
  explicit EmptyFeatureHost(bool* entered) : entered_(entered) {}
  void on_enter(const ui::FeatureContext&) override {
    if (entered_) *entered_ = true;
  }
  void on_exit() override {}
  void on_update(float) override {}
  bool on_input(const dev::InputEvent&) override { return false; }

 private:
  bool* entered_ = nullptr;
};

struct Args {
  std::string cmd;
  ui::u32 width = 480;
  ui::u32 height = 320;
  ui::u32 frames = 1;
  std::string feature;
  std::string output;
  // UI 布局 v1:侧栏条目由 CLI(即"外部")注入,UI 层不认识具体名字。
  std::vector<std::string> left_items;
  std::vector<std::string> right_items;
  ui::u32 left_selected = 0;
  ui::u32 right_selected = 0;
  ui::u32 right_first_row = 0;
  bool enter_stub = false;   // 进右侧下标 right_selected 的空白 FeatureHost 再出图
  // ---- UI 布局 v2 ----
  // 资源根(含 cubeMap/ 与 models/ 的目录)。空 = 取 MB_ASSET_ROOT。
  std::string asset_root;
  // 天空盒场景名。空字符串 = 不开天空盒。
  std::string sky_scene;
  // 左列表是否用真实模型目录(models/) + 渲染缩略图(v2 的主路径)。
  bool use_models = true;
  // 缩略图尺寸(像素)。
  ui::u32 thumb_w = 48;
  ui::u32 thumb_h = 36;
  // ---- MSAA(t6 / R1 + R2)----
  // UI 离屏目标的采样数。缺省 1 = 不开启,**不加该参数时出图与引入前逐字节一致**。
  // 这是用户唯一能看到的界面路径上的 MSAA 入口 —— 之前整个 UI 层没有它。
  ui::u32 msaa = 1;
};

bool parse_args(int argc, char** argv, Args* out) {
  if (argc < 1) return false;
  out->cmd = argv[0];
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s 缺少取值\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--width") {
      const char* v = next("--width");
      if (!v) return false;
      out->width = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--height") {
      const char* v = next("--height");
      if (!v) return false;
      out->height = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--frames") {
      const char* v = next("--frames");
      if (!v) return false;
      out->frames = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--msaa") {
      const char* v = next("--msaa");
      if (!v) return false;
      out->msaa = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--feature") {
      const char* v = next("--feature");
      if (!v) return false;
      out->feature = v;
    } else if (a == "--output") {
      const char* v = next("--output");
      if (!v) return false;
      out->output = v;
    } else if (a == "--left-items") {
      // 逗号分隔的条目文案(外部注入;UI 层不认识这些名字)
      const char* v = next("--left-items");
      if (!v) return false;
      out->left_items = split(v);
    } else if (a == "--right-items") {
      const char* v = next("--right-items");
      if (!v) return false;
      out->right_items = split(v);
    } else if (a == "--left-selected" || a == "--right-selected" || a == "--right-first-row") {
      const char* v = next(a.c_str());
      if (!v) return false;
      const ui::u32 n = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
      if (a == "--left-selected") out->left_selected = n;
      else if (a == "--right-selected") out->right_selected = n;
      else out->right_first_row = n;
    } else if (a == "--enter-stub") {
      out->enter_stub = true;
    } else if (a == "--asset-root") {
      const char* v = next("--asset-root");
      if (!v) return false;
      out->asset_root = v;
    } else if (a == "--sky-scene") {
      const char* v = next("--sky-scene");
      if (!v) return false;
      out->sky_scene = v;
    } else if (a == "--no-models") {
      out->use_models = false;
    } else if (a == "--thumb-size") {
      const char* v = next("--thumb-size");
      if (!v) return false;
      std::sscanf(v, "%ux%u", &out->thumb_w, &out->thumb_h);
      if (out->thumb_w == 0 || out->thumb_h == 0) return false;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return false;
    }
  }
  return out->width > 0 && out->height > 0 && out->frames > 0;
}

// 资源根解析:显式参数 > 环境变量 MB_ASSET_ROOT > 仓库内 assets/res。
// 全部落空时返回空串,由调用方报错(不是"悄悄用当前目录")。
std::string resolve_asset_root(const std::string& explicit_root) {
  if (!explicit_root.empty()) return explicit_root;
  if (const char* env = std::getenv("MB_ASSET_ROOT"); env && *env) return env;
  return "assets/res";
}

dev::DeviceDesc sw_desc() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks UI CLI";
  d.require_software_rasterizer = true;  // 强筛,绝不回退物理 GPU
  return d;
}

// 装配:设备 → 离屏目标 → 后端帧渲染器 → UI 层。
struct Session {
  std::unique_ptr<dev::IRenderDevice> device;
  std::unique_ptr<dev::IRenderTarget> target;
  std::unique_ptr<magicblocks::backend::VulkanFrameRenderer> renderer;
  std::unique_ptr<ui::UiApp> app;
  std::unique_ptr<ui::OffscreenTargetRenderer> offscreen_host;

  // 状态存储(退出保存 / 重进恢复,spec §4 IStateStore)
  ui::MemoryStateStore store;
  bool feature_entered = false;

  // ---- UI 布局 v2 ----
  // 缩略图(与左列表条目一一对应,顺序相同)。渲染失败也保留(带 error)。
  std::vector<ui_cli::Thumbnail> thumbnails;
  // 场景渲染共用的 GPU 资源(管线 + 实例缓冲)。缩略图与场景共用,
  // 因为分配器不自动扩页 —— 每张缩略图各建一条会在第 5 张撞满 64 KiB 槽位。
  std::unique_ptr<ui_cli::SceneResources> scene_res;
  // 天空盒(未开启时 pass 为空,保持原清屏行为)
  ui_cli::SkySetup sky;
  // 选中模型的路径/名字(场景渲染用)
  std::string selected_model_path;
  std::string selected_model_name;
};

bool build_session(const Args& args, Session* s) {
  auto dev_r = dev::create_render_device(sw_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败: %s\n", dev_r.message().c_str());
    return false;
  }
  s->device = dev_r.take();

  // ---- 预取分配器页(v2 新增) ----
  //
  // 分配器**不会**自动扩页:桶里没空位时 `create_buffer`/`create_texture` 直接
  // 返回 OOM("请先 reserve_pages() 预取整页")。v1 只画 HUD、不需要 GPU 缓冲,
  // 所以没做这一步;v2 要缩略图 + 场景 + 天空盒,必须先把页要下来。
  //
  // 页大小(见 README):设备页 64 MiB / 1024 个 64 KiB 子块;Host 页 8 MiB。
  // 纹理页 8 MiB —— 一个 1024² 天空盒六面 = 24 MiB,需要 3 页,预取 4 页。
  res::IMemoryAllocator* alloc = s->device->memory_allocator();
  if (alloc == nullptr) {
    std::fprintf(stderr, "内部错误: 没有内存分配器\n");
    return false;
  }
  for (res::MemoryKind k :
       {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
    if (auto st = alloc->reserve_pages(k, 4); !st.ok()) {
      std::fprintf(stderr, "预取页失败: %s\n", st.message.c_str());
      return false;
    }
  }
  if (!args.sky_scene.empty()) {
    if (auto st = alloc->reserve_pages(res::MemoryKind::Texture, 4); !st.ok()) {
      std::fprintf(stderr, "预取纹理页失败: %s\n", st.message.c_str());
      return false;
    }
  }

  // 场景资源(缩略图与场景共用一条实例化管线 + 一张实例缓冲)
  s->scene_res = ui_cli::SceneResources::create(*s->device, 1, /*has_depth=*/true);
  if (!s->scene_res->ok()) {
    std::fprintf(stderr, "场景资源装配失败: %s\n", s->scene_res->error().c_str());
    return false;
  }

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {args.width, args.height};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  // t6 / R1+R2:UI 离屏目标的采样数。**不降级** —— 请求了设备不支持的值
  // 就明确报错退出,绝不悄悄退回 1 采样(那正是"开了没效果"的来源)。
  {
    const ui::u32 want = args.msaa == 0 ? 1u : args.msaa;
    const dev::DeviceCaps caps = s->device->caps();
    bool supported = false;
    for (dev::u32 lv : caps.supported_msaa_levels) {
      if (lv == want) supported = true;
    }
    if (!supported) {
      std::fprintf(stderr,
                   "--msaa %u 不被设备支持(设备: %s)。支持列表:", want,
                   caps.device_name.c_str());
      for (dev::u32 lv : caps.supported_msaa_levels) std::fprintf(stderr, " %u", lv);
      std::fprintf(stderr, "\n");
      return false;
    }
    td.samples = want;
  }
  auto tgt_r = s->device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return false;
  }
  s->target = tgt_r.take();

  auto* off = dynamic_cast<magicblocks::backend::OffscreenTarget*>(s->target.get());
  if (!off) {
    std::fprintf(stderr, "内部错误:离屏目标类型不符\n");
    return false;
  }

  auto ctx = static_cast<magicblocks::backend::VulkanRenderDevice*>(s->device.get())->context();
  s->renderer = std::make_unique<magicblocks::backend::VulkanFrameRenderer>(ctx, off);
  s->offscreen_host = std::make_unique<ui::OffscreenTargetRenderer>();
  if (!s->offscreen_host->attach(s->renderer.get())) {
    std::fprintf(stderr, "内部错误:离屏后端 attach 失败\n");
    return false;
  }

  ui::UiAppConfig cfg;
  cfg.display = ui::DisplayInfo{args.width, args.height, 1.f};
  cfg.mode = ui::TargetMode::Offscreen;
  cfg.scene_frame.mode = ui::TargetMode::Offscreen;
  // 离屏出图必须与 tests/ui_test.cpp 里的 test_hud_cfg() 用**同一套** HUD 布局参数,
  // 否则 UI golden(tests/data/golden_ui.png)由 CLI 生成、却由测试比对,
  // 两边几何不同 → 永远对不上。这里显式钉住,不依赖 HudLayoutConfig 的默认值。
  cfg.hud.margin_dp = 8.f;
  cfg.hud.row_height_dp = 32.f;
  cfg.hud.button_width_dp = 80.f;
  cfg.hud.gap_dp = 6.f;
  cfg.hud.panel_padding_dp = 6.f;

  // ---- UI 布局 v2:左列表接**真实模型目录** ----
  //
  // 扫描与列表来源在这里(CLI = 外部装配方)。UI 层仍然只拿到一串名字。
  // 目录不存在 → **明确失败并返回 false**(调用方 exit 2),不静默退化成
  // 空列表 —— 空列表与"目录找不到"在界面上是两件完全不同的事。
  const std::string asset_root = resolve_asset_root(args.asset_root);
  std::vector<std::string> model_names;
  std::vector<std::string> model_paths;
  if (args.use_models) {
    const std::string models_dir = asset_root + "/models";
    auto entries = mbp::scan_model_dir(models_dir);
    if (!entries.ok()) {
      std::fprintf(stderr, "扫描模型目录失败(%s): %s\n", models_dir.c_str(),
                   entries.message().c_str());
      return false;
    }
    for (const auto& e : entries.value()) {
      // 扫描失败的条目**照样进列表**(catalog 纪律),但在 UI 上禁用
      if (!e.ok) {
        std::fprintf(stderr, "模型条目不可用,仍列入(禁用): %s (%s)\n", e.name.c_str(),
                     e.message.c_str());
      }
      model_names.push_back(e.name);
      model_paths.push_back(e.path);
    }
  }

  // 侧栏条目:模型目录优先;显式 --left-items 覆盖(便于测试/对照)。
  cfg.sidebar = default_sidebar();
  if (!model_names.empty()) {
    cfg.sidebar.left_items = model_names;
    cfg.sidebar.left_title = "models";
  }
  if (!args.left_items.empty()) cfg.sidebar.left_items = args.left_items;
  if (!args.right_items.empty()) cfg.sidebar.right_items = args.right_items;
  cfg.sidebar.left_selected = args.left_selected;
  cfg.sidebar.right_selected = args.right_selected;
  cfg.sidebar.right_first_row = args.right_first_row;

  ui::UiAppDeps deps;
  // 离屏模式没有壳层 surface:场景 sink 为空,SceneFrameDriver 会按 G7 跳过场景帧。
  // CLI 关心的是 UI 图元(由 OffscreenTargetRenderer 单独出图),不是场景上屏。
  deps.scene_sink = nullptr;
  deps.state_store = &s->store;

  s->app = std::make_unique<ui::UiApp>();
  std::string err;
  if (!s->app->init(cfg, deps, &err)) {
    std::fprintf(stderr, "UI 层初始化失败: %s\n", err.c_str());
    return false;
  }

  // 注册空 FeatureHost 骨架(--feature 指定 ID 时才有意义)
  if (!args.feature.empty()) {
    bool* entered = &s->feature_entered;
    s->app->register_feature(ui::FeatureRegistration{
        ui::FeatureId(args.feature),
        [entered]() { return std::make_unique<EmptyFeatureHost>(entered); }});
    if (s->app->router().push(ui::FeatureId(args.feature)) != ui::RouteAction::Pushed) {
      std::fprintf(stderr, "进入玩法失败(--feature %s)\n", args.feature.c_str());
      return false;
    }
  }

  // 右列表 -> 空白 FeatureHost 骨架:每条一个独立 ID(由 CLI 生成,UI 层不认识),
  // 点中即经 Router 进入;没有任何玩法逻辑,符合"本轮只做空桩"。
  bool* entered = &s->feature_entered;
  for (std::size_t i = 0; i < cfg.sidebar.right_items.size(); ++i) {
    const std::string id = "slot" + std::to_string(i);
    s->app->register_feature(ui::FeatureRegistration{
        ui::FeatureId(id),
        [entered]() { return std::make_unique<EmptyFeatureHost>(entered); }});
    if (!s->app->bind_sidebar_route(static_cast<ui::u32>(i), ui::FeatureId(id))) {
      std::fprintf(stderr, "绑定右列表下标 %zu 失败\n", i);
      return false;
    }
  }

  // --enter-stub:模拟"点了右侧第 N 条" -> 经 Router 进空桩(用于像素断言高亮态)
  if (args.enter_stub) {
    const std::string id = "slot" + std::to_string(args.right_selected);
    if (s->app->router().push(ui::FeatureId(id)) != ui::RouteAction::Pushed) {
      std::fprintf(stderr, "进入空桩失败(slot%u 未注册?)\n",
                   static_cast<unsigned>(args.right_selected));
      return false;
    }
  }

  // ---- UI 布局 v2:渲染缩略图(左列表条目的图) ----
  //
  // 只在"左列表确实来自模型目录"时渲染:--left-items 是纯文本对照模式,
  // 没有对应文件,强行渲染只会给出一堆失败缩略图。
  if (args.use_models && !model_paths.empty() && args.left_items.empty()) {
    ui_cli::ThumbnailConfig tc;
    tc.width = args.thumb_w;
    tc.height = args.thumb_h;
    s->thumbnails =
        ui_cli::render_thumbnails(*s->device, *s->scene_res, model_names, model_paths, tc);
    std::size_t bad = 0;
    for (const auto& t : s->thumbnails) {
      if (!t.usable()) {
        ++bad;
        std::fprintf(stderr, "缩略图渲染失败[%s]: %s\n", t.name.c_str(), t.error.c_str());
      }
    }
    std::fprintf(stderr, "缩略图: %zu/%zu 成功(%ux%u)\n",
                 s->thumbnails.size() - bad, s->thumbnails.size(),
                 static_cast<unsigned>(args.thumb_w), static_cast<unsigned>(args.thumb_h));
  }

  // 选中模型 -> 场景预览(下标对齐左列表;越界则留空,只画天空/背景)
  if (args.use_models && !model_paths.empty() && args.left_items.empty()) {
    if (args.left_selected < model_paths.size()) {
      s->selected_model_path = model_paths[args.left_selected];
      s->selected_model_name = model_names[args.left_selected];
    }
  }

  // ---- UI 布局 v2:天空盒 ----
  //
  // 资源缺失 → 明确失败(exit 2),不静默退化成一屏纯色(那是 sky.h 契约 5)。
  if (!args.sky_scene.empty()) {
    s->sky = ui_cli::setup_sky(*s->device, asset_root, args.sky_scene);
    if (!s->sky.ok) {
      std::fprintf(stderr, "天空盒装配失败: %s\n", s->sky.error.c_str());
      return false;
    }
    std::fprintf(stderr, "天空盒: %s (%s)\n", s->sky.scene.c_str(), asset_root.c_str());
  }
  return true;
}

// 把一张缩略图按"源 over 目标"贴到目标 RGBA8 缓冲的 dst 矩形里。
//
// 为什么在 CPU 上贴而不是走 overlay 的 blit():`TextureHandle` 目前只有
// `kNoTexture`(纹理图集未落地),走 blit 就得改 backend 的纹理绑定 —— 那是
// skybox 的文件。离屏路径本来就是 spec §5.5 允许读回的口子,这里沿用。
//
// 越界一律裁剪到目标缓冲内(不越界写);缩略图比目标小时按最近邻放大,
// 保证"条目里一定看得到模型的形状"而不是一个小点。
void composite_thumbnail(const ui_cli::Thumbnail& t, const ui::Rect& dst,
                         ui::u32 frame_w, ui::u32 frame_h, std::uint8_t* pixels) {
  if (!t.usable() || dst.empty()) return;
  const int x0 = static_cast<int>(dst.x < 0.f ? 0.f : dst.x);
  const int y0 = static_cast<int>(dst.y < 0.f ? 0.f : dst.y);
  const int x1 = static_cast<int>(dst.right() > static_cast<float>(frame_w)
                                      ? static_cast<float>(frame_w)
                                      : dst.right());
  const int y1 = static_cast<int>(dst.bottom() > static_cast<float>(frame_h)
                                      ? static_cast<float>(frame_h)
                                      : dst.bottom());
  if (x1 <= x0 || y1 <= y0) return;

  const float sx = static_cast<float>(t.width) / static_cast<float>(x1 - x0);
  const float sy = static_cast<float>(t.height) / static_cast<float>(y1 - y0);
  for (int y = y0; y < y1; ++y) {
    const int ty = static_cast<int>((static_cast<float>(y - y0) + 0.5f) * sy);
    if (ty < 0 || ty >= static_cast<int>(t.height)) continue;
    for (int x = x0; x < x1; ++x) {
      const int tx = static_cast<int>((static_cast<float>(x - x0) + 0.5f) * sx);
      if (tx < 0 || tx >= static_cast<int>(t.width)) continue;
      const std::size_t si =
          (static_cast<std::size_t>(ty) * t.width + static_cast<std::size_t>(tx)) * 4u;
      const std::size_t di =
          (static_cast<std::size_t>(y) * frame_w + static_cast<std::size_t>(x)) * 4u;
      const float a = static_cast<float>(t.rgba[si + 3]) / 255.f;
      if (a <= 0.f) continue;
      for (int c = 0; c < 3; ++c) {
        const float src = static_cast<float>(t.rgba[si + c]);
        const float cur = static_cast<float>(pixels[di + c]);
        pixels[di + c] = static_cast<std::uint8_t>(src * a + cur * (1.f - a));
      }
      pixels[di + 3] = 255;
    }
  }
}

int cmd_render(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  Session s;
  if (!build_session(args, &s)) return 1;

  magicblocks::ImageRgba8 img;
  img.width = args.width;
  img.height = args.height;
  img.pixels.assign(img.byte_size(), 0);

  // 推进一帧让 Router/Governor 状态到位,再离屏出图(§5.5)
  s.app->run_frame(0.f, 0.f);
  if (!s.offscreen_host->present(*s.app, args.width, args.height, img.pixels.data(),
                                 args.width * 4)) {
    std::fprintf(stderr, "离屏渲染失败\n");
    return 1;
  }

  // ---- UI 布局 v2:把缩略图贴进左列表条目 ----
  //
  // 条目几何从 HUD 读(不自己算坐标):UI 层的布局是唯一事实来源,
  // CLI 只消费它。缩略图与条目按**下标**一一对应。
  // 场景大图在下一步画到**两侧面板之外**的右侧空白区,所以这里先算出那块矩形。
  const ui::SidebarIds ids = s.app->sidebar_ids();
  const ui::ListColumnLayout lay = s.app->hud().list_layout(ids.left_panel);
  ui::Rect scene_area;  // 场景大图区域(右侧面板之外);空 = 不画场景
  {
    // 面板矩形取自 HUD(唯一事实来源)。实测 480x360 时左面板 x=[8,108]、
    // 右面板 x=[115,215] —— 两侧之间只有 6px,画不下场景,所以场景必须放到
    // **两块面板的并集之外**(右侧剩余区域),而不是"夹在中间"。
    const ui::Control* left = s.app->hud().find(ids.left_panel);
    const ui::Control* right = s.app->hud().find(ids.right_panel);
    if (left != nullptr && right != nullptr && !left->rect_px.empty() &&
        !right->rect_px.empty()) {
      const float panels_right = left->rect_px.right() > right->rect_px.right()
                                     ? left->rect_px.right()
                                     : right->rect_px.right();
      const float panels_top = left->rect_px.y < right->rect_px.y ? left->rect_px.y
                                                                  : right->rect_px.y;
      const float panels_bottom = left->rect_px.bottom() > right->rect_px.bottom()
                                      ? left->rect_px.bottom()
                                      : right->rect_px.bottom();
      const float gap = 8.f;
      const float x0 = panels_right + gap;
      const float w = static_cast<float>(args.width) - x0;
      const float top = panels_top > gap ? panels_top : gap;
      const float bottom = static_cast<float>(args.height) - gap;
      if (w > 32.f && bottom > top) {
        scene_area = ui::Rect{x0, top, w, bottom - top};
      }
    }
  }

  // ---- UI 布局 v2:场景预览(天空盒 + 选中模型) ----
  //
  // **在贴缩略图之前**画场景:场景只写 scene_area 这块矩形,缩略图写在条目里,
  // 两者不重叠;先生成场景可以让"缩略图压在场景上"的顺序天然正确(即使将来
  // 两个矩形有交叠,条目内容也一定在上面)。
  if (!scene_area.empty() && (!s.selected_model_path.empty() || s.sky.ok)) {
    ui_cli::SceneRenderConfig sc;
    sc.width = args.width;
    sc.height = args.height;
    sc.area_x = static_cast<ui::u32>(scene_area.x);
    sc.area_y = static_cast<ui::u32>(scene_area.y);
    sc.area_width = static_cast<ui::u32>(scene_area.w);
    sc.area_height = static_cast<ui::u32>(scene_area.h);
    sc.draw_sky = s.sky.ok;
    std::string serr;
    if (!ui_cli::render_scene_area(*s.device, *s.scene_res, s.sky.pass.get(),
                                   s.sky.cube.get(), s.selected_model_path,
                                   s.selected_model_name, sc,
                                   img.pixels.data(), static_cast<std::size_t>(args.width) * 4u,
                                   &serr)) {
      // 场景渲染失败**不让整张图失败**:显式记下来,报告里能看到
      std::fprintf(stderr, "场景渲染失败: %s\n", serr.c_str());
    } else {
      std::fprintf(stderr, "场景: %s%s @ (%u,%u %ux%u)\n",
                   s.selected_model_name.empty() ? "(无模型)" : s.selected_model_name.c_str(),
                   s.sky.ok ? " + 天空盒" : "", sc.area_x, sc.area_y, sc.area_width,
                   sc.area_height);
    }
  }

  if (!s.thumbnails.empty()) {
    const ui::Control* panel = s.app->hud().find(ids.left_panel);
    if (panel != nullptr) {
      std::size_t pasted = 0;
      for (std::size_t i = 0; i < s.thumbnails.size() && i < ids.left_items.size(); ++i) {
        const ui::Control* item = s.app->hud().find(ids.left_items[i]);
        if (item == nullptr || item->rect_px.empty()) continue;  // 被滚动裁掉
        // 缩略图贴在条目左侧,高度内缩 2px,宽度按缩略图纵横比
        ui::Rect dst = item->rect_px;
        const float inset = 2.f;
        dst.y += inset;
        dst.h -= inset * 2.f;
        const float aspect = static_cast<float>(args.thumb_w) /
                             static_cast<float>(args.thumb_h);
        dst.w = dst.h * aspect;
        // 避开条目左侧的 3px 强调条
        dst.x += 4.f;
        if (dst.right() > lay.viewport.right()) {
          dst.w = lay.viewport.right() - dst.x;
        }
        if (dst.w <= 0.f) continue;
        composite_thumbnail(s.thumbnails[i], dst, args.width, args.height,
                            img.pixels.data());
        ++pasted;
      }
      std::fprintf(stderr, "缩略图合成: %zu 条\n", pasted);
    }
  }

  std::string err;
  if (!magicblocks::write_png_rgba8(args.output, img, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::printf("wrote %s (%ux%u)\n", args.output.c_str(), args.width, args.height);
  return 0;
}

int cmd_stats(const Args& args) {
  Session s;
  if (!build_session(args, &s)) return 1;

  for (ui::u32 i = 0; i < args.frames; ++i) {
    s.app->run_frame(0.f, 0.f);
  }
  const auto& ov = ui::as_internal(s.app.get())->overlay();
  const auto st = ov.compute_stats();
  const auto dc = s.app->hud().draw_counts();

  std::printf("{\n");
  std::printf("  \"mode\": \"offscreen\",\n");
  std::printf("  \"feature\": \"%s\",\n", args.feature.c_str());
  std::printf("  \"feature_entered\": %s,\n", s.feature_entered ? "true" : "false");
  std::printf("  \"hud_controls\": %zu,\n", s.app->hud().size());
  std::printf("  \"primitives\": %zu,\n", ov.primitive_count());
  std::printf("  \"batches\": %u,\n", st.batches);
  std::printf("  \"draw_calls\": %u,\n", st.draw_calls);
  std::printf("  \"vertices\": %u,\n", st.vertices);
  std::printf("  \"hud_quads\": %u,\n", dc.quads);
  std::printf("  \"hud_strokes\": %u,\n", dc.strokes);
  std::printf("  \"hud_text_runs\": %u,\n", dc.text_runs);
  std::printf("  \"tier\": \"%s\"\n", ui::to_string(s.app->governor().tier()));
  std::printf("}\n");
  return 0;
}

int cmd_present(const Args& args) {
  Session s;
  if (!build_session(args, &s)) return 1;

  // 路线变更后的语义：上屏归壳层。SDK 只负责把一帧场景渲染进
  // **壳层提供的 surface**（ISceneFrameSink）。CLI 没有壳层，也就没有 sink，
  // 因此状态是 NoSurface、submits=0 —— 这是**正确行为**，不是错误。
  //
  // 真正带 surface 的可运行验证在 magicblocks-sink-verify 里。
  std::printf("[");
  for (ui::u32 i = 0; i < args.frames; ++i) {
    const auto r = s.app->run_frame(0.f, 0.f);
    std::printf("%s{\"frame\": %llu, \"phase\": \"%s\", \"status\": \"%s\", "
                "\"submits\": %u, \"latency_frames\": %u}",
                i ? ", " : "", static_cast<unsigned long long>(r.frame_index),
                ui::to_string(r.phase), ui::to_string(r.status),
                static_cast<unsigned>(r.submits_this_frame),
                static_cast<unsigned>(r.input_latency_frames));
  }
  std::printf("]\n");

  if (s.app->scene_frame().last().status == ui::FrameStatus::NoSurface) {
    std::fprintf(stderr,
                 "说明：未提供 ISceneFrameSink，本帧跳过上屏（NoSurface）。\n"
                 "      上屏由壳层负责；带真实 surface 的验证见 magicblocks-sink-verify。\n");
  }

  const auto& gov = s.app->governor().stats();
  std::fprintf(stderr, "panels:\n%s", s.app->debug_panel().c_str());
  if (gov.submit_violations != 0) {
    std::fprintf(stderr, "submit 约束被破坏: %u 帧\n", gov.submit_violations);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  Args args;
  if (!parse_args(argc - 1, argv + 1, &args)) {
    print_usage();
    return 2;
  }
  if (args.cmd == "render") return cmd_render(args);
  if (args.cmd == "stats") return cmd_stats(args);
  if (args.cmd == "present") return cmd_present(args);
  print_usage();
  return 2;
}
