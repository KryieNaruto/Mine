// MagicBlocks L4 —— 魔法叠叠乐(F-VX) golden 基线：结构与像素护栏（GAP-07 / §15 A10）
//
// ============================ 这个文件解决什么 ============================
// 审计（2026-11-11）指出 F-VX 的 golden 基线**从未落地**：`tests/data/` 只有
// golden_l0/l2/ui，`CMakeLists.txt` 里 grep `golden_l4` 零命中。没有基线的后果是
// "画出东西且确定性"能验、"画的是不是需求要的那个东西"验不了 —— 旧用例只能看
// 「PNG 字节数 > 2000」，而实测 320x200 的 9 张图**字节数完全相同**（256283），
// 那类断言对内容毫无判别力。
//
// 本文件做两件事：
//   ① **结构断言**（不依赖 golden 文件内容，防"错误基线自我固化"）：
//      非背景像素占比落在区间内、四角必须是背景、包围盒不贴满、画面中心有东西；
//   ② **像素比对**：与 `tests/data/golden_l4_stack_<case>.png` 比，容差 ≤1%
//      （同平台同驱动应当逐字节相同，容差只为跨驱动留口）。
//
// 解码走 `core/image_io.cpp` 的 `read_png_rgba8`（与 CLI 写盘同一个模块），
// 不在这里另写一份 PNG 解析 —— 两处一旦分叉，"golden 对不上"会变成定位不了的现象。
//
// ============================ 重新生成 golden ============================
// **只有当渲染逻辑有意变更时才重新生成**，且生成后必须：
//   a) 跑 `--gtest_filter=StackGolden.*`；
//   b) 用 `MagicBlocks\tools\png_inspect.py <png> --colors 8` 人工核对结构数字；
//   c) 在 `tests/data/README.md` 里登记（谁、何时、为什么、结构数字）。
//
//   magicblocks-l3-cli stack --width 320 --height 200 --layers 8 --strokes 3 \
//       --output <MB_GOLDEN_DIR>/golden_l4_stack_basic.png
//   ... --grid-overlay off     --output ...golden_l4_stack_no_overlay.png
//   ... --color-mode by-layer  --output ...golden_l4_stack_by_layer.png
//
// 另有 `--inner-pass off` 一条**暂不纳入**（原因见 kGoldenCases 上方的说明）。
//
// 缺 golden 文件时**直接失败**（不是跳过）：缺基线等于"没有护栏"，那必须响。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/image_io.h"

#if defined(_WIN32)
#include <process.h>  // _getpid
#define MBG_POPEN _popen
#define MBG_PCLOSE _pclose
#define MBG_GETPID _getpid
#else
#include <unistd.h>  // getpid
#define MBG_POPEN popen
#define MBG_PCLOSE pclose
#define MBG_GETPID getpid
#endif

