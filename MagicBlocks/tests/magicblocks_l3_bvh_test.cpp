// MagicBlocks L3 —— IPicker 的 BVH 加速（spec §13 M4 第 20 项）
//
// 这一组测试的**唯一目的**：证明加速没有改变结果。
//
// 为什么这么写：BVH 是纯粹的"少做无用功"优化，它的正确性标准不是
// "能点到东西"，而是"和暴力扫描给出同一答案"。
// 一个漏掉子树的 BVH 在大多数像素上仍然是对的 —— 只在某些角度、
// 某些重叠处悄悄漏检，肉眼几乎不可能发现。所以这里用
// **逐像素比对**而不是抽查：
//
//   n <= 8 个对象  → 走线性扫描
//   n >  8 个对象  → 走 BVH
//
// 两条路径的结果必须逐像素相同（命中的 entity 与距离）。

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/picker.h"
#include "magicblocks/primitives/types.h"

namespace mbp = magicblocks::primitives;

namespace {

constexpr int kWidth = 200;
constexpr int kHeight = 140;

// 从高处俯视原点，屏幕上 = -Z。
mbp::CameraPose look_down_from(float x, float y, float z) {
  mbp::CameraPose p;
  p.position = {x, y, z};
  p.target = {0.0f, 0.0f, 0.0f};
  p.up = {0.0f, 0.0f, -1.0f};
  return p;
}

std::unique_ptr<mbp::ICamera> make_camera(float y, int w, int h) {
  auto cam = mbp::create_camera();
  mbp::CameraDesc d;
  d.kind = mbp::ProjectionKind::Perspective;
  d.fov_y_degrees = 50.0f;
  d.near_plane = 0.1f;
  d.far_plane = 200.0f;
  d.viewport.width = static_cast<mbp::u32>(w);
  d.viewport.height = static_cast<mbp::u32>(h);
  EXPECT_TRUE(cam->set_desc(d).ok());
  EXPECT_TRUE(cam->set_pose(look_down_from(0.0f, y, 0.0f)).ok());
  return cam;
}

mbp::Box box_at(float x, float y, float z, float half) {
  return mbp::Box{mbp::Vec3{x - half, y - half, z - half},
                  mbp::Vec3{x + half, y + half, z + half}, false};
}

struct Scene {
  std::vector<mbp::Box> boxes;
  std::vector<std::uint32_t> entities;
};

// 构造散布在**一个大平面**上、彼此不重叠的小盒子。
//
// 为什么不用"沿 Z 轴纵深排列"：那种布局下所有盒子的并集会连成
// 一根贯穿全场的长条，任何一个子节点盒都覆盖整条纵深，
// 于是射线到任一节点盒的进入距离都极小，`t > best` 永不成立 ——
// BVH 结构建对了，但一次剪枝都发生不了（实测 pruned == 0）。
// 那是场景的问题，不是实现的问题，用它是测不出剪枝的。
//
// 平面网格布局才能体现 BVH 的价值：射线只穿过它真正扫过的那些格子。
Scene make_grid_scene(int side) {
  Scene s;
  const float spacing = 1.2f;
  const float half = 0.4f;
  for (int iz = 0; iz < side; ++iz) {
    for (int ix = 0; ix < side; ++ix) {
      const int idx = iz * side + ix;
      const float f = static_cast<float>(idx);
      const float x = (static_cast<float>(ix) - static_cast<float>(side - 1) * 0.5f) * spacing;
      const float z = (static_cast<float>(iz) - static_cast<float>(side - 1) * 0.5f) * spacing;
      const float y = std::sin(f * 0.9f) * 0.15f;
      s.boxes.push_back(box_at(x, y, z, half));
      s.entities.push_back(static_cast<std::uint32_t>(1000 + idx));
    }
  }
  return s;
}

std::unique_ptr<mbp::IPicker> picker_with(const Scene& s) {
  auto p = mbp::create_picker();
  p->begin_scene();
  for (std::size_t i = 0; i < s.boxes.size(); ++i) {
    p->add_object(s.entities[i], s.boxes[i]);
  }
  return p;
}

struct Sample {
  bool hit = false;
  std::uint32_t entity = 0;
  float distance = -1.0f;
};

std::vector<Sample> sweep(mbp::IPicker& picker, const mbp::ICamera& cam, int w, int h, int step) {
  std::vector<Sample> out;
  out.reserve(static_cast<std::size_t>((w / step + 1) * (h / step + 1)));
  for (int py = 0; py < h; py += step) {
    for (int px = 0; px < w; px += step) {
      mbp::PickHit hit;
      Sample s;
      if (picker.pick_scene(cam, static_cast<float>(px), static_cast<float>(py), &hit)) {
        s.hit = true;
        s.entity = hit.entity;
        s.distance = hit.distance;
      }
      out.push_back(s);
    }
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------------

TEST(PickerBvh, BvhResultMatchesLinearScanExactly) {
  // 真正的对照实验：同一个场景，只让对象数量跨过 BVH 阈值。
  //   8 个  → 线性扫描
  //   9 个  → BVH
  // 第 9 个对象埋在原点正下方很深处，任何从上方来的射线都够不到它，
  // 所以它不参与任何命中，两条路径**应当逐像素完全相同**。
  //
  // 这是本文件最重要的一条：它把"BVH 漏检"变成一个可判定的命题。
  // 若 BVH 漏掉某棵子树，某个像素上就会少一个命中，mismatch 立刻 > 0。
  Scene linear = make_grid_scene(3);  // 9 个
  linear.boxes.pop_back();            // 裁到 8 个 → 线性扫描
  linear.entities.pop_back();

  Scene bvh = linear;  // 同样的 8 个
  // 加一个埋在深处的盒子：位于原点下方 -60，从 +Y 来的射线要穿过
  // 整个场景才可能碰到；而 far_plane=200 且它被其它物体遮挡，
  // 实际上永远不是最近命中。
  bvh.boxes.push_back(box_at(0.0f, -60.0f, 0.0f, 0.3f));
  bvh.entities.push_back(999999u);

  auto cam = make_camera(14.0f, kWidth, kHeight);
  auto p_linear = picker_with(linear);
  auto p_bvh = picker_with(bvh);

  const std::vector<Sample> a = sweep(*p_linear, *cam, kWidth, kHeight, 1);
  const std::vector<Sample> b = sweep(*p_bvh, *cam, kWidth, kHeight, 1);
  ASSERT_EQ(a.size(), b.size());

  // 确认两条路径真的被走到了，否则比对毫无意义。
  EXPECT_GT(p_linear->stats().entities_tested, 0u) << "线性路径没被触发";
  EXPECT_GT(p_bvh->stats().entities_tested, 0u) << "BVH 路径没被触发";

  int hits = 0;
  int mismatch = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].hit) ++hits;
    if (a[i].hit != b[i].hit) {
      ++mismatch;
      continue;
    }
    if (!a[i].hit) continue;
    if (a[i].entity != b[i].entity) {
      ++mismatch;
      continue;
    }
    if (std::fabs(a[i].distance - b[i].distance) > 1e-3f) ++mismatch;
  }
  EXPECT_GT(hits, 0) << "整屏都没命中，测试无意义";
  EXPECT_EQ(mismatch, 0) << "BVH 与线性扫描结果不一致（逐像素共 " << a.size() << " 个采样点）";
}

