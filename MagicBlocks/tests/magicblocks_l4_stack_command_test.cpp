// F-VX 阶段 7：命令流三态（录制 / 回放 / 复现）+ 门禁用例
//
// 分两个 suite：
//   StackCommandMode.* —— 逻辑层三态。只碰 IBlockSet / IRecorder / ICommandStream，
//                         不需要设备，所以断言可以做到"逐值、逐字节"。
//   StackGates.*       —— 把 tools/ 下的门禁脚本当子进程跑一遍，
//                         把"门禁全绿"从 CI 里的口头结论变成 gtest 事实。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/features/stack/block_brush.h"
#include "mb_internal/magicblocks/features/stack/block_set.h"
#include "mb_internal/magicblocks/features/stack/stack_command.h"
#include "mb_internal/magicblocks/features/stack/stack_types.h"

namespace mbst = magicblocks::features::stack;

namespace {

// 与 CLI 同构的网格：8 层、0.5 格、半径 8（合法域 x/z ∈ [-8,8)）。
mbst::GridSpec cmd_grid() {
  mbst::GridSpec g{};
  g.origin[0] = 0.0f;
  g.origin[1] = 0.0f;
  g.origin[2] = 0.0f;
  g.cell_size = 0.5f;
  g.layers = 8;
  g.half_extent = 8;
  return g;
}

// 一段水平路径（按整数像素写，像素 * cell_size = 世界坐标）。
std::vector<mbst::PathPoint> line_path(int y) {
  std::vector<mbst::PathPoint> p;
  for (int x = -3; x <= 3; ++x) p.push_back(mbst::PathPoint{x, y});
  return p;
}

std::vector<mbst::StrokeScript> three_stroke_script() {
  std::vector<mbst::StrokeScript> s;
  s.push_back(mbst::StrokeScript{line_path(0), 0xE04040FFu, 0});
  s.push_back(mbst::StrokeScript{line_path(2), 0x40E040FFu, 1});
  s.push_back(mbst::StrokeScript{line_path(-2), 0x4040E0FFu, 2});
  return s;
}

}  // namespace

// ---------------------------------------------------------------- 三态

TEST(StackCommandMode, RecordReplayReproduceHashEqual) {
  // §13.3 的核心判据：三态两两相等。
  auto chk = mbst::check_three_states(three_stroke_script(), cmd_grid());
  ASSERT_TRUE(chk.ok()) << chk.text();
  const mbst::CommandCheck& c = chk.value();

  EXPECT_TRUE(c.stream_hash_equal) << "序列化往返丢了信息(流哈希不等)";
  EXPECT_TRUE(c.set_hash_equal)
      << "回放与录制真源不等: recorded=" << c.recorded_set_hash
      << " replayed=" << c.replayed_set_hash;
  EXPECT_TRUE(c.reproduce_equal) << "两次独立 build 结果不等 —— 链路存在非确定性";
  EXPECT_TRUE(c.blocks_equal) << "三态方块数不一致";
  // 总判据必须由上面四项共同决定，不能是独立算了另一个东西。
  EXPECT_TRUE(c.all_equal());
}