namespace {

using magicblocks::ImageRgba8;

// 清屏色 #0f121a（CLI 的固定清屏色，与 tests/data/README 的登记一致）。
constexpr std::uint8_t kClearR = 15, kClearG = 18, kClearB = 26;

// ---- 结构统计：只对像素值做，不依赖 golden ----
struct Structure {
  std::size_t total = 0;
  std::size_t foreground = 0;
  int distinct = 0;
  int min_x = 1 << 30, max_x = -1, min_y = 1 << 30, max_y = -1;
  bool corners_are_clear = false;
  bool center_has_ink = false;
  double fg_ratio() const {
    return total == 0 ? 0.0 : static_cast<double>(foreground) / static_cast<double>(total);
  }
  int bbox_w() const { return max_x >= min_x ? max_x - min_x + 1 : 0; }
  int bbox_h() const { return max_y >= min_y ? max_y - min_y + 1 : 0; }
};

bool is_bg(const std::vector<std::uint8_t>& px, std::size_t i, int tol) {
  return std::abs(static_cast<int>(px[i + 0]) - static_cast<int>(kClearR)) <= tol &&
         std::abs(static_cast<int>(px[i + 1]) - static_cast<int>(kClearG)) <= tol &&
         std::abs(static_cast<int>(px[i + 2]) - static_cast<int>(kClearB)) <= tol;
}

Structure analyze(const ImageRgba8& img) {
  Structure st;
  st.total = static_cast<std::size_t>(img.width) * img.height;
  const int tol = 6;
  std::map<std::uint32_t, int> hist;
  for (std::uint32_t y = 0; y < img.height; ++y) {
    for (std::uint32_t x = 0; x < img.width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4u;
      if (!is_bg(img.pixels, i, tol)) {
        ++st.foreground;
        if (static_cast<int>(x) < st.min_x) st.min_x = static_cast<int>(x);
        if (static_cast<int>(x) > st.max_x) st.max_x = static_cast<int>(x);
        if (static_cast<int>(y) < st.min_y) st.min_y = static_cast<int>(y);
        if (static_cast<int>(y) > st.max_y) st.max_y = static_cast<int>(y);
      }
      const std::uint32_t c = (static_cast<std::uint32_t>(img.pixels[i + 0]) << 16) |
                              (static_cast<std::uint32_t>(img.pixels[i + 1]) << 8) |
                              static_cast<std::uint32_t>(img.pixels[i + 2]);
      ++hist[c];
    }
  }
  st.distinct = static_cast<int>(hist.size());

  const struct {
    std::uint32_t x, y;
  } corners[4] = {{0, 0},
                  {img.width - 1, 0},
                  {0, img.height - 1},
                  {img.width - 1, img.height - 1}};
  st.corners_are_clear = true;
  for (const auto& c : corners) {
    const std::size_t i = (static_cast<std::size_t>(c.y) * img.width + c.x) * 4u;
    if (!is_bg(img.pixels, i, tol)) st.corners_are_clear = false;
  }

  // 画面中心 17x17 邻域必须有非背景像素（"几何压到画面边角"= 出了图但等于没画）。
  const int cx = static_cast<int>(img.width / 2);
  const int cy = static_cast<int>(img.height / 2);
  st.center_has_ink = false;
  for (int dy = -8; dy <= 8 && !st.center_has_ink; ++dy) {
    for (int dx = -8; dx <= 8; ++dx) {
      const int x = cx + dx, y = cy + dy;
      if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height)) {
        continue;
      }
      const std::size_t i = (static_cast<std::size_t>(y) * img.width + static_cast<std::uint32_t>(x)) * 4u;
      if (!is_bg(img.pixels, i, tol)) {
        st.center_has_ink = true;
        break;
      }
    }
  }
  return st;
}

ImageRgba8 load_png(const std::string& path, const char* what) {
  ImageRgba8 img;
  std::string err;
  if (!magicblocks::read_png_rgba8(path, &img, &err)) {
    ADD_FAILURE() << "[" << what << "] 读不到/解不开 PNG: " << path << " (" << err << ")";
  }
  return img;
}

int run_stack_cli(const std::string& args, std::string* out) {
  const std::string inner = std::string("\"") + MB_L3_CLI_PATH + "\" stack " + args + " 2>&1";
  const std::string cmd = std::string("\"") + inner + "\"";
  FILE* p = MBG_POPEN(cmd.c_str(), "r");
  if (p == nullptr) return -999;
  char buf[4096];
  out->clear();
  while (std::fgets(buf, sizeof(buf), p) != nullptr) out->append(buf);
  return MBG_PCLOSE(p);
}

std::string golden_path(const char* name) {
  return std::string(MB_GOLDEN_DIR) + "/golden_l4_stack_" + name + ".png";
}

