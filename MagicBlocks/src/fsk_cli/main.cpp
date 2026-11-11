// MagicBlocks L4 —— 魔法涂鸦：F-SK 离屏出图（render / replay / reproduce）
//
// ============================ 为什么单独一个 CLI ============================
// 设计 §12 把 F-SK 的出图通路定为 `magicblocks-fsk-cli render/replay/reproduce/dump`。
// 之前 F-SK 是塞在 `magicblocks-l3-cli sketch` 里的，后果很具体：
//   - 没有 `--camera ortho|perspective` ⇒ A3 无法验收；
//   - 没有 record/replay/reproduce ⇒ A7/A8 无法验收；
//   - 它和 L3 的 paint/blocks/gizmo 共用一张参数表 ⇒ `sketch --template circle`
//     这类跨子命令参数会被**静默接受**（S-8）。
// 独立可执行文件 + 独立参数表，三条一起解决：每个子命令只认自己的参数，
// 未知参数一律 exit 2（不静默忽略）。
//
// ============================ 录制的是什么 ============================
// 录制的是**事件流**，不是 GPU 命令。理由（设计 §9.3 / §11.5）：
//   文档 → 网格 → 缓冲 → 命令 这条链是纯函数式的，只要"事件流 + 参数"一致，
//   重新推导出来的命令就一定一致；而 GPU 命令里的 ResourceId 含页内偏移，
//   跨进程不可复现。录事件流，回放时整条链重跑，比录命令更严格：
//   它连"几何生成是否确定"都一起验了。
//
// 回放与复现的区别（与 L2 同口径）：
//   replay    —— 从流里读事件，交给**同一套** features 管线（只验"流→帧"这一段）；
//   reproduce —— 用流里的参数**重新生成**同一串事件，再跑一遍（验"输入→流→帧"）。
// 两者的 PNG 都必须与录制帧逐像素一致（A8）。
//
// 退出码：0 成功 · 1 运行失败 · 2 参数错误 · 3 需要提权让 loader 看到 SwiftShader。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "backend/vulkan/vulkan_target.h"
#include "core/image_io.h"
#include "magicblocks/device/events.h"
#include "magicblocks/device/render_device.h"
#include "magicblocks/features/sketch/sketch_canvas.h"
#include "magicblocks/features/sketch/sketch_document.h"
#include "magicblocks/features/sketch/sketch_pipeline.h"
#include "magicblocks/primitives/batch.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/mesh_factory.h"
#include "magicblocks/primitives/painter.h"
#include "magicblocks/primitives/primitive_renderer.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace mbp = magicblocks::primitives;
namespace fsk = magicblocks::features::sketch;

namespace {

// ---------------------------------------------------------------- 参数

// 采集文件格式版本。**升级必须同时升版本号**：旧文件被当成新格式误读，
// 表现是"回放出来的图和录制的不一样"，而不是一个明确的错误。
constexpr char kCaptureMagic[4] = {'M', 'B', 'F', 'S'};
constexpr std::uint32_t kCaptureVersion = 1;

struct Args {
  std::string cmd;

  // render
  int width = 480;
  int height = 360;
  int strokes = 3;
  std::string camera = "perspective";  // ortho | perspective
  float thickness = 0.12f;
  float canvas_width = 2.0f;
  float canvas_height = 1.5f;
  float stroke_width = 0.10f;
  std::uint32_t frame_index = 0;
  bool region = false;  // 用 Region（"路径包围的布尔区域"）解读演示笔画
  bool merge = false;   // 多笔合并成一个实体
  std::string output;
  std::string record;  // 可选：落盘事件流