TEST(StackCommandMode, ReplayPreservesStrokeIdValues) {
  // **M10**：回放态每笔 stroke_id 与记录态**逐值相等**。
  //
  // 这条是"回放别偷懒重新编号"的唯一守卫。若回放走 next_stroke_id()
  // 重新分配，只要流的顺序与记录时一致，id 竟然**恰好**也会相同 ——
  // 所以光比最终 hash 抓不到。这里额外把 id 集合逐个列出来比。
  const auto grid = cmd_grid();
  auto script = three_stroke_script();

  auto set_r = mbst::create_block_set(grid);
  ASSERT_TRUE(set_r.ok()) << set_r.text();
  auto rec_set = std::move(set_r).take();
  auto rec = mbst::create_recorder();
  ASSERT_TRUE(rec->attach(rec_set.get(), grid).ok());
  for (const auto& s : script) {
    auto id = rec->stroke(s.path, s.rgba, s.layer);
    ASSERT_TRUE(id.ok()) << id.text();
  }

  std::vector<std::uint32_t> recorded_ids;
  rec_set->strokes_in_order(&recorded_ids);
  ASSERT_EQ(recorded_ids.size(), 3u);
  // 记录态 id 应当从 1 起单调递增（I2：单调不复用）。
  EXPECT_EQ(recorded_ids[0], 1u);
  EXPECT_EQ(recorded_ids[1], 2u);
  EXPECT_EQ(recorded_ids[2], 3u);

  auto rep_r = mbst::create_block_set(grid);
  ASSERT_TRUE(rep_r.ok());
  auto rep_set = std::move(rep_r).take();
  auto parsed = mbst::create_command_stream();
  std::string detail;
  ASSERT_TRUE(parsed->parse(rec->stream().serialize(), &detail)) << detail;
  mbst::StackResult<mbst::ReplayStats> rp = mbst::replay(*parsed, *rep_set, grid);
  ASSERT_TRUE(rp.ok()) << rp.text();

  std::vector<std::uint32_t> replayed_ids;
  rep_set->strokes_in_order(&replayed_ids);
  EXPECT_EQ(recorded_ids, replayed_ids) << "回放态的 stroke_id 序列与记录态不同";

  // 逐笔的格集合也必须一致（id 相同但内容不同同样算失败）。
  for (std::uint32_t id : recorded_ids) {
    std::vector<std::size_t> a, b;
    rec_set->indices_of_stroke(id, &a);
    rep_set->indices_of_stroke(id, &b);
    EXPECT_EQ(a.size(), b.size()) << "stroke " << id << " 的格数记录态与回放态不同";
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const mbst::Block* ba = rec_set->at(a[i]);
      const mbst::Block* bb = rep_set->at(b[i]);
      ASSERT_NE(ba, nullptr);
      ASSERT_NE(bb, nullptr);
      EXPECT_EQ(ba->cell.x, bb->cell.x);
      EXPECT_EQ(ba->cell.y, bb->cell.y);
      EXPECT_EQ(ba->cell.z, bb->cell.z);
      EXPECT_EQ(ba->color, bb->color);
      EXPECT_EQ(ba->stroke_id, bb->stroke_id);
    }
  }
}

TEST(StackCommandMode, SerializeParseRoundTripIsLossless) {
  // 序列化只是传输格式，不是有损日志。往返之后字段必须逐个恢复。
  auto stream = mbst::create_command_stream();
  mbst::EventRecord a;
  a.kind = mbst::EventKind::Stroke;
  a.stroke_id = 7;
  a.color = 0x11223344u;
  a.layer = 3;
  a.path = {mbst::PathPoint{-2, 5}, mbst::PathPoint{0, 0}, mbst::PathPoint{9, -9}};
  stream->append(a);

  mbst::EventRecord b;
  b.kind = mbst::EventKind::Undo;
  b.stroke_id = 7;
  stream->append(b);

  mbst::EventRecord c;
  c.kind = mbst::EventKind::SetColor;
  c.cell = mbst::Cell{1, 2, 3};
  c.color = 0xAABBCCDDu;
  stream->append(c);

  mbst::EventRecord d;
  d.kind = mbst::EventKind::LockLayer;
  d.layer = 5;
  stream->append(d);
  stream->append(mbst::EventRecord{mbst::EventKind::UnlockLayer, 0, {}, 0, -1, {}});

  ASSERT_EQ(stream->size(), 5u);
  const std::string text = stream->serialize();
  auto back = mbst::create_command_stream();
  std::string detail;
  ASSERT_TRUE(back->parse(text, &detail)) << detail;
  EXPECT_EQ(back->hash(), stream->hash()) << "往返后哈希变化\n原文:\n" << text;
  ASSERT_EQ(back->size(), 5u);
  EXPECT_EQ(back->at(0).path.size(), 3u);
  EXPECT_EQ(back->at(0).path[2].px, 9);
  EXPECT_EQ(back->at(0).path[2].py, -9);
  EXPECT_EQ(back->at(2).cell.z, 3);
  EXPECT_EQ(back->at(3).layer, 5);
}

TEST(StackCommandMode, ParseRejectsMalformedInput) {
  // 坏输入必须**明确失败**并给出行号，不静默跳过（D8）。
  auto stream = mbst::create_command_stream();
  std::string detail;

  EXPECT_FALSE(stream->parse("bogus_event 1 2\n", &detail));
  EXPECT_NE(detail.find("未知事件种类"), std::string::npos) << detail;

  detail.clear();
  EXPECT_FALSE(stream->parse("stroke 1 255 0 3 1 2\n", &detail));
  EXPECT_NE(detail.find("路径点数"), std::string::npos) << detail;

  detail.clear();
  EXPECT_FALSE(stream->parse("stroke 1 255 0 1 1 x 2\n", &detail));
  EXPECT_NE(detail.find("坐标"), std::string::npos) << detail;
}