// 中间产物必须**带进程号**。
//
// 2026-11-11 实测：原先这里是纯固定名 `_out_l4_stack_golden_<name>.png`，
// 而本文件的出图靠 `_popen` 拉起子进程。于是两份测试进程（并行 ctest、
// 或两个构建目录同时跑）会写同一个文件：A 刚写完、B 的 std::remove 就把它删了，
// A 再 load_png 就拿到空/半截图 → 偶发红。实测 8 个并发进程里 2 红，
// 同一组用例单跑 20/20 全绿 —— 是测试自身的写冲突，不是渲染缺陷。
//
// 加进程号后每个进程各写各的，互不可见。绝不要为了"文件名好看"把它改回去。
//
// 【副作用与对策】带进程号意味着不再覆盖旧文件，中间产物会**累积**。
// qa 实测多轮 j8/j16/全量跑之后 tests/data 里堆到 766 个 / 170.9 MB
// （已被 .gitignore 的 `_out_*.png` 挡住，不污染仓库，但会持续吃盘）。
// 所以下面登记本进程写过的每个路径，进程退出时统一删掉 ——
// 不依赖"谁记得手动清"，也不需要额外的 ctest 清理脚本。
//
// 【为什么是"永不析构"的指针而不是普通的函数内 static 对象】
// atexit 回调与静态局部对象的析构注册在**同一张退出表**里，后注册者先执行。
// 而这里的 vector 是在 arm_tmp_cleanup() **之后**才首次构造的（arm 里只碰了
// 另一个 static bool），于是它的析构比 atexit 回调后注册、也就先执行 ——
// 回调真正跑起来时列表已经被析构清空，std::remove 一个都删不到。
// 实测就是这个现象：push 时 size=1/2/3，atexit 里恒为 0，文件全部残留。
// 故意让容器永不析构（进程退出时由 OS 回收），把它的寿命拉到所有 atexit
// 回调之后。代价是 sizeof(vector) 的常量泄漏，收益是清理真的会发生。
std::vector<std::string>& process_tmp_files() {
  static std::vector<std::string>* files = new std::vector<std::string>();
  return *files;
}

void remove_process_tmp_files() {
  for (const auto& f : process_tmp_files()) std::remove(f.c_str());
  process_tmp_files().clear();
}

void arm_tmp_cleanup() {
  static bool armed = false;
  if (armed) return;
  armed = true;
  std::atexit(&remove_process_tmp_files);
}

std::string tmp_path(const char* name) {
  arm_tmp_cleanup();
  const auto pid = static_cast<unsigned long>(MBG_GETPID());
  std::string p = std::string(MB_TEST_DATA_DIR) + "/_out_l4_stack_golden_" + name + "_" +
                  std::to_string(pid) + ".png";
  process_tmp_files().push_back(p);
  return p;
}

// 一次完整的「跑 CLI → 结构断言 → 与 golden 比像素」。
void expect_matches_golden(const char* name, const std::string& cli_args, double fg_lo, double fg_hi,
                           int min_distinct) {
  const std::string out = tmp_path(name);
  std::remove(out.c_str());
  std::string log;
  ASSERT_EQ(run_stack_cli(cli_args + " --output \"" + out + "\"", &log), 0) << log;

  const ImageRgba8 actual = load_png(out, "实跑输出");
  ASSERT_EQ(actual.byte_size(), actual.pixels.size()) << "实跑输出为空/尺寸为 0";
  const ImageRgba8 golden = load_png(golden_path(name), "golden 基线");
  ASSERT_FALSE(golden.pixels.empty())
      << "golden 基线缺失/为空 —— 缺基线等于没有护栏，必须按本文件顶部的要求生成并登记";
  ASSERT_EQ(actual.width, golden.width);
  ASSERT_EQ(actual.height, golden.height);

  // ---- ① 结构断言（对实跑输出做；与 golden 内容无关）----
  const Structure st = analyze(actual);
  EXPECT_TRUE(st.corners_are_clear) << "[" << name << "] 四角不是背景 —— 整帧被擦黑/染色";
  EXPECT_TRUE(st.center_has_ink) << "[" << name << "] 画面中心 17x17 全是背景 —— 几何不在画面正中";
  EXPECT_GE(st.fg_ratio(), fg_lo) << "[" << name << "] 前景占比过低: " << st.fg_ratio();
  EXPECT_LE(st.fg_ratio(), fg_hi) << "[" << name << "] 前景占比过高: " << st.fg_ratio();
  EXPECT_GT(st.distinct, min_distinct) << "[" << name << "] 颜色种数太少: " << st.distinct;
  EXPECT_GT(st.bbox_w(), 8) << "[" << name << "] 包围盒过窄 " << st.bbox_w();
  EXPECT_GT(st.bbox_h(), 8) << "[" << name << "] 包围盒过矮 " << st.bbox_h();
  EXPECT_LT(static_cast<double>(st.bbox_w()), 0.98 * actual.width) << "包围盒横向贴满整幅";
  EXPECT_LT(static_cast<double>(st.bbox_h()), 0.98 * actual.height) << "包围盒纵向贴满整幅";

  // ---- ② 与 golden 逐像素比对（容差 4/255，差异像素 < 1%）----
  const std::size_t differing = magicblocks::count_differing_pixels(actual, golden, 4);
  const double ratio = static_cast<double>(differing) / static_cast<double>(st.total);
  EXPECT_LT(ratio, 0.01) << "[" << name << "] 与 golden 差异 " << differing << " 像素 (" << ratio << ")";
}

}  // namespace