  // replay / reproduce / dump
  std::string replay;
  bool verify = false;
  bool json = false;
};

void print_usage() {
  std::fprintf(
      stderr,
      "用法:\n"
      "  magicblocks-fsk-cli render    --width W --height H --strokes N\n"
      "                                [--camera ortho|perspective] [--thickness F]\n"
      "                                [--canvas-width F] [--canvas-height F] [--stroke-width F]\n"
      "                                [--region] [--merge] [--frame-index N]\n"
      "                                [--record cap.fsk] --output out.png\n"
      "  magicblocks-fsk-cli replay    --replay cap.fsk [--verify] --output out.png\n"
      "  magicblocks-fsk-cli reproduce --replay cap.fsk [--verify] --output out.png\n"
      "  magicblocks-fsk-cli dump      --replay cap.fsk [--json]\n"
      "\n"
      "退出码: 0 成功 · 1 运行失败 · 2 参数错误 · 3 需要提权让 loader 看到 SwiftShader\n");
}

bool take_value(int argc, char** argv, int* i, const char* flag, std::string* out) {
  if (*i + 1 >= argc) {
    std::fprintf(stderr, "%s 缺少参数值\n", flag);
    return false;
  }
  *out = argv[++*i];
  return true;
}

// 各子命令的合法参数集合。**未知参数 → 2**，跨子命令的参数同样算未知。
bool flag_allowed(const std::string& cmd, const std::string& flag) {
  static const char* kRender[] = {"--width", "--height", "--strokes", "--camera",
                                  "--thickness", "--canvas-width", "--canvas-height",
                                  "--stroke-width", "--region", "--merge", "--frame-index",
                                  "--record", "--output"};
  static const char* kReplayLike[] = {"--replay", "--output", "--verify"};
  static const char* kDump[] = {"--replay", "--json"};

  const char** set = nullptr;
  std::size_t n = 0;
  if (cmd == "render") {
    set = kRender;
    n = sizeof(kRender) / sizeof(kRender[0]);
  } else if (cmd == "replay" || cmd == "reproduce") {
    set = kReplayLike;
    n = sizeof(kReplayLike) / sizeof(kReplayLike[0]);
  } else if (cmd == "dump") {
    set = kDump;
    n = sizeof(kDump) / sizeof(kDump[0]);
  } else {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (flag == set[i]) return true;
  }
  return false;
}

int parse_args(int argc, char** argv, Args* out) {
  if (argc < 2) return -1;
  out->cmd = argv[1];
  if (out->cmd != "render" && out->cmd != "replay" && out->cmd != "reproduce" &&
      out->cmd != "dump") {
    std::fprintf(stderr, "未知子命令: %s\n", out->cmd.c_str());
    return -2;
  }
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") return -1;
    if (!flag_allowed(out->cmd, a)) {
      std::fprintf(stderr, "%s 不认识参数: %s\n", out->cmd.c_str(), a.c_str());
      return -2;
    }
    std::string v;
    if (a == "--width") {
      if (!take_value(argc, argv, &i, "--width", &v)) return -2;
      out->width = std::atoi(v.c_str());
    } else if (a == "--height") {
      if (!take_value(argc, argv, &i, "--height", &v)) return -2;
      out->height = std::atoi(v.c_str());
    } else if (a == "--strokes") {
      if (!take_value(argc, argv, &i, "--strokes", &v)) return -2;
      out->strokes = std::atoi(v.c_str());
    } else if (a == "--camera") {
      if (!take_value(argc, argv, &i, "--camera", &v)) return -2;
      if (v != "ortho" && v != "perspective") {
        std::fprintf(stderr, "--camera 取值必须显式(ortho|perspective), 实际: %s\n", v.c_str());
        return -2;
      }
      out->camera = v;
    } else if (a == "--thickness") {
      if (!take_value(argc, argv, &i, "--thickness", &v)) return -2;
      out->thickness = static_cast<float>(std::atof(v.c_str()));
    } else if (a == "--canvas-width") {
      if (!take_value(argc, argv, &i, "--canvas-width", &v)) return -2;
      out->canvas_width = static_cast<float>(std::atof(v.c_str()));
    } else if (a == "--canvas-height") {
      if (!take_value(argc, argv, &i, "--canvas-height", &v)) return -2;
      out->canvas_height = static_cast<float>(std::atof(v.c_str()));
    } else if (a == "--stroke-width") {
      if (!take_value(argc, argv, &i, "--stroke-width", &v)) return -2;
      out->stroke_width = static_cast<float>(std::atof(v.c_str()));
    } else if (a == "--frame-index") {
      if (!take_value(argc, argv, &i, "--frame-index", &v)) return -2;
      out->frame_index = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (a == "--region") {
      out->region = true;
    } else if (a == "--merge") {
      out->merge = true;
    } else if (a == "--output") {
      if (!take_value(argc, argv, &i, "--output", &v)) return -2;
      out->output = v;
    } else if (a == "--record") {
      if (!take_value(argc, argv, &i, "--record", &v)) return -2;
      out->record = v;
    } else if (a == "--replay") {
      if (!take_value(argc, argv, &i, "--replay", &v)) return -2;
      out->replay = v;
    } else if (a == "--verify") {
      out->verify = true;
    } else if (a == "--json") {
      out->json = true;
    }
  }
  return 0;
}

// ---------------------------------------------------------------- 事件流（演示输入）

// 一帧的全部输入：参数 + **完整的事件流**。录制就是把它原样落盘。
//
// 为什么事件流本身要进采集文件：F-SK 的真源是"用户画了哪些笔画"，
// 而笔画来自事件。只存参数（--strokes N）等于把生成器写死成唯一合法的输入源，
// 将来换成真实鼠标/触摸输入时，采集文件就没法表达"这一帧用户具体怎么画的"。
// 现在就把事件流存进去，回放的是**真实的事件序列**，不是"再跑一次生成器"。
struct Capture {
  std::uint32_t width = 480;
  std::uint32_t height = 360;
  std::uint32_t frame_index = 0;
  int camera_ortho = 0;  // 1 = 正交
  float ortho_height = 2.0f;
  float thickness = 0.12f;
  fsk::CanvasParams canvas{};
  float stroke_width = 0.10f;
  std::uint32_t region = 0;
  std::uint32_t merge = 0;
  std::vector<dev::InputEvent> events;
  // 输入参数的哈希（= 事件流 + 常量参数的指纹）。三态校验用它。
  std::uint64_t input_hash = 0;
};

// 确定性事件序列：屏幕坐标由 (strokes, frame_index, region) 完全推导。
//
// **不读时钟、不读随机数、不读文件系统状态** —— 同参数必然产出同一串事件。
// 事件是 `device::InputEvent`（真实输入类型），不是"已经算好的画布坐标"：
// 这一层之差就是 R-SK-1 要补的那一跳，演示输入必须从**屏幕**开始。
std::vector<dev::InputEvent> make_demo_events(int strokes, int width, int height,
                                              std::uint32_t frame_index, bool region) {
  std::vector<dev::InputEvent> out;
  if (strokes <= 0) return out;

  std::int64_t ts = 0;
  auto push = [&out, &ts](dev::InputEventKind kind, float x, float y) {
    dev::InputEvent e;
    e.kind = kind;
    e.pointer_id = 1;
    e.position = dev::Vec2f{x, y};
    e.button = 0;
    e.timestamp_us = ts++;
    out.push_back(e);
  };

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);