TEST(StackCommandMode, UndoAndColorReplayIdentically) {
  // 撤销与改色也是事件，回放必须同样还原 —— 否则"回放态等于记录态"
  // 只在纯落笔的脚本上成立，那就是个假绿灯。
  const auto grid = cmd_grid();
  auto set_r = mbst::create_block_set(grid);
  ASSERT_TRUE(set_r.ok());
  auto set = std::move(set_r).take();

  auto rec = mbst::create_recorder();
  ASSERT_TRUE(rec->attach(set.get(), grid).ok());

  auto id1 = rec->stroke(line_path(0), 0xFF0000FFu, 0);
  ASSERT_TRUE(id1.ok()) << id1.text();
  auto id2 = rec->stroke(line_path(3), 0x00FF00FFu, 1);
  ASSERT_TRUE(id2.ok()) << id2.text();
  const std::size_t after_two = set->size();

  // 撤掉第二笔：真源必须恰好少掉第二笔的块数。
  ASSERT_TRUE(rec->undo(id2.value()).ok());
  EXPECT_LT(set->size(), after_two);

  // 改第一笔某一块的颜色。
  mbst::Cell target{-3, 0, 0};
  ASSERT_NE(set->find(target), nullptr) << "预设格不存在，测试前提被破坏";
  ASSERT_TRUE(rec->set_color(target, 0x123456FFu).ok());
  EXPECT_EQ(set->find(target)->color, 0x123456FFu);

  const std::uint64_t recorded_hash = set->hash();

  // 回放。
  auto rep_r = mbst::create_block_set(grid);
  ASSERT_TRUE(rep_r.ok());
  auto rep_set = std::move(rep_r).take();
  mbst::StackResult<mbst::ReplayStats> rp2 = mbst::replay(rec->stream(), *rep_set, grid);
  ASSERT_TRUE(rp2.ok()) << rp2.text();
  EXPECT_EQ(rep_set->hash(), recorded_hash) << "撤销/改色路径回放不等";
  EXPECT_EQ(rp2.value().undos, 1u);
  EXPECT_EQ(rp2.value().color_sets, 1u);
  EXPECT_EQ(rep_set->find(target)->color, 0x123456FFu);
}

TEST(StackCommandMode, BuildIsDeterministicAcrossInstances) {
  // 复现态：两次全新 build 的流哈希与真源哈希都相等。
  // 这里单独测 build()，因为 check_three_states 的 reproduce 分支
  // 只比一次，而"多跑几次都相等"才是确定性。
  const auto grid = cmd_grid();
  const auto script = three_stroke_script();
  auto a = mbst::build(script, grid);
  ASSERT_TRUE(a.ok()) << a.text();
  for (int i = 0; i < 5; ++i) {
    auto b = mbst::build(script, grid);
    ASSERT_TRUE(b.ok()) << b.text();
    EXPECT_EQ(a.value().stream_hash, b.value().stream_hash)
        << "第 " << i << " 次复现的流哈希不同";
    EXPECT_EQ(a.value().set_hash, b.value().set_hash) << "第 " << i << " 次复现的真源哈希不同";
    EXPECT_EQ(a.value().blocks, b.value().blocks);
  }
  EXPECT_GT(a.value().blocks, 0u);
}

TEST(StackCommandMode, StreamHashIsOrderSensitive) {
  // 流哈希必须**对顺序敏感**。若它只对"事件集合"敏感，把回放顺序搞错
  // 也照样通过三态校验 —— 那三态校验就废了。
  auto s1 = mbst::create_command_stream();
  auto s2 = mbst::create_command_stream();
  mbst::EventRecord a{mbst::EventKind::LockLayer, 0, {}, 0, 0, {}};
  mbst::EventRecord b{mbst::EventKind::LockLayer, 0, {}, 0, 3, {}};
  s1->append(a);
  s1->append(b);
  s2->append(b);
  s2->append(a);
  EXPECT_NE(s1->hash(), s2->hash()) << "流哈希对事件顺序不敏感";
}

// ---------------------------------------------------------------- 门禁

namespace {

#if defined(_WIN32)
#define MBG_POPEN _popen
#define MBG_PCLOSE _pclose
#else
#define MBG_POPEN popen
#define MBG_PCLOSE pclose
#endif

// 跑一个 python 脚本，返回退出码，输出写进 out。
//
// 两个必须的细节：
//  * `PYTHONIOENCODING=utf-8`：门禁脚本输出中文，子进程在 cmd 下默认用 GBK，
//    中文会变成乱码 —— 而在乱码里 find("扫描") 永远找不到，用例就会
//    "因为编码问题"假失败（反向同理，也会有人拿它当假通过）。
//  * 二进制模式读：不经过 CRT 的文本翻译，拿到的就是脚本真正吐出的字节。
int run_python(const std::string& script, const std::string& extra_args, std::string* out) {
  const std::string inner = "set PYTHONIOENCODING=utf-8&& \"" + std::string(MB_PYTHON_EXE) +
                            "\" \"" + MB_MINE_ROOT + "/" + script + "\" " + extra_args +
                            " 2>&1";
  const std::string cmd = "\"" + inner + "\"";
  FILE* p = MBG_POPEN(cmd.c_str(), "rb");
  if (p == nullptr) return -999;
  char buf[4096];
  out->clear();
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out->append(buf, n);
  return MBG_PCLOSE(p);
}

}  // namespace