// ================================================================ 用例

// 当前纳入 golden 的场景。
//
// 关于 `inner_off`（--inner-pass off）：一度**刻意排除**，原因是 captain 在共享构建目录
// 观测到 `InnerPass*` 出现 diff=0（内面 pass 根本没画），而独立构建 `build-fvx` 里
// `StackRender*` 9/9 全绿 —— 两边观测不一致时录基线等于把未确认行为固化成"正确答案"。
//
// 2026-11-11 该不一致已解除：独立结构验证脚本（build-fvx/verify_structure.py）实测
//   内面 pass ON 的颜色种数 5 > OFF 的 3，且两图相差 2129 像素；
// 即内面 pass **确实在画东西**。于是 `inner_off` 现在纳入基线，并额外断言
// "inner_off 与 basic 必须不同"—— 这条能直接抓住"内面 pass 静默失效"回归。
inline constexpr const char* kGoldenCases[] = {"basic", "no_overlay", "inner_off", "by_layer"};

TEST(StackGolden, GoldenFilesExistAndDecode) {
  // 每个纳入的场景基线必须都在且能解开。缺一个 = 那条场景没有护栏，必须响。
  for (const char* n : kGoldenCases) {
    const std::string p = golden_path(n);
    ImageRgba8 img;
    std::string err;
    EXPECT_TRUE(magicblocks::read_png_rgba8(p, &img, &err)) << "缺少/无法读取 golden: " << p << " (" << err << ")";
    EXPECT_EQ(img.width, 320u) << p;
    EXPECT_EQ(img.height, 200u) << p;
  }
}

TEST(StackGolden, BasicMatchesGoldenWithStructure) {
  expect_matches_golden("basic", "--width 320 --height 200 --layers 8 --strokes 3", 0.008, 0.40, 6);
}

TEST(StackGolden, InnerPassOffMatchesGoldenWithStructure) {
  // --inner-pass off 只画表面 pass。它必须**不等于** basic（内面 pass 确实参与绘制），
  // 否则说明内面 pass 静默失效 —— 那正是 2026-11-11 那次事故的形态。
  expect_matches_golden("inner_off",
                        "--width 320 --height 200 --layers 8 --strokes 3 --inner-pass off", 0.008, 0.40,
                        3);

  ImageRgba8 a, b;
  std::string err;
  ASSERT_TRUE(magicblocks::read_png_rgba8(golden_path("inner_off"), &a, &err)) << err;
  ASSERT_TRUE(magicblocks::read_png_rgba8(golden_path("basic"), &b, &err)) << err;
  const std::size_t diff = magicblocks::count_differing_pixels(a, b, 0);
  EXPECT_GT(diff, 500u)
      << "inner_off 与 basic 几乎相同(差异 " << diff << " 像素)—— 内面 pass 没有参与绘制";
}