TEST(PickerBvh, ClosestObjectWinsRegardlessOfInsertionOrder) {
  // 同样的盒子按正序、逆序各插一遍，命中结果必须一致。
  // 这条能抓出"BVH 依赖插入顺序"这类错误 —— 建树用了 nth_element，
  // 如果比较函数不满足严格弱序（比如平局处理不当），顺序会渗透进结果。
  Scene fwd = make_grid_scene(5);  // 25 个 → BVH
  Scene rev;
  for (auto it = fwd.boxes.rbegin(); it != fwd.boxes.rend(); ++it) rev.boxes.push_back(*it);
  for (auto it = fwd.entities.rbegin(); it != fwd.entities.rend(); ++it)
    rev.entities.push_back(*it);

  auto cam = make_camera(14.0f, 120, 80);
  auto p1 = picker_with(fwd);
  auto p2 = picker_with(rev);

  const std::vector<Sample> a = sweep(*p1, *cam, 120, 80, 3);
  const std::vector<Sample> b = sweep(*p2, *cam, 120, 80, 3);
  ASSERT_EQ(a.size(), b.size());

  int diff_entity = 0;
  int diff_distance = 0;
  int hits = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!a[i].hit && !b[i].hit) continue;
    ++hits;
    if (a[i].hit != b[i].hit || a[i].entity != b[i].entity) {
      ++diff_entity;
    } else if (std::fabs(a[i].distance - b[i].distance) > 1e-3f) {
      ++diff_distance;
    }
  }
  EXPECT_GT(hits, 0) << "整屏都没命中，测试无意义";
  EXPECT_EQ(diff_entity, 0) << "插入顺序改变了命中的实体";
  EXPECT_EQ(diff_distance, 0) << "插入顺序改变了命中距离";
}

TEST(PickerBvh, FailedPickLeavesHitInvalid) {
  // PickHit 的默认 distance 必须是 -1 而不是 0。
  // 若是 0，`valid()`（distance >= 0）对**从未被写过**的对象返回 true，
  // 调用方就会把"没点到"当成"点到了 distance=0 的东西"。
  // 这种 bug 不崩不报错，只在"点空白处却选中了原点物体"时显形。
  auto p = mbp::create_picker();
  p->begin_scene();
  auto cam = make_camera(14.0f, 64, 48);
  mbp::PickHit hit;
  EXPECT_FALSE(hit.valid()) << "默认构造的 PickHit 必须无效";
  EXPECT_FALSE(p->pick_scene(*cam, 32.0f, 24.0f, &hit));
  EXPECT_FALSE(hit.valid()) << "拾取失败后 hit 必须仍然无效";
}