  for (int i = 0; i < strokes; ++i) {
    const float t = (strokes <= 1) ? 0.5f
                                   : static_cast<float>(i) / static_cast<float>(strokes - 1);
    if (region) {
      // 一块闭合方框：四条边各若干采样点，首尾回到起点（Region 要求闭合）。
      const float cx = 0.5f * w + (t - 0.5f) * 0.34f * w;
      const float cy = 0.5f * h + (t - 0.5f) * 0.18f * h;
      const float hx = 0.10f * w;
      const float hy = 0.13f * h;
      constexpr int kSeg = 6;
      std::vector<dev::Vec2f> pts;
      // 上边 → 右边 → 下边 → 左边 → 回起点
      const dev::Vec2f corners[4] = {{cx - hx, cy - hy},
                                     {cx + hx, cy - hy},
                                     {cx + hx, cy + hy},
                                     {cx - hx, cy + hy}};
      for (int k = 0; k < 4; ++k) {
        const dev::Vec2f a = corners[k];
        const dev::Vec2f b = corners[(k + 1) & 3];
        for (int s = 0; s < kSeg; ++s) {
          const float u = static_cast<float>(s) / static_cast<float>(kSeg);
          pts.push_back({a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u});
        }
      }
      pts.push_back(corners[0]);
      if (!pts.empty()) {
        push(dev::InputEventKind::PointerDown, pts[0].x, pts[0].y);
        for (std::size_t k = 1; k < pts.size(); ++k) {
          push(dev::InputEventKind::PointerMove, pts[k].x, pts[k].y);
        }
        push(dev::InputEventKind::PointerUp, pts.back().x, pts.back().y);
      }
    } else {
      // 一条右下的波浪线：32 段。
      constexpr int kSteps = 32;
      const float base_y = 0.5f * h + (t - 0.5f) * 0.30f * h;
      float first_x = 0.0f, first_y = 0.0f, last_x = 0.0f, last_y = 0.0f;
      for (int k = 0; k <= kSteps; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(kSteps);
        const float x = 0.10f * w + 0.80f * w * u;
        const float y = base_y + 0.06f * h * std::sin(u * 9.42478f + t * 3.14159f);
        if (k == 0) {
          first_x = x;
          first_y = y;
          push(dev::InputEventKind::PointerDown, x, y);
        } else {
          push(dev::InputEventKind::PointerMove, x, y);
        }
        last_x = x;
        last_y = y;
      }
      push(dev::InputEventKind::PointerUp, last_x, last_y);
      (void)first_x;
      (void)first_y;
    }
  }
  (void)frame_index;  // 事件形状不依赖帧号；帧号只进采集头与文字输出
  return out;
}

// ---------------------------------------------------------------- 采集文件（二进制）

void put_u32(std::vector<std::uint8_t>* b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}
void put_u64(std::vector<std::uint8_t>* b, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) b->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}
void put_f32(std::vector<std::uint8_t>* b, float f) {
  std::uint32_t v = 0;
  std::memcpy(&v, &f, sizeof(v));
  put_u32(b, v);
}

std::uint64_t fnv1a(const std::uint8_t* data, std::size_t n, std::uint64_t seed) {
  std::uint64_t h = seed;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(data[i]);
    h *= 0x100000001b3ull;
  }
  return h;
}

std::uint64_t compute_input_hash(const Capture& c) {
  std::vector<std::uint8_t> b;
  put_u32(&b, c.width);
  put_u32(&b, c.height);
  put_u32(&b, c.frame_index);
  put_u32(&b, c.camera_ortho ? 1u : 0u);
  put_f32(&b, c.ortho_height);
  put_f32(&b, c.thickness);
  put_f32(&b, c.canvas.width);
  put_f32(&b, c.canvas.height);
  put_f32(&b, c.canvas.extrude_thickness);
  put_u32(&b, c.canvas.clamp_to_canvas ? 1u : 0u);
  put_f32(&b, c.canvas.min_spacing);
  put_u32(&b, c.canvas.max_points_per_stroke);
  for (int k = 0; k < 16; ++k) put_f32(&b, c.canvas.transform[k]);
  put_f32(&b, c.stroke_width);
  put_u32(&b, c.region);
  put_u32(&b, c.merge);
  put_u32(&b, static_cast<std::uint32_t>(c.events.size()));
  for (const dev::InputEvent& e : c.events) {
    put_u32(&b, static_cast<std::uint32_t>(e.kind));
    put_u32(&b, e.pointer_id);
    put_f32(&b, e.position.x);
    put_f32(&b, e.position.y);
    put_f32(&b, e.delta.x);
    put_f32(&b, e.delta.y);
    put_u32(&b, static_cast<std::uint32_t>(e.button));
    put_u64(&b, static_cast<std::uint64_t>(e.timestamp_us));
  }
  return fnv1a(b.data(), b.size(), 0xcbf29ce484222325ull);
}

std::vector<std::uint8_t> encode_capture(Capture c) {
  c.input_hash = compute_input_hash(c);
  std::vector<std::uint8_t> b;
  b.insert(b.end(), kCaptureMagic, kCaptureMagic + 4);
  put_u32(&b, kCaptureVersion);
  put_u32(&b, c.width);
  put_u32(&b, c.height);
  put_u32(&b, c.frame_index);
  put_u32(&b, c.camera_ortho ? 1u : 0u);
  put_f32(&b, c.ortho_height);
  put_f32(&b, c.thickness);
  put_f32(&b, c.canvas.width);
  put_f32(&b, c.canvas.height);
  put_f32(&b, c.canvas.extrude_thickness);
  put_u32(&b, c.canvas.clamp_to_canvas ? 1u : 0u);
  put_f32(&b, c.canvas.min_spacing);
  put_u32(&b, c.canvas.max_points_per_stroke);
  for (int k = 0; k < 16; ++k) put_f32(&b, c.canvas.transform[k]);
  put_f32(&b, c.stroke_width);
  put_u32(&b, c.region);
  put_u32(&b, c.merge);
  put_u32(&b, static_cast<std::uint32_t>(c.events.size()));
  for (const dev::InputEvent& e : c.events) {
    put_u32(&b, static_cast<std::uint32_t>(e.kind));
    put_u32(&b, e.pointer_id);
    put_f32(&b, e.position.x);
    put_f32(&b, e.position.y);
    put_f32(&b, e.delta.x);
    put_f32(&b, e.delta.y);
    put_u32(&b, static_cast<std::uint32_t>(e.button));
    put_u64(&b, static_cast<std::uint64_t>(e.timestamp_us));
  }
  put_u64(&b, c.input_hash);
  return b;
}

struct Reader {
  const std::vector<std::uint8_t>& b;
  std::size_t p = 0;
  bool ok = true;
  bool need(std::size_t n) {
    if (!ok || p + n > b.size()) {
      ok = false;
      return false;
    }
    return true;
  }
  std::uint32_t u32() {
    if (!need(4)) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[p + i]) << (8 * i);
    p += 4;
    return v;
  }
  std::uint64_t u64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[p + i]) << (8 * i);
    p += 8;
    return v;
  }
  float f32() {
    const std::uint32_t v = u32();
    float f = 0.0f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
  }
};

