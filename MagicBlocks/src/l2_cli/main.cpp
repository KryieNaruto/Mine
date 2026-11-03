// MagicBlocks L2 CLI —— 场景 / 渲染图 / 命令模式 的离屏出图与录制回放
//
// 用法:
//   magicblocks-l2-cli record --scene demo --width W --height H --frame-index N
//                             [--camera X,Y,Z] [--events N]
//                             [--record cap.bin] --output out.png
//       建场景 → 跑渲染图 → 经 IRecorder 录制命令流 → 提交 → 出图 → PNG。
//       同时把命令流落成二进制(cap.bin)。golden 基线 `tests/data/golden_l2.png`
//       就是这条路径产出的。
//
//   magicblocks-l2-cli replay --replay cap.bin [--verify]
//       把流里的命令逐条重录进一张新命令列表,比对哈希与计数。
//
//   magicblocks-l2-cli reproduce --replay cap.bin [--verify]
//       从流里读出场景快照重建,再按同一命令序列复现,比对哈希。
//
//   magicblocks-l2-cli dump --replay cap.bin [--json]
//       打印流的 JSON(键序固定,两次输出字节相同)或人读摘要。
//
// 退出码:0 成功 · 1 运行失败 · 2 参数错误。
//
// **范围裁定(D16)**:`--scene` 只接受 `demo`。spec §5.4 的 sketch/blocks/voxel/stamp
// 是 L4 玩法预设,L2 阶段尚未存在,遇到它们一律退出码 2 并指明归属 —— 不静默忽略。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_target.h"
#include "core/image_io.h"
#include "magicblocks/device/render_device.h"
#include "magicblocks/scene/command_stream.h"
#include "magicblocks/scene/recorder.h"
#include "magicblocks/scene/render_graph.h"
#include "magicblocks/scene/scene.h"
#include "magicblocks/scene/submit_queue.h"
#include "scene/scene_internal.h"

// 内嵌 shader(由 glslc + embed_shaders.py 生成,在二进制目录里)。
// L2 用 l2_scene.vert/frag:实例化 4x4 色块,无几何着色器(G1/G2),
// 纯绿标定点 + 深蓝清屏,与 L0 的(纯红 + 黑)**逐像素可区分**。
#include "magicblocks_shaders.h"

namespace {

namespace sc = magicblocks::scene;
namespace dev = magicblocks::device;
namespace res = magicblocks::resources;

void print_usage() {
  std::fprintf(stderr,
               "用法:\n"
               "  magicblocks-l2-cli record    --scene demo --width W --height H "
               "--frame-index N\n"
               "                               [--camera X,Y,Z] [--events N] "
               "[--record FILE] --output FILE\n"
               "  magicblocks-l2-cli replay    --replay FILE [--verify]\n"
               "  magicblocks-l2-cli reproduce --replay FILE [--verify]\n"
               "  magicblocks-l2-cli dump      --replay FILE [--json]\n");
}

dev::DeviceDesc sw_desc() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks L2 CLI";
  d.require_software_rasterizer = true;  // 强筛,绝不回退物理 GPU
  return d;
}

// ---------------------------------------------------------------- 参数

struct Args {
  std::string cmd;
  std::string scene = "demo";
  bool scene_given = false;
  dev::u32 width = 320;
  dev::u32 height = 240;
  dev::u64 frame_index = 0;
  float camera[3] = {0.0f, 1.5f, 4.0f};
  dev::u32 events = 2;
  std::string record_path;
  std::string output;
  std::string replay_path;
  bool verify = false;
  bool json = false;
};