TEST(StackGates, G7GateExitZeroAndScansNTimes) {
  // G7：primitives 里不得出现玩法名。**可测版**：直接跑脚本，
  // 断言 exit 0 且输出里确实有"扫描 N 个文件"、N > 0。
  //
  // 特意不写"在 primitives 里塞一个 stack 期望命中"那种用例 ——
  // `stack` 不在 G7 词表内（`std::stack` 是标准库名），那种用例必然空过，
  // 也就是一条永远绿的假测试。
  std::string out;
  const int rc = run_python("tools/check_l3_no_gameplay_names.py",
                            "--root \"" + std::string(MB_SOURCE_ROOT) + "\"", &out);
  EXPECT_EQ(rc, 0) << "G7 门禁失败:\n" << out;
  EXPECT_NE(out.find("扫描"), std::string::npos) << "G7 没有输出扫描统计:\n" << out;

  // 从输出里抠出扫描文件数，必须 > 0 —— 证明脚本真的扫了东西。
  const std::size_t p = out.find("扫描");
  if (p != std::string::npos) {
    long n = std::strtol(out.c_str() + p + std::strlen("扫描"), nullptr, 10);
    EXPECT_GT(n, 0) << "G7 扫描文件数为 0, 等于没查:\n" << out;
  }
}

TEST(StackGates, G3LayerPurityExitZero) {
  // G3：分层纯度（L4 不得被更底层依赖；tests/ 也只能用公共头）。
  std::string out;
  const int rc = run_python("tools/check_layer_purity.py",
                            "--root \"" + std::string(MB_SOURCE_ROOT) + "\"", &out);
  EXPECT_EQ(rc, 0) << "G3 门禁失败:\n" << out;
}

TEST(StackGates, NoVkInFeaturesStack) {
  // A13：`src/features/stack/` 内不得出现原生 Vulkan 调用。
  // 这条**不靠** G3（G3 管头文件包含方向），而是直接扫源码文本。
  //
  // 做法：把扫描逻辑写成临时 .py 再执行，而不是塞进 `python -c "..."`。
  // v1 就是被内联引号坑了 —— cmd.exe 把 python.exe" -c "import 当成一个
  // 坏掉的命令名，于是三条断言全红但一条真问题都没有（那次是假失败）。
  // 让 shell 只负责启动一个脚本文件，引号问题就彻底消失。
  const std::string dir = std::string(MB_SOURCE_ROOT) + "/src/features/stack";
  const std::string py = std::string(MB_SOURCE_ROOT) + "/tests/_scan_vk.py";

  const char* kPyBody =
      "import sys, glob, os\n"
      "d = sys.argv[1]\n"
      "bad = []\n"
      "files = sorted(glob.glob(os.path.join(d, '*')))\n"
      "files = [p for p in files if os.path.isfile(p)]\n"
      "for p in files:\n"
      "    text = open(p, encoding='utf-8', errors='ignore').read()\n"
      "    for sym in ['vkCmd', 'VkCommandBuffer', 'CmdDraw', 'vkCreate']:\n"
      "        if sym in text:\n"
      "            bad.append(p + ': ' + sym)\n"
      "if bad:\n"
      "    print(chr(10).join(bad))\n"
      "    sys.exit(1)\n"
      "print('scanned ' + str(len(files)) + ' files')\n";
  {
    FILE* f = std::fopen(py.c_str(), "wb");
    ASSERT_NE(f, nullptr) << "写不了临时扫描脚本: " << py;
    const std::string body(kPyBody);
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
  }

  std::string out;
  const std::string inner = "set PYTHONIOENCODING=utf-8&& \"" + std::string(MB_PYTHON_EXE) +
                            "\" \"" + py + "\" \"" + dir + "\" 2>&1";
  const std::string cmd = "\"" + inner + "\"";
  FILE* p = MBG_POPEN(cmd.c_str(), "rb");
  if (p == nullptr) {
    std::remove(py.c_str());
    FAIL() << "拉不起子进程";
  }
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
  const int rc = MBG_PCLOSE(p);
  std::remove(py.c_str());

  EXPECT_EQ(rc, 0) << "src/features/stack/ 里出现了原生 Vulkan 符号:\n" << out;
  EXPECT_NE(out.find("scanned"), std::string::npos)
      << "扫描本身没跑起来（不是通过，是空过）:\n" << out;
}