bool decode_capture(const std::vector<std::uint8_t>& bytes, Capture* out, std::string* err) {
  if (bytes.size() < 8 || std::memcmp(bytes.data(), kCaptureMagic, 4) != 0) {
    *err = "不是 F-SK 采集文件(魔数不符)";
    return false;
  }
  Reader r{bytes};
  r.p = 4;
  const std::uint32_t ver = r.u32();
  if (ver != kCaptureVersion) {
    *err = "采集文件版本不支持: " + std::to_string(ver) + "(本程序支持 " +
           std::to_string(kCaptureVersion) + ")";
    return false;
  }
  Capture c;
  c.width = r.u32();
  c.height = r.u32();
  c.frame_index = r.u32();
  c.camera_ortho = static_cast<int>(r.u32());
  c.ortho_height = r.f32();
  c.thickness = r.f32();
  c.canvas.width = r.f32();
  c.canvas.height = r.f32();
  c.canvas.extrude_thickness = r.f32();
  c.canvas.clamp_to_canvas = r.u32() != 0;
  c.canvas.min_spacing = r.f32();
  c.canvas.max_points_per_stroke = r.u32();
  for (int k = 0; k < 16; ++k) c.canvas.transform[k] = r.f32();
  c.stroke_width = r.f32();
  c.region = r.u32();
  c.merge = r.u32();
  const std::uint32_t n = r.u32();
  if (!r.ok || n > 1u << 20) {  // 防御：损坏文件里的巨大计数不得让它 OOM
    *err = "采集文件截断或事件数非法";
    return false;
  }
  c.events.reserve(n);
  for (std::uint32_t i = 0; i < n && r.ok; ++i) {
    dev::InputEvent e;
    e.kind = static_cast<dev::InputEventKind>(r.u32());
    e.pointer_id = r.u32();
    e.position.x = r.f32();
    e.position.y = r.f32();
    e.delta.x = r.f32();
    e.delta.y = r.f32();
    e.button = static_cast<std::int32_t>(r.u32());
    e.timestamp_us = static_cast<std::int64_t>(r.u64());
    c.events.push_back(e);
  }
  c.input_hash = r.u64();
  if (!r.ok || r.p != bytes.size()) {
    *err = "采集文件截断或尾部有多余字节";
    return false;
  }
  // 哈希自检：文件被截断/篡改却没变长度时，这一条抓得到。
  const std::uint64_t recomputed = compute_input_hash(c);
  if (recomputed != c.input_hash) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "采集文件内容与自带哈希不符(重算 %016llx != 记录 %016llx)",
                  static_cast<unsigned long long>(recomputed),
                  static_cast<unsigned long long>(c.input_hash));
    *err = buf;
    return false;
  }
  *out = std::move(c);
  return true;
}

bool read_file(const std::string& path, std::vector<std::uint8_t>* out, std::string* err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    *err = "无法打开文件: " + path;
    return false;
  }
  out->clear();
  std::uint8_t buf[8192];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
  std::fclose(f);
  return true;
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string* err) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    *err = "无法写入文件: " + path;
    return false;
  }
  const std::size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
  const int rc = std::fclose(f);
  if (n != bytes.size() || rc != 0) {
    *err = "写入不完整: " + path;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 设备 / 相机

dev::DeviceDesc software_desc() {
  dev::DeviceDesc d;
  d.backend = dev::BackendType::Vulkan;
  d.enable_validation = false;
  // 要求软件光栅器：golden 必须跨机器可复现（与 L0/L2/L3 CLI 同口径）。
  d.require_software_rasterizer = true;
  return d;
}

std::unique_ptr<mbp::ICamera> make_camera(const Capture& c) {
  auto cam = mbp::create_camera();
  if (!cam) return nullptr;
  mbp::CameraDesc cd;
  cd.kind = c.camera_ortho ? mbp::ProjectionKind::Orthographic : mbp::ProjectionKind::Perspective;
  cd.fov_y_degrees = 45.0f;
  // 正交视高要能{同时}框住整张画布：画布 height + 余量。
  cd.ortho_height = c.ortho_height;
  cd.near_plane = 0.05f;
  cd.far_plane = 100.0f;
  cd.viewport.width = c.width;
  cd.viewport.height = c.height;
  if (!cam->set_desc(cd).ok()) return nullptr;
  mbp::CameraPose pose;
  // 斜俯视：能同时看到顶盖与侧壁（纯正面看不出"挤出"这回事）。
  pose.position = {1.15f, 0.95f, 2.6f};
  pose.target = {0.0f, 0.0f, 0.0f};
  pose.up = {0.0f, 1.0f, 0.0f};
  if (!cam->set_pose(pose).ok()) return nullptr;
  return cam;
}

// 与 L3 CLI 的 scene_pipeline_desc 逐字段一致的实例化 3D 管线
// （顶点/实例布局、深度、剔除、预乘 alpha）。复制而不是复用：
// l3_cli 是可执行程序的内部函数，跨可执行文件共享会把两个 CLI 的构建绑在一起。
res::PipelineDesc scene_pipeline_desc(std::uint32_t samples) {
  res::PipelineDesc d;
  {
    res::VertexInputBinding vb;
    vb.binding = 0;
    vb.stride = sizeof(mbp::Vertex);
    vb.instanced = false;
    d.vertexLayout.bindings.push_back(vb);
  }
  {
    res::VertexInputBinding vb;
    vb.binding = 1;
    vb.stride = sizeof(mbp::InstanceAttribs);
    vb.instanced = true;
    d.vertexLayout.bindings.push_back(vb);
  }
  auto attr = [&d](std::uint32_t loc, std::uint32_t b, res::VertexFormat f, std::uint32_t off) {
    res::VertexAttribute a;
    a.location = loc;
    a.binding = b;
    a.format = f;
    a.offset = off;
    d.vertexLayout.attributes.push_back(a);
  };
  attr(0, 0, res::VertexFormat::Float3, 0);
  attr(1, 0, res::VertexFormat::Float3, 16);
  attr(2, 0, res::VertexFormat::UByte4Norm, 32);
  attr(3, 0, res::VertexFormat::Float2, 36);
  attr(4, 1, res::VertexFormat::Float4, 0);
  attr(5, 1, res::VertexFormat::Float4, 16);
  attr(6, 1, res::VertexFormat::Float4, 32);
  attr(7, 1, res::VertexFormat::Float4, 48);
  attr(8, 1, res::VertexFormat::Float4, 64);
  attr(9, 1, res::VertexFormat::Float4, 80);
  attr(10, 1, res::VertexFormat::Float4, 96);
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Vertex;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL3_sceneVert);
    m.size_bytes = magicblocks::shaders::kL3_sceneVertSize;
    d.stages.push_back(m);
  }
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Fragment;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL3_sceneFrag);
    m.size_bytes = magicblocks::shaders::kL3_sceneFragSize;
    d.stages.push_back(m);
  }
  d.blend.enabled = true;
  d.blend.src_color = res::BlendFactor::One;
  d.blend.dst_color = res::BlendFactor::OneMinusSrcAlpha;
  d.blend.src_alpha = res::BlendFactor::One;
  d.blend.dst_alpha = res::BlendFactor::OneMinusSrcAlpha;
  d.depth.depthTest = true;
  d.depth.depthWrite = true;
  d.raster.cull = res::CullMode::Back;
  d.samples = samples == 0 ? 1u : samples;
  d.primitive = res::PrimitiveType::TriangleList;
  d.push_constant_bytes = 128;
  return d;
}