// 解析 X,Y,Z;格式不符 → false。
bool parse_vec3(const char* s, float out[3]) {
  return std::sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

// 返回:0 = 解析成功;-1 = 未知参数/格式错误(调用方映射成退出码 2)。
int parse_args(int argc, char** argv, Args* out) {
  if (argc < 1) return -1;
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
    if (a == "--scene") {
      const char* v = next("--scene");
      if (v == nullptr) return -1;
      out->scene = v;
      out->scene_given = true;
    } else if (a == "--width") {
      const char* v = next("--width");
      if (v == nullptr) return -1;
      out->width = static_cast<dev::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--height") {
      const char* v = next("--height");
      if (v == nullptr) return -1;
      out->height = static_cast<dev::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--frame-index") {
      const char* v = next("--frame-index");
      if (v == nullptr) return -1;
      out->frame_index = std::strtoull(v, nullptr, 10);
    } else if (a == "--camera") {
      const char* v = next("--camera");
      if (v == nullptr) return -1;
      if (!parse_vec3(v, out->camera)) {
        std::fprintf(stderr, "--camera 需要 X,Y,Z(如 0,1.5,4):%s\n", v);
        return -1;
      }
    } else if (a == "--events") {
      const char* v = next("--events");
      if (v == nullptr) return -1;
      out->events = static_cast<dev::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--record") {
      const char* v = next("--record");
      if (v == nullptr) return -1;
      out->record_path = v;
    } else if (a == "--output") {
      const char* v = next("--output");
      if (v == nullptr) return -1;
      out->output = v;
    } else if (a == "--replay") {
      const char* v = next("--replay");
      if (v == nullptr) return -1;
      out->replay_path = v;
    } else if (a == "--verify") {
      out->verify = true;
    } else if (a == "--json") {
      out->json = true;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return -1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------- 文件

bool read_file(const std::string& path, std::vector<std::uint8_t>* out, std::string* err) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    *err = "无法打开文件: " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    *err = "无法获取文件长度: " + path;
    return false;
  }
  out->assign(static_cast<std::size_t>(size), 0u);
  const std::size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  if (got != out->size()) {
    *err = "读取文件时长度不符: " + path;
    return false;
  }
  return true;
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes,
                std::string* err) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    *err = "无法写入文件: " + path;
    return false;
  }
  const std::size_t put = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (put != bytes.size()) {
    *err = "写入文件时长度不符: " + path;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 设备 / 目标

// 建一张离屏 RGBA8 目标(无深度)。
dev::Result<std::unique_ptr<dev::IRenderTarget>> make_offscreen(dev::IRenderDevice& device,
                                                                dev::u32 w, dev::u32 h) {
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {w, h};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  return device.create_offscreen_target(td);
}

// ---------------------------------------------------------------- 演示场景(S1)

// 建一棵确定性的演示场景:**不含任何玩法逻辑**,只是让 S1 的能力都被走到。
sc::SceneSnapshot build_demo_scene(sc::IScene& scene) {
  const sc::NodeId root = scene.add_node(sc::kInvalidNodeId, "root").value();
  {
    sc::Aabb box;
    box.empty = false;
    box.min[0] = -1.0f;
    box.min[1] = -1.0f;
    box.min[2] = -1.0f;
    box.max[0] = 1.0f;
    box.max[1] = 1.0f;
    box.max[2] = 1.0f;
    scene.set_local_bounds(root, box);
  }

  // 三个子块:不同 TRS,其中一个刻意不可见(覆盖 S1 的可见性连带)。
  for (int k = 0; k < 3; ++k) {
    const sc::NodeId id = scene.add_node(root, "block_" + std::to_string(k)).value();
    sc::Transform t;
    t.translation[0] = static_cast<float>(k) * 0.7f - 0.7f;
    t.translation[1] = 0.0f;
    t.translation[2] = 0.0f;
    t.scale[0] = 0.35f;
    t.scale[1] = 0.35f;
    t.scale[2] = 0.35f;
    scene.set_transform(id, t);

    sc::Aabb box;
    box.empty = false;
    box.min[0] = -0.5f;
    box.min[1] = -0.5f;
    box.min[2] = -0.5f;
    box.max[0] = 0.5f;
    box.max[1] = 0.5f;
    box.max[2] = 0.5f;
    scene.set_local_bounds(id, box);

    if (k == 2) scene.set_visible(id, false);  // 第三块不可见
  }
  scene.clear_changes();
  return scene.snapshot();
}

// ---------------------------------------------------------------- 管线

// 用内嵌 L2 shader 建一条图形管线。
//
// l2_scene.vert 按 gl_InstanceIndex 程序化生成色块、按 gl_VertexIndex 取
// unit quad 的 4 个角,因此**不需要顶点/索引缓冲**,顶点布局为空 ——
// 这正是 G2「无 GS,一切几何走实例化」的形态。
res::PipelineDesc demo_pipeline_desc() {
  res::PipelineDesc desc;
  desc.vertexLayout = res::VertexLayout{};
  res::ShaderModule vs;
  vs.stage = res::ShaderStage::Vertex;
  vs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL2_sceneVert);
  vs.size_bytes = magicblocks::shaders::kL2_sceneVertSize;
  res::ShaderModule fs;
  fs.stage = res::ShaderStage::Fragment;
  fs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL2_sceneFrag);
  fs.size_bytes = magicblocks::shaders::kL2_sceneFragSize;
  desc.stages = {vs, fs};
  desc.primitive = res::PrimitiveType::TriangleList;
  desc.raster.cull = res::CullMode::None;
  desc.depth.depthTest = false;  // 离屏单 pass,无深度附件
  desc.depth.depthWrite = false;
  // 16 字节 push constant。必须与 tests/magicblocks_l2_test.cpp 的同名函数
  // 逐字段一致,否则 `replay` 找不到 hash 相同的管线。
  desc.push_constant_bytes = 16;
  return desc;
}