TEST(StackGolden, ByLayerMatchesGoldenWithStructure) {
  // 按层改色：几何不变、颜色变多。
  expect_matches_golden("by_layer",
                        "--width 320 --height 200 --layers 8 --strokes 3 --color-mode by-layer", 0.008,
                        0.40, 8);}

TEST(StackGolden, GridOverlayChangesPixels) {
  // 需求「带网格指示」的**集成级**证据，独立于 golden：
  // 同参数只差 --grid-overlay on/off，两张图必须明显不同 —— 证明覆盖层真的进了出图路径。
  // （golden 双双换新也照样成立，因为它比的是"本次运行的两张图"。）
  const std::string on = tmp_path("ov_on");
  const std::string off = tmp_path("ov_off");
  std::remove(on.c_str());
  std::remove(off.c_str());
  std::string log;
  ASSERT_EQ(run_stack_cli("--width 320 --height 200 --strokes 3 --grid-overlay on --output \"" + on + "\"",
                          &log),
            0)
      << log;
  ASSERT_NE(log.find("网格指示 on("), std::string::npos) << "CLI 没报告网格指示已接线: " << log;
  ASSERT_EQ(
      run_stack_cli("--width 320 --height 200 --strokes 3 --grid-overlay off --output \"" + off + "\"",
                    &log),
      0)
      << log;
  ASSERT_NE(log.find("网格指示 off("), std::string::npos) << log;

  const ImageRgba8 a = load_png(on, "overlay on");
  const ImageRgba8 b = load_png(off, "overlay off");
  ASSERT_EQ(a.width, b.width);
  ASSERT_EQ(a.height, b.height);
  ASSERT_GT(a.width, 0u);

  const std::size_t differing = magicblocks::count_differing_pixels(a, b, 0);
  const std::size_t total = static_cast<std::size_t>(a.width) * a.height;
  const double ratio = static_cast<double>(differing) / static_cast<double>(total);
  // 网格线是稀疏的：至少 200 个像素不同，但不能把整幅换掉（那就不是"指示"而是"挡板"）。
  EXPECT_GT(differing, 200u) << "开/关网格指示的像素完全相同 —— 覆盖层根本没进出图路径";
  EXPECT_LT(ratio, 0.30) << "开/关差异占 " << ratio << "，网格指示把画面遮住了大半";

  // 两张图都必须结构成立：覆盖层不许把积木吃掉、也不许把整帧擦黑。
  EXPECT_TRUE(analyze(b).corners_are_clear) << "关掉网格时四角不是背景";
  EXPECT_TRUE(analyze(a).corners_are_clear) << "开网格后四角不是背景 —— 整帧被擦黑";
  const Structure sa = analyze(a);
  EXPECT_GT(sa.foreground, analyze(b).foreground) << "开网格后前景像素反而更少 —— 覆盖层在擦除画面";
}

TEST(StackGolden, NoOverlayMatchesGoldenWithStructure) {
  // 关掉网格指示的基线：与 basic 同参数，唯一差别是 --grid-overlay off。
  // 这条基线存在的意义：将来若把"网格指示缺省开"改回关，两套基线会立刻分叉并被抓住。
  //
  // 颜色种数下限取 5：实测 no_overlay 恰好 6 种（背景 + 4 个明暗面 + 1 个过渡色），
  // 而 basic（开网格）是 52 种。写 6 会卡在边界上（`distinct > 6` 要求 ≥7），
  // 这里留 1 的余量，真正的判别力由"与 golden 逐像素比 + 开/关两图必须不同"承担。
  expect_matches_golden("no_overlay",
                        "--width 320 --height 200 --layers 8 --strokes 3 --grid-overlay off", 0.008,
                        0.40, 5);
}