// ---------------------------------------------------------------- 纯 CPU 阶段

// 事件流 → 文档 → 网格。**全程不碰设备**，所以 dry-run / 统计也能用。
struct CpuResult {
  bool ok = false;
  std::string error;
  fsk::SketchDocument doc;
  fsk::SketchMeshSet set;
  std::uint32_t committed = 0;
  std::uint32_t painter_segments = 0;
  fsk::Canvas::SampleStats sample_stats{};
  std::uint64_t doc_hash = 0;
};

CpuResult build_document(const Capture& c) {
  CpuResult r;
  auto cam = make_camera(c);
  if (!cam) {
    r.error = "创建相机失败(参数非法?)";
    return r;
  }
  fsk::Canvas canvas(c.canvas);
  if (!canvas.set_params(c.canvas).ok()) {
    r.error = "画布参数非法";
    return r;
  }

  // ① device::InputEvent → primitives::StrokeEvent（D6 映射，不做坐标变换）
  std::vector<mbp::StrokeEvent> events;
  const fsk::InputMapStats mst =
      fsk::map_input_stream(c.events.data(), c.events.size(), &events);
  if (mst.mapped + mst.ignored != c.events.size()) {
    r.error = "事件映射计数不自洽";
    return r;
  }

  // ② 事件 → IPainter（增量几何）+ Canvas（画布采样）
  fsk::Stroke tpl;
  tpl.width = c.stroke_width;
  tpl.style = c.region ? mbp::PathStyle::Region : mbp::PathStyle::Ribbon;
  tpl.closed = c.region;
  tpl.color = {0.85f, 0.80f, 0.72f, 1.0f};
  auto painter = mbp::create_painter();
  fsk::PainterFeedResult fed =
      fsk::feed_painter_and_canvas(*painter, canvas, *cam, events.data(), events.size(), tpl);
  r.painter_segments = fed.painter_segments_appended;
  r.sample_stats = canvas.stats();

  // ③ 提交的笔画 → 文档。零笔 = 明确失败，不写一张纯背景图（那会被 golden 锁死）。
  for (const fsk::Stroke& s : fed.committed) {
    if (!r.doc.add_stroke(s).valid()) {
      r.error = "添加笔画失败(参数非法)";
      return r;
    }
    ++r.committed;
  }
  if (r.doc.stroke_count() == 0) {
    r.error = "事件流没有产生任何笔画(拒绝出图)";
    return r;
  }

  // ④ 文档 → 轮廓 → 挤出
  fsk::BuildOptions opts;
  opts.thickness = c.thickness;
  opts.merge_strokes = c.merge != 0;
  r.set = fsk::build_meshes(r.doc, opts);
  if (!r.set.ok()) {
    std::string msg;
    for (const auto& e : r.set.errors) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "[code=%d] stroke#%llu %s; ", static_cast<int>(e.code),
                    static_cast<unsigned long long>(e.stroke.value), e.detail.c_str());
      msg += buf;
    }
    r.error = "管线报错: " + msg;
    return r;
  }
  if (r.set.meshes.empty()) {
    r.error = "管线没有产出任何实体(拒绝出图)";
    return r;
  }
  r.doc_hash = r.doc.hash();
  r.ok = true;
  return r;
}

// 统计行：录制/回放两条路径都打同一行，便于调用方逐字段比对。
void print_stats(const Capture& c, const CpuResult& r) {
  std::printf(
      "[fsk] 相机=%s 笔画 %zu | 实体 %zu | 顶点 %zu 索引 %zu | doc_hash=%016llx "
      "mesh_hash=%016llx | 事件 %zu(绘制 %u) 采样 %u 夹取 %u | input_hash=%016llx\n",
      c.camera_ortho ? "ortho" : "perspective", r.doc.stroke_count(), r.set.meshes.size(),
      r.set.vertex_count(), r.set.triangle_count() * 3,
      static_cast<unsigned long long>(r.doc_hash),
      static_cast<unsigned long long>(r.set.mesh_hash), c.events.size(), r.committed,
      r.sample_stats.samples_accepted, r.sample_stats.samples_clamped,
      static_cast<unsigned long long>(c.input_hash));
}

// ---------------------------------------------------------------- 出图（设备阶段）

// 一个实体在 GPU 上的顶点/索引资源。
struct GpuMesh {
  std::unique_ptr<res::IBuffer> vb;
  std::unique_ptr<res::IBuffer> ib;
  std::uint32_t index_count = 0;
  mbp::Color4 color;
};