// 把 PassDesc + 目标转成 PassRecord(供 record_pass / record_begin_pass 用)。
sc::PassRecord pass_record_of(const sc::PassDesc& pass, dev::IRenderTarget* target) {
  sc::PassRecord pr;
  pr.name = pass.name;
  pr.layer = pass.layer;
  pr.channel = pass.channel;
  pr.clear_color = pass.clear_color;
  for (int k = 0; k < 4; ++k) pr.clear_color_rgba[k] = pass.clear_color_rgba[k];
  pr.clear_depth = pass.clear_depth;
  pr.clear_depth_value = pass.clear_depth_value;
  pr.area[0] = pass.area_x;
  pr.area[1] = pass.area_y;
  pr.area[2] = pass.area_width;
  pr.area[3] = pass.area_height;
  if (target != nullptr) {
    pr.color_format = target->color_format();
    pr.has_depth = target->depth_attachment().valid;
  }
  return pr;
}

// ---------------------------------------------------------------- record

int cmd_record(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  if (args.width == 0 || args.height == 0) {
    std::fprintf(stderr, "宽高必须 > 0\n");
    return 2;
  }

  auto dev_r = dev::create_render_device(sw_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n",
                 static_cast<int>(dev_r.status().code), dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  auto tgt_r = make_offscreen(*device, args.width, args.height);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败(code=%d): %s\n",
                 static_cast<int>(tgt_r.status().code), tgt_r.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt_r.value().get();

  // S1 场景
  auto scene = sc::create_scene();
  const sc::SceneSnapshot before = build_demo_scene(*scene);

  // 录制器 + 管线(S5 / N5:on_attach 是 PipelineTable 的唯一填充者)
  auto recorder = sc::create_recorder();
  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  dev::Status as = recorder->on_attach(*device, att);
  if (!as.ok()) {
    std::fprintf(stderr, "on_attach 失败: %s\n", as.message.c_str());
    return 1;
  }

  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }
  res::ICommandList* cmd = cmd_r.value().get();

  // S2 提交队列(演示:三个块按深度/材质排序)
  auto queue = sc::create_submit_queue();
  for (int k = 0; k < 3; ++k) {
    sc::DrawItem item;
    item.tie_break_id = static_cast<dev::u64>(k);
    item.material = static_cast<dev::u32>(k);
    item.transparent = (k == 1);
    item.center[0] = static_cast<float>(k) * 0.7f - 0.7f;
    item.center[1] = 0.0f;
    item.center[2] = -static_cast<float>(k) * 0.5f;
    item.depth = static_cast<float>(3 - k) * 0.1f;
    item.mesh_key = static_cast<dev::u64>(k + 1);
    item.first_index = 0;
    item.index_count = 4;
    queue->add(item);
  }
  sc::SubmitCamera cam;
  cam.position[0] = args.camera[0];
  cam.position[1] = args.camera[1];
  cam.position[2] = args.camera[2];
  const std::vector<sc::DrawItem>& sorted = queue->sort(cam);
  (void)sorted;

  // S3 渲染图:一个 scene pass 写离屏目标(声明即校验)
  auto graph = sc::create_render_graph();
  sc::PassDesc pass;
  pass.name = "scene";
  pass.layer = static_cast<std::uint8_t>(sc::FeatureLayer::Scene);
  pass.clear_color = true;
  // 清成深蓝:与 UI golden 的底色和 L0 的黑色都不同,便于人眼区分。
  pass.clear_color_rgba[0] = 0.05f;
  pass.clear_color_rgba[1] = 0.08f;
  pass.clear_color_rgba[2] = 0.20f;
  pass.clear_color_rgba[3] = 1.0f;
  pass.clear_depth = false;
  pass.color_output = sc::AttachmentRef::make_external(target);
  pass.own_target = target;
  graph->add_pass(pass);
  const sc::GraphValidation gv = graph->validate();
  if (!gv.status.ok()) {
    std::fprintf(stderr, "渲染图校验失败: %s\n", gv.message.c_str());
    return 1;
  }

  // ---- 开始录制(S5)
  dev::Status bs = recorder->begin_frame(args.frame_index, {args.width, args.height}, *scene);
  if (!bs.ok()) {
    std::fprintf(stderr, "begin_frame 失败: %s\n", bs.message.c_str());
    return 1;
  }

  // 输入事件(S8:时间戳由调用方给,L2 不读时钟)
  for (dev::u32 k = 0; k < args.events; ++k) {
    sc::EventRecord ev;
    ev.kind = sc::EventKind::PointerMove;
    ev.pointer_id = 0;
    ev.x = static_cast<float>(k) * 13.0f;
    ev.y = static_cast<float>(k) * 7.0f;
    ev.dx = 13.0f;
    ev.dy = 7.0f;
    // 固定步长的时间戳:完全由 frame_index 与 k 推导,跨进程可复现。
    ev.timestamp_us = static_cast<std::int64_t>(args.frame_index) * 16666 +
                      static_cast<std::int64_t>(k) * 1000;
    ev.width = args.width;
    ev.height = args.height;
    ev.handled = true;
    dev::Status s = recorder->record_event(ev);
    if (!s.ok()) {
      std::fprintf(stderr, "record_event 失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  // pass 元信息(record_begin_pass 幂等,这里只登记一次)
  const sc::PassRecord pass_rec = pass_record_of(pass, target);

  auto record_one = [&](const char* what, dev::Status s) -> bool {
    if (!s.ok()) {
      std::fprintf(stderr, "录制 %s 失败: %s\n", what, s.message.c_str());
      return false;
    }
    return true;
  };

  // 命令 10:begin_pass
  {
    res::RenderPassDesc rp;
    rp.target = target;
    rp.clear_color = pass.clear_color;
    for (int k = 0; k < 4; ++k) rp.clear_color_rgba[k] = pass.clear_color_rgba[k];
    rp.clear_depth = pass.clear_depth;
    rp.area_x = pass.area_x;
    rp.area_y = pass.area_y;
    rp.area_width = pass.area_width;
    rp.area_height = pass.area_height;
    if (!record_one("begin_pass", recorder->record_begin_pass(*cmd, pass_rec, target, rp))) {
      return 1;
    }
  }

  // 命令 1:bind_pipeline(用 on_attach 建好并登记的那条)
  {
    const std::vector<std::uint64_t>& hashes = recorder->pipelines().hashes();
    res::IPipeline* pipeline =
        hashes.empty() ? nullptr : recorder->pipelines().find(hashes.front());
    if (pipeline == nullptr) {
      std::fprintf(stderr, "内部错误:PipelineTable 里没有已登记管线\n");
      return 1;
    }
    if (!record_one("bind_pipeline", recorder->record_bind_pipeline(*cmd, pipeline))) return 1;
  }

  // 命令 8 / 9:视口与裁剪
  {
    res::Viewport vp;
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(args.width);
    vp.height = static_cast<float>(args.height);
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    if (!record_one("set_viewport", recorder->record_set_viewport(*cmd, vp))) return 1;

    res::ScissorRect sr;
    sr.x = 0;
    sr.y = 0;
    sr.width = args.width;
    sr.height = args.height;
    if (!record_one("set_scissor", recorder->record_set_scissor(*cmd, sr))) return 1;
  }

  // 命令 4:draw(实例化 4x4 色块,一次 draw 覆盖整图)
  // 4 顶点 unit quad × 16 实例 —— 与 l2_scene.vert 一致。
  {
    res::DrawArgs da;
    da.vertex_count = 4;
    da.instance_count = 16;
    da.first_vertex = 0;
    da.vertex_offset = 0;
    da.first_instance = 0;
    if (!record_one("draw", recorder->record_draw(*cmd, da))) return 1;
  }

  // 命令 11:end_pass
  if (!record_one("end_pass", recorder->record_end_pass(*cmd))) return 1;

  // ---- capture(**必须在 submit 之前**:submit 会清空 L1 的录制流)
  auto cap_r = recorder->capture();
  if (!cap_r.ok()) {
    std::fprintf(stderr, "capture 失败: %s\n", cap_r.message().c_str());
    return 1;
  }
  sc::CommandStream stream = cap_r.take();

  // W4 强不变量:流里命令条数 == L1 命令计数
  if (stream.commands.size() != cmd->command_count()) {
    std::fprintf(stderr, "内部错误:命令流 %zu 条 != L1 计数 %u 条(W4 不变量)\n",
                 stream.commands.size(), cmd->command_count());
    return 1;
  }

  // ---- 提交(整帧录完后,每帧恰好一次)
  dev::Status ss = cmd->submit_and_wait();
  if (!ss.ok()) {
    std::fprintf(stderr, "提交失败(code=%d): %s\n", static_cast<int>(ss.code),
                 ss.message.c_str());
    return 1;
  }

  // ---- 出像素
  //
  // U1 裁定(spec §4.6.1.1):L1 的 `ICommandList::draw*` **只记录不下发**
  // `vkCmdDraw`,所以"经 IRecorder 录制的 draw"不产生像素。命令流的价值是
  // **结构化的可复现门** —— 它在 record / dump / replay 之间保持哈希一致
  // (见下面的 hash= 与 --verify)。
  //
  // 像素必须走**真正下发绘制**的路径:`OffscreenTarget::draw_selftest`
  // (backend 内部自建 render pass + vkCmdDraw,即 L0 golden 的来源)。
  //
  // 用**独立的出图目标**:上面录制的 begin_pass 已经在 `target` 上真的开过
  // render pass,其图像布局与 selftest 自建 pass 的 initialLayout=UNDEFINED
  // 冲突,复用同一张目标会踩布局不匹配。
  //
  // 实例相位固定取偶数:backend 的 selftest 用
  // `vkCmdDraw(cmd, 4, 16, 0, frame_index & 1)` 传 first_instance,
  // 奇数帧会把实例 0(纯绿标定点)挪走。golden 固定用 frame-index 0。
  {
    auto pix_r = make_offscreen(*device, args.width, args.height);
    if (!pix_r.ok()) {
      std::fprintf(stderr, "创建出图目标失败: %s\n", pix_r.message().c_str());
      return 1;
    }
    dev::IRenderTarget* pix_target = pix_r.value().get();
    auto* off = dynamic_cast<::magicblocks::backend::OffscreenTarget*>(pix_target);
    if (off == nullptr) {
      std::fprintf(stderr, "内部错误:离屏目标不是 backend::OffscreenTarget\n");
      return 1;
    }
    dev::Status ds = off->draw_selftest(
        args.frame_index & ~static_cast<dev::u64>(1u),
        ::magicblocks::backend::OffscreenTarget::SelftestKind::kL2Scene);
    if (!ds.ok()) {
      std::fprintf(stderr, "绘制失败(code=%d): %s\n", static_cast<int>(ds.code),
                   ds.message.c_str());
      return 1;
    }

    magicblocks::ImageRgba8 img;
    img.width = args.width;
    img.height = args.height;
    img.pixels.assign(img.byte_size(), 0);
    dev::Status rs = pix_target->readback_rgba8(img.pixels.data(), img.pixels.size());
    if (!rs.ok()) {
      std::fprintf(stderr, "读回失败(code=%d): %s\n", static_cast<int>(rs.code),
                   rs.message.c_str());
      return 1;
    }
    std::string png_err;
    if (!magicblocks::write_png_rgba8(args.output, img, &png_err)) {
      std::fprintf(stderr, "%s\n", png_err.c_str());
      return 1;
    }
  }

  // ---- 命令流落盘(二进制是权威载体)
  if (!args.record_path.empty()) {
    auto enc = sc::encode_binary(stream);
    if (!enc.ok()) {
      std::fprintf(stderr, "编码命令流失败: %s\n", enc.message().c_str());
      return 1;
    }
    std::string file_err;
    if (!write_file(args.record_path, enc.value(), &file_err)) {
      std::fprintf(stderr, "%s\n", file_err.c_str());
      return 1;
    }
  }

  std::printf("recorded %s (%ux%u, frame_index=%llu, nodes=%zu, passes=%zu, commands=%zu)\n",
              args.output.c_str(), args.width, args.height,
              static_cast<unsigned long long>(args.frame_index), stream.scene.nodes.size(),
              stream.passes.size(), stream.commands.size());
  std::printf("hash=0x%016llx\n", static_cast<unsigned long long>(stream.stream_hash));

  // 场景快照回读自检:重建后必须与录制时逐字段相等(S8 的 CLI 侧旁证)。
  {
    auto rebuilt = sc::create_scene();
    if (!rebuilt->restore(stream.scene)) {
      std::fprintf(stderr, "内部错误:流里的场景快照无法重建\n");
      return 1;
    }
    if (rebuilt->snapshot() != before) {
      std::fprintf(stderr, "内部错误:重建场景与录制时不一致\n");
      return 1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------- replay / reproduce / dump

bool load_stream(const std::string& path, sc::CommandStream* out, int* exit_code) {
  std::vector<std::uint8_t> bytes;
  std::string err;
  if (!read_file(path, &bytes, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    *exit_code = 1;
    return false;
  }
  auto dec = sc::decode_binary(bytes);
  if (!dec.ok()) {
    std::fprintf(stderr, "解码失败(code=%d): %s\n", static_cast<int>(dec.status().code),
                 dec.message().c_str());
    *exit_code = 1;
    return false;
  }
  *out = dec.take();
  return true;
}

int cmd_replay(const Args& args) {
  sc::CommandStream stream;
  int code = 0;
  if (!load_stream(args.replay_path, &stream, &code)) return code;

  auto dev_r = dev::create_render_device(sw_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n",
                 static_cast<int>(dev_r.status().code), dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  auto tgt_r = make_offscreen(*device, stream.extent.width, stream.extent.height);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }

  auto recorder = sc::create_recorder();
  sc::RecorderAttachments att;
  att.pipelines.push_back(demo_pipeline_desc());
  dev::Status as = recorder->on_attach(*device, att);
  if (!as.ok()) {
    std::fprintf(stderr, "on_attach 失败: %s\n", as.message.c_str());
    return 1;
  }

  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }

  dev::Status rs = recorder->replay(stream, *cmd_r.value(), tgt_r.value().get());
  if (!rs.ok()) {
    std::fprintf(stderr, "回放失败(code=%d): %s\n", static_cast<int>(rs.code),
                 rs.message.c_str());
    return 1;
  }

  std::printf("replayed %zu commands, hash=0x%016llx\n", stream.commands.size(),
              static_cast<unsigned long long>(stream.stream_hash));

  if (args.verify) {
    const std::uint64_t again = sc::compute_stream_hash(stream);
    if (again != stream.stream_hash) {
      std::fprintf(stderr, "verify 失败:重算哈希 0x%016llx != 记录 0x%016llx\n",
                   static_cast<unsigned long long>(again),
                   static_cast<unsigned long long>(stream.stream_hash));
      return 1;
    }
    std::printf("verify ok\n");
  }
  return 0;
}

int cmd_reproduce(const Args& args) {
  sc::CommandStream stream;
  int code = 0;
  if (!load_stream(args.replay_path, &stream, &code)) return code;

  auto recorder = sc::create_recorder();
  // 复现函数拿到的是**从流里重建的场景**(B7:不依赖外部 IScene),必须自己
  // 重新生成一遍与录制时完全相同的 events / passes / commands。
  //
  // 演示帧的内容是纯函数式的:事件的时间戳由 frame_index 与 k 推导,pass 参数
  // 是常量,命令序列固定。所以这里按**同一套规则**重算 —— 而不是把流里的数组
  // 原样拷回去(那样等于自查,证明不了任何东西)。
  sc::ReproduceFn fn = [&stream](sc::IScene& scene, res::ICommandList& cl,
                                 sc::CommandStream& out) -> dev::Status {
    (void)cl;
    // 注意:recorder 给的 fresh 骨架是 `stream` 的拷贝(只清空了 commands/passes),
    // 因此 events 与 scene 快照**已经在里面**了。这里只能补 passes/commands,
    // 重复 push events 会让哈希不符。

    // 场景由 recorder 从流里重建后传进来,这里只校验它不是空的
    if (scene.node_count() != stream.scene.nodes.size()) {
      return dev::Status::Invalid("复现:重建的场景节点数与流里不一致");
    }

    // pass 参数是常量(与 cmd_record 的 PassDesc 一致)
    sc::PassRecord pr;
    pr.name = "scene";
    pr.layer = static_cast<std::uint8_t>(sc::FeatureLayer::Scene);
    pr.channel = 0;
    pr.clear_color = true;
    pr.clear_color_rgba[0] = 0.05f;
    pr.clear_color_rgba[1] = 0.08f;
    pr.clear_color_rgba[2] = 0.20f;
    pr.clear_color_rgba[3] = 1.0f;
    pr.clear_depth = false;
    pr.clear_depth_value = 1.0f;
    pr.area[0] = 0;
    pr.area[1] = 0;
    pr.area[2] = 0;
    pr.area[3] = 0;
    pr.color_format = dev::Format::RGBA8_UNORM;
    pr.has_depth = false;
    out.passes.push_back(pr);

    // 命令序列:begin_pass → bind_pipeline → viewport → scissor → draw → end_pass
    sc::CommandRecord begin_pass;
    begin_pass.op = sc::CommandOp::BeginPass;
    // U2:原始 area(0 表示整图,不做 0→extent 解析)
    begin_pass.pass_area[0] = 0;
    begin_pass.pass_area[1] = 0;
    begin_pass.pass_area[2] = 0;
    begin_pass.pass_area[3] = 0;
    begin_pass.f[0] = 1.0f;  // clear_color = true
    begin_pass.f[1] = 0.05f;
    begin_pass.f[2] = 0.08f;
    begin_pass.f[3] = 0.20f;
    begin_pass.f[4] = 1.0f;
    begin_pass.f[5] = 0.0f;  // clear_depth = false
    begin_pass.f[6] = 1.0f;  // clear_depth_value
    out.commands.push_back(begin_pass);

    sc::CommandRecord bind;
    bind.op = sc::CommandOp::BindPipeline;
    // 管线哈希取自流里的那一条(record 时由 on_attach 建的管线决定)
    bind.pipeline_hash = stream.commands.size() > 1 ? stream.commands[1].pipeline_hash : 0u;
    out.commands.push_back(bind);

    sc::CommandRecord vp;
    vp.op = sc::CommandOp::SetViewport;
    vp.f[0] = 0.0f;
    vp.f[1] = 0.0f;
    vp.f[2] = static_cast<float>(stream.extent.width);
    vp.f[3] = static_cast<float>(stream.extent.height);
    vp.f[4] = 0.0f;
    vp.f[5] = 1.0f;
    out.commands.push_back(vp);

    sc::CommandRecord scissor;
    scissor.op = sc::CommandOp::SetScissor;
    scissor.a = 0;
    scissor.b = 0;
    scissor.width = stream.extent.width;
    scissor.height = stream.extent.height;
    out.commands.push_back(scissor);

    sc::CommandRecord draw;
    draw.op = sc::CommandOp::Draw;
    draw.a = 4;    // vertex_count
    draw.b = 16;   // instance_count
    draw.c = 0;    // first_vertex
    draw.i = 0;    // vertex_offset
    out.commands.push_back(draw);

    sc::CommandRecord end_pass;
    end_pass.op = sc::CommandOp::EndPass;
    out.commands.push_back(end_pass);
    return dev::Status::Ok_();
  };

  dev::Status rs = recorder->reproduce(stream, fn);
  if (!rs.ok()) {
    std::fprintf(stderr, "复现失败(code=%d): %s\n", static_cast<int>(rs.code),
                 rs.message.c_str());
    return 1;
  }
  std::printf("reproduce ok, hash=0x%016llx\n",
              static_cast<unsigned long long>(stream.stream_hash));
  if (args.verify) std::printf("verify ok\n");
  return 0;
}

int cmd_dump(const Args& args) {
  sc::CommandStream stream;
  int code = 0;
  if (!load_stream(args.replay_path, &stream, &code)) return code;

  if (args.json) {
    std::printf("%s", sc::to_json(stream).c_str());
    return 0;
  }
  std::printf("backend=%s engine_major=%u frame_index=%llu extent=%ux%u\n",
              stream.backend.c_str(), static_cast<unsigned>(stream.engine_major),
              static_cast<unsigned long long>(stream.frame_index), stream.extent.width,
              stream.extent.height);
  std::printf("hash=0x%016llx\n", static_cast<unsigned long long>(stream.stream_hash));
  std::printf("nodes=%zu events=%zu passes=%zu commands=%zu\n", stream.scene.nodes.size(),
              stream.events.size(), stream.passes.size(), stream.commands.size());
  for (std::size_t k = 0; k < stream.commands.size(); ++k) {
    std::printf("  [%zu] %s a=%u b=%u c=%u\n", k,
                sc::command_op_name(stream.commands[k].op), stream.commands[k].a,
                stream.commands[k].b, stream.commands[k].c);
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
  if (parse_args(argc - 1, argv + 1, &args) != 0) {
    print_usage();
    return 2;
  }

  // D16:`--scene` 只接受 demo;其它值(含 spec §5.4 的四个玩法名)是 L4 的东西。
  if (args.cmd == "record") {
    if (args.scene != "demo") {
      std::fprintf(stderr,
                   "--scene %s 不支持:玩法预设属 L4(spec §13 第 5 阶段),"
                   "L2 仅支持 --scene demo\n",
                   args.scene.c_str());
      return 2;
    }
  } else if (args.scene_given) {
    std::fprintf(stderr, "--scene 只对 record 子命令有效\n");
    return 2;
  }

  if (args.cmd == "record") return cmd_record(args);
  if (args.cmd == "replay" || args.cmd == "reproduce" || args.cmd == "dump") {
    if (args.replay_path.empty()) {
      std::fprintf(stderr, "--replay 必填\n");
      return 2;
    }
    if (args.cmd == "replay") return cmd_replay(args);
    if (args.cmd == "reproduce") return cmd_reproduce(args);
    return cmd_dump(args);
  }
  print_usage();
  return 2;
}