TEST(PickerBvh, BeginSceneResetsGeometry) {
  // 每帧重建是 picker 的契约（场景变化频繁，增量维护更贵）。
  // 这条验证 begin_scene 之后旧对象真的没了 —— 不是靠 stats 归零
  // 来"推断"（那是循环论证：空场景当然不测试任何实体），
  // 而是拾取结果本身必须变化。
  auto cam = make_camera(14.0f, 80, 60);
  Scene s = make_grid_scene(4);  // 16 个 → BVH
  auto p = picker_with(s);

  // 不假设"屏幕中心正好命中" —— 网格布局下中心常常落在格子间隙里。
  // 扫一遍找一个确实命中的像素，再验证 begin_scene 之后它不再命中。
  mbp::PickHit before;
  bool hit_before = false;
  float hit_px = 0.0f;
  float hit_py = 0.0f;
  for (int py = 0; py < 60 && !hit_before; ++py) {
    for (int px = 0; px < 80; ++px) {
      if (p->pick_scene(*cam, static_cast<float>(px), static_cast<float>(py), &before)) {
        hit_before = true;
        hit_px = static_cast<float>(px);
        hit_py = static_cast<float>(py);
        break;
      }
    }
  }
  ASSERT_TRUE(hit_before) << "整屏都点不到任何盒子，测试无意义";

  p->begin_scene();  // 清空，但不再 add_object

  mbp::PickHit after;
  EXPECT_FALSE(p->pick_scene(*cam, hit_px, hit_py, &after))
      << "begin_scene 之后，(px=" << hit_px << ", py=" << hit_py
      << ") 不该还能点到上一帧的对象";
}

TEST(PickerBvh, StatsCountPruningWhenTreeIsUsed) {
  // 场景足够大时，最近优先剪枝应当真的发生（pruned > 0）。
  // 如果 BVH 的剪枝条件写错（比如忘了比较 t > best），
  // 结果仍然正确但一点没加速 —— 这类"正确但没用"的退化和漏检一样
  // 需要被发现。
  Scene s = make_grid_scene(7);  // 49 个
  auto cam = make_camera(14.0f, kWidth, kHeight);
  auto p = picker_with(s);
  sweep(*p, *cam, kWidth, kHeight, 2);

  const mbp::PickStats& st = p->stats();
  EXPECT_GT(st.entities_tested, 0u);
  EXPECT_GT(st.pruned, 0u) << "一次剪枝都没发生：BVH 退化成全量扫描";
}

TEST(PickerBvh, HandlesDegenerateCoincidentBoxes) {
  // 全部对象重合：划分轴长度为 0，nth_element 的比较函数必须
  // 在"键完全相等"时仍然给出严格弱序（这里用下标做平局判据）。
  // 否则 MSVC 的 nth_element 可能越界或死循环 —— 比较函数
  // 必须满足严格弱序是标准库的硬要求。
  Scene s;
  for (int i = 0; i < 16; ++i) {
    s.boxes.push_back(box_at(0.0f, 0.0f, 0.0f, 1.0f));  // 完全重合
    s.entities.push_back(static_cast<std::uint32_t>(i + 1));
  }
  auto cam = make_camera(10.0f, 80, 60);
  auto p = picker_with(s);

  // 同样不假设中心点命中，先扫出一个命中点。
  mbp::PickHit hit;
  bool found = false;
  for (int py = 0; py < 60 && !found; ++py) {
    for (int px = 0; px < 80; ++px) {
      if (p->pick_scene(*cam, static_cast<float>(px), static_cast<float>(py), &hit)) {
        found = true;
        break;
      }
    }
  }
  EXPECT_TRUE(found) << "重合盒子的投影应当点得到";
  if (found) {
    EXPECT_GT(hit.distance, 0.0f);
    EXPECT_GE(hit.entity, 1u);
  }
}

TEST(PickerBvh, LargeSceneSweepIsStableAcrossRepeatedPicks) {
  // 同一 picker 反复拾取，结果必须完全一致。
  // stats 是累积的，但命中结果不该受其影响 —— 这条能抓出
  // "把上一帧状态带进本次拾取"的错误。
  Scene s = make_grid_scene(6);  // 36 个
  auto cam = make_camera(14.0f, 100, 70);
  auto p = picker_with(s);

  const std::vector<Sample> first = sweep(*p, *cam, 100, 70, 5);
  const std::vector<Sample> second = sweep(*p, *cam, 100, 70, 5);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].hit, second[i].hit) << "第 " << i << " 个采样点命中与否变了";
    if (first[i].hit && second[i].hit) {
      EXPECT_EQ(first[i].entity, second[i].entity) << "第 " << i << " 个采样点命中实体变了";
      EXPECT_FLOAT_EQ(first[i].distance, second[i].distance);
    }
  }
}