int render_to_png(const Capture& c, const CpuResult& r, const std::string& output) {
  auto dev_r = dev::create_render_device(software_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "[fsk] 创建设备失败(code=%d): %s\n",
                 static_cast<int>(dev_r.status().code), dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      if (auto s = alloc->reserve_pages(kind, 4); !s.ok()) {
        std::fprintf(stderr, "[fsk] 预取内存页失败: %s\n", s.message.c_str());
        return 1;
      }
    }
  }

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {c.width, c.height};
  td.with_depth = true;  // 挤出体是实体，必须走深度测试否则穿帮
  auto tgt_r = device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "[fsk] 创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt_r.value().get();

  auto camera = make_camera(c);
  if (!camera) {
    std::fprintf(stderr, "[fsk] 创建相机失败\n");
    return 1;
  }

  std::vector<GpuMesh> gpu;
  gpu.reserve(r.set.meshes.size());
  for (const fsk::StrokeMesh& sm : r.set.meshes) {
    const mbp::MeshData& md = sm.mesh;
    res::BufferDesc vbd;
    vbd.size = md.vertices.size() * sizeof(mbp::Vertex);
    vbd.usage = res::BufferUsage::Vertex;
    auto vb_r = device->create_buffer(vbd);
    if (!vb_r.ok()) {
      std::fprintf(stderr, "[fsk] 创建顶点缓冲失败: %s\n", vb_r.message().c_str());
      return 1;
    }
    std::unique_ptr<res::IBuffer> vb = std::move(vb_r.value());
    if (!vb->upload(md.vertices.data(), vbd.size).ok()) {
      std::fprintf(stderr, "[fsk] 写顶点缓冲失败\n");
      return 1;
    }
    res::BufferDesc ibd;
    ibd.size = md.indices.size() * sizeof(std::uint32_t);
    ibd.usage = res::BufferUsage::Index;
    auto ib_r = device->create_buffer(ibd);
    if (!ib_r.ok()) {
      std::fprintf(stderr, "[fsk] 创建索引缓冲失败: %s\n", ib_r.message().c_str());
      return 1;
    }
    std::unique_ptr<res::IBuffer> ib = std::move(ib_r.value());
    if (!ib->upload(md.indices.data(), ibd.size).ok()) {
      std::fprintf(stderr, "[fsk] 写索引缓冲失败\n");
      return 1;
    }
    // upload() 只入队 Staging，必须紧跟 flush_pending()，否则 GPU 读到全 0
    // （表现为"画面空白但清屏色正常"，L3 踩过）。
    vb->flush_pending();
    ib->flush_pending();
    if (vb->pending_upload_bytes() != 0 || ib->pending_upload_bytes() != 0) {
      std::fprintf(stderr, "[fsk] 上传批未提交:漏了 flush_pending()\n");
      return 1;
    }
    GpuMesh g;
    g.vb = std::move(vb);
    g.ib = std::move(ib);
    g.index_count = static_cast<std::uint32_t>(md.indices.size());
    g.color = sm.color;
    gpu.push_back(std::move(g));
  }

  std::vector<mbp::InstanceAttribs> instances(gpu.size());
  for (std::size_t i = 0; i < gpu.size(); ++i) {
    instances[i] = mbp::InstanceAttribs{};
    instances[i].model[0] = 1.0f;
    instances[i].model[5] = 1.0f;
    instances[i].model[10] = 1.0f;
    instances[i].model[15] = 1.0f;
    instances[i].base_color[0] = gpu[i].color.r;
    instances[i].base_color[1] = gpu[i].color.g;
    instances[i].base_color[2] = gpu[i].color.b;
    instances[i].base_color[3] = gpu[i].color.a;
    instances[i].roughness = 0.55f;
    instances[i].metallic = 0.0f;
  }

  res::BufferDesc instd;
  instd.size = instances.size() * sizeof(mbp::InstanceAttribs);
  instd.usage = res::BufferUsage::Vertex;
  auto inst_r = device->create_buffer(instd);
  if (!inst_r.ok()) {
    std::fprintf(stderr, "[fsk] 创建实例缓冲失败: %s\n", inst_r.message().c_str());
    return 1;
  }
  res::IBuffer* inst_buf = inst_r.value().get();
  if (!inst_buf->upload(instances.data(), instd.size).ok()) {
    std::fprintf(stderr, "[fsk] 上传实例失败\n");
    return 1;
  }
  inst_buf->flush_pending();

  auto pl_r = device->create_pipeline(scene_pipeline_desc(1));
  if (!pl_r.ok()) {
    std::fprintf(stderr, "[fsk] 创建涂鸦管线失败: %s\n", pl_r.message().c_str());
    return 1;
  }
  res::IPipeline* pipeline = pl_r.value().get();

  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "[fsk] 创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }
  res::ICommandList* cmd = cmd_r.value().get();

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = 0.06f;
  rp.clear_color_rgba[1] = 0.07f;
  rp.clear_color_rgba[2] = 0.10f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = true;
  rp.clear_depth_value = 1.0f;
  if (auto s = cmd->begin_pass(rp); !s.ok()) {
    std::fprintf(stderr, "[fsk] begin_pass 失败: %s\n", s.message.c_str());
    return 1;
  }
  res::Viewport vpv;
  vpv.width = static_cast<float>(c.width);
  vpv.height = static_cast<float>(c.height);
  vpv.min_depth = 0.0f;
  vpv.max_depth = 1.0f;
  if (auto s = cmd->set_viewport(vpv); !s.ok()) {
    std::fprintf(stderr, "[fsk] set_viewport 失败: %s\n", s.message.c_str());
    return 1;
  }
  res::ScissorRect sr;
  sr.width = c.width;
  sr.height = c.height;
  if (auto s = cmd->set_scissor(sr); !s.ok()) {
    std::fprintf(stderr, "[fsk] set_scissor 失败: %s\n", s.message.c_str());
    return 1;
  }
  if (auto s = cmd->bind_pipeline(pipeline, 0x4653'4B31ull); !s.ok()) {
    std::fprintf(stderr, "[fsk] bind_pipeline 失败: %s\n", s.message.c_str());
    return 1;
  }
  struct SketchPC {
    float view_proj[16];
    float normal_matrix[16];
  } pc{};
  std::memcpy(pc.view_proj, camera->view_proj_matrix().m, sizeof(float) * 16);
  pc.normal_matrix[0] = 1.0f;
  pc.normal_matrix[5] = 1.0f;
  pc.normal_matrix[10] = 1.0f;
  pc.normal_matrix[15] = 1.0f;
  constexpr dev::u32 kStageVertex = 0x1u;
  constexpr dev::u32 kStageFragment = 0x10u;
  if (auto s = cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
    std::fprintf(stderr, "[fsk] push_constants 失败: %s\n", s.message.c_str());
    return 1;
  }
  if (auto s = cmd->bind_vertex_buffer(inst_buf->id(), 1, 0); !s.ok()) {
    std::fprintf(stderr, "[fsk] bind 实例缓冲失败: %s\n", s.message.c_str());
    return 1;
  }
  for (std::size_t i = 0; i < gpu.size(); ++i) {
    if (auto s = cmd->bind_vertex_buffer(gpu[i].vb->id(), 0, 0); !s.ok()) {
      std::fprintf(stderr, "[fsk] bind 顶点缓冲失败: %s\n", s.message.c_str());
      return 1;
    }
    if (auto s = cmd->bind_index_buffer(gpu[i].ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
      std::fprintf(stderr, "[fsk] bind 索引缓冲失败: %s\n", s.message.c_str());
      return 1;
    }
    res::DrawArgs da;
    da.index_count = gpu[i].index_count;
    da.instance_count = 1;
    da.first_instance = static_cast<res::u32>(i);
    if (auto s = cmd->draw_indexed(da); !s.ok()) {
      std::fprintf(stderr, "[fsk] draw_indexed 失败: %s\n", s.message.c_str());
      return 1;
    }
  }
  if (auto s = cmd->end_pass(); !s.ok()) {
    std::fprintf(stderr, "[fsk] end_pass 失败: %s\n", s.message.c_str());
    return 1;
  }
  if (auto s = device->submit_command_list(*cmd); !s.ok()) {
    std::fprintf(stderr, "[fsk] 提交失败: %s\n", s.message.c_str());
    return 1;
  }

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(c.width) * c.height * 4u, 0);
  if (auto s = target->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    std::fprintf(stderr, "[fsk] 读回失败: %s\n", s.message.c_str());
    return 1;
  }
  magicblocks::ImageRgba8 img;
  img.width = c.width;
  img.height = c.height;
  img.pixels = std::move(rgba);
  std::string err;
  if (!magicblocks::write_png_rgba8(output, img, &err)) {
    std::fprintf(stderr, "[fsk] 写 PNG 失败: %s\n", err.c_str());
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------- 子命令

int cmd_render(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  if (args.width <= 0 || args.height <= 0) {
    std::fprintf(stderr, "--width/--height 必须为正\n");
    return 2;
  }
  if (args.strokes <= 0) {
    // 0 笔 = 没有笔画。必须拒绝出图，而不是写一张纯背景 PNG ——
    // 后者会被 golden 锁死成"正确的图"。
    std::fprintf(stderr, "--strokes 必须 >= 1(0 笔没有可挤出的笔画)\n");
    return 2;
  }
  if (!(args.thickness > 0.0f)) {
    std::fprintf(stderr, "--thickness 必须 > 0\n");
    return 2;
  }
  if (!(args.canvas_width > 0.0f) || !(args.canvas_height > 0.0f)) {
    std::fprintf(stderr, "--canvas-width/--canvas-height 必须 > 0\n");
    return 2;
  }
  if (!(args.stroke_width > 0.0f)) {
    std::fprintf(stderr, "--stroke-width 必须 > 0\n");
    return 2;
  }

  Capture c;
  c.width = static_cast<std::uint32_t>(args.width);
  c.height = static_cast<std::uint32_t>(args.height);
  c.frame_index = args.frame_index;
  c.camera_ortho = (args.camera == "ortho") ? 1 : 0;
  // 正交视高 = 画布高度 * 1.25（留边），保证整张画布入画。
  c.ortho_height = args.canvas_height * 1.25f;
  c.thickness = args.thickness;
  c.canvas.width = args.canvas_width;
  c.canvas.height = args.canvas_height;
  c.canvas.extrude_thickness = args.thickness;
  c.canvas.clamp_to_canvas = true;
  c.stroke_width = args.stroke_width;
  c.region = args.region ? 1u : 0u;
  c.merge = args.merge ? 1u : 0u;
  c.events = make_demo_events(args.strokes, args.width, args.height, args.frame_index, args.region);
  c.input_hash = compute_input_hash(c);

  const CpuResult r = build_document(c);
  if (!r.ok) {
    std::fprintf(stderr, "[fsk] %s\n", r.error.c_str());
    return 1;
  }
  print_stats(c, r);

  const int rc = render_to_png(c, r, args.output);
  if (rc != 0) return rc;

  if (!args.record.empty()) {
    std::string err;
    const std::vector<std::uint8_t> bytes = encode_capture(c);
    if (!write_file(args.record, bytes, &err)) {
      std::fprintf(stderr, "[fsk] 写采集文件失败: %s\n", err.c_str());
      return 1;
    }
    std::printf("[fsk] 已录制事件流 %s (%zu 事件, %zu 字节)\n", args.record.c_str(),
                c.events.size(), bytes.size());
  }
  std::printf("[fsk] 已写出 %s (%dx%d)\n", args.output.c_str(), args.width, args.height);
  return 0;
}

// replay / reproduce 的共用出口：出图 + 可选的 verify 比对。
int replay_like(const Args& args, bool reproduce) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  std::vector<std::uint8_t> bytes;
  std::string err;
  if (!read_file(args.replay, &bytes, &err)) {
    std::fprintf(stderr, "[fsk] %s\n", err.c_str());
    return 1;
  }
  Capture c;
  if (!decode_capture(bytes, &c, &err)) {
    std::fprintf(stderr, "[fsk] %s\n", err.c_str());
    return 1;
  }

  if (reproduce) {
    // 复现：**不用**流里的事件，而是用流里的参数重新生成一遍。
    // 若生成器不确定（读了时钟/随机/文件状态），两串事件会不同，
    // 下面的哈希比对会立刻失败 —— 这正是复现与回放的分工。
    const int strokes = static_cast<int>([&c]() {
      // 从事件流里数出笔画数（每个 PointerDown 一笔）。
      int n = 0;
      for (const auto& e : c.events) {
        if (e.kind == dev::InputEventKind::PointerDown) ++n;
      }
      return n;
    }());
    const std::vector<dev::InputEvent> again =
        make_demo_events(strokes, static_cast<int>(c.width), static_cast<int>(c.height),
                         c.frame_index, c.region != 0);
    const std::uint64_t regenerated_hash = compute_input_hash([&]() {
      Capture t = c;
      t.events = again;
      return t;
    }());
    if (again.size() != c.events.size() || regenerated_hash != c.input_hash) {
      std::fprintf(stderr,
                   "[fsk] 复现失败: 重新生成的事件流与采集文件不一致"
                   "(事件 %zu vs %zu, input_hash %016llx vs %016llx)\n",
                   again.size(), c.events.size(),
                   static_cast<unsigned long long>(regenerated_hash),
                   static_cast<unsigned long long>(c.input_hash));
      return 1;
    }
    std::printf("[fsk] reproduce: 事件重新生成一致 (%zu 事件)\n", again.size());
  }

  const CpuResult r = build_document(c);
  if (!r.ok) {
    std::fprintf(stderr, "[fsk] %s\n", r.error.c_str());
    return 1;
  }
  print_stats(c, r);

  const int rc = render_to_png(c, r, args.output);
  if (rc != 0) return rc;
  std::printf("[fsk] 已写出 %s (%ux%u)\n", args.output.c_str(), c.width, c.height);

  if (args.verify && !reproduce) {
    // verify 在 replay 分支的语义：流的自带哈希必须能被重算复现
    // （坏文件/被改过的文件在这里被明确拒绝，而不是画出一张"看起来还行"的图）。
    if (compute_input_hash(c) != c.input_hash) {
      std::fprintf(stderr, "[fsk] verify 失败: 重算 input_hash 不符\n");
      return 1;
    }
    std::printf("[fsk] verify ok (input_hash=%016llx)\n",
                static_cast<unsigned long long>(c.input_hash));
  }
  return 0;
}

int cmd_dump(const Args& args) {
  std::vector<std::uint8_t> bytes;
  std::string err;
  if (!read_file(args.replay, &bytes, &err)) {
    std::fprintf(stderr, "[fsk] %s\n", err.c_str());
    return 1;
  }
  Capture c;
  if (!decode_capture(bytes, &c, &err)) {
    std::fprintf(stderr, "[fsk] %s\n", err.c_str());
    return 1;
  }
  if (args.json) {
    // 键序固定，两次输出字节相同（与 L2 的 dump --json 同纪律）。
    std::printf(
        "{\"capture_version\":%u,\"width\":%u,\"height\":%u,\"frame_index\":%u,"
        "\"camera\":\"%s\",\"thickness\":%.9g,\"canvas\":{\"width\":%.9g,\"height\":%.9g},"
        "\"stroke_width\":%.9g,\"region\":%u,\"merge\":%u,\"events\":%u,"
        "\"input_hash\":\"%016llx\"}\n",
        static_cast<unsigned>(kCaptureVersion), c.width, c.height, c.frame_index,
        c.camera_ortho ? "ortho" : "perspective", static_cast<double>(c.thickness),
        static_cast<double>(c.canvas.width), static_cast<double>(c.canvas.height),
        static_cast<double>(c.stroke_width), c.region, c.merge,
        static_cast<unsigned>(c.events.size()),
        static_cast<unsigned long long>(c.input_hash));
    return 0;
  }
  std::printf("capture_version=%u extent=%ux%u frame_index=%u camera=%s\n",
              static_cast<unsigned>(kCaptureVersion), c.width, c.height, c.frame_index,
              c.camera_ortho ? "ortho" : "perspective");
  std::printf("thickness=%.6g canvas=%.6gx%.6g stroke_width=%.6g region=%u merge=%u\n",
              static_cast<double>(c.thickness), static_cast<double>(c.canvas.width),
              static_cast<double>(c.canvas.height), static_cast<double>(c.stroke_width), c.region,
              c.merge);
  std::printf("events=%zu input_hash=%016llx\n", c.events.size(),
              static_cast<unsigned long long>(c.input_hash));
  for (std::size_t i = 0; i < c.events.size(); ++i) {
    const dev::InputEvent& e = c.events[i];
    std::printf("  [%zu] kind=%u pid=%u pos=(%.3f,%.3f) ts=%lld\n", i,
                static_cast<unsigned>(e.kind), e.pointer_id,
                static_cast<double>(e.position.x), static_cast<double>(e.position.y),
                static_cast<long long>(e.timestamp_us));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  const int pr = parse_args(argc, argv, &args);
  if (pr == -1) {
    print_usage();
    return 0;
  }
  if (pr != 0) {
    print_usage();
    return 2;
  }

  if (args.cmd == "render") return cmd_render(args);
  if (args.cmd == "replay" || args.cmd == "reproduce") {
    if (args.replay.empty()) {
      std::fprintf(stderr, "--replay 必填\n");
      return 2;
    }
    return replay_like(args, args.cmd == "reproduce");
  }
  if (args.cmd == "dump") {
    if (args.replay.empty()) {
      std::fprintf(stderr, "--replay 必填\n");
      return 2;
    }
    return cmd_dump(args);
  }
  print_usage();
  return 2;
}
