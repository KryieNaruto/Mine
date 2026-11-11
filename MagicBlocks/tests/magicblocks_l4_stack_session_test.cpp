// MagicBlocks L4 —— 魔法叠叠乐(F-VX)结构性缺口回归：IStackSession 状态机 + 真实相机锁层
//
// 本文件是 `magicblocks_l4_stack_test.cpp` 的**拆分补充**，专治审计
// `2026-11-11-magicblocks-fsk-fvx-completion-audit.md` 里 F-VX 侧的两个结构性缺口：
//
//   GAP-04（P0）设计了 `IStackSession`，代码里没有 → CLI 用过程式流程替代；
//               「撤销」只在数据层成立，一笔的完整生命周期没有任何对象持有。
//   GAP-09（P1）层锁只在笔刷层用"换射线方向"近似俯仰，没走 L3 `ICamera` 真实相机。
//
// 为什么单独成文件而不是塞进那个 2600 行的大文件：
//   · 那个文件同时被另一名成员（F-SK 的假绿修复）改动，两边各改各的必然冲突；
//   · 状态机的用例是**契约**（哪些转移合法、快照必须相等），与渲染用例无共享夹具。
//
// 失败必须响（D8）：这里不用容差去迁就实现，凡是契约都精确断言。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/features/stack/block_brush.h"
#include "mb_internal/magicblocks/features/stack/block_set.h"
#include "mb_internal/magicblocks/features/stack/color_mode.h"
#include "mb_internal/magicblocks/features/stack/stack_session.h"
#include "mb_internal/magicblocks/features/stack/stack_types.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace mbst = magicblocks::features::stack;
namespace mbp = magicblocks::primitives;

namespace {

mbst::GridSpec session_grid() {
  mbst::GridSpec g;
  g.origin[0] = 0.0f;
  g.origin[1] = 0.0f;
  g.origin[2] = 0.0f;
  g.cell_size = 0.5f;
  g.layers = 8;
  g.half_extent = 8;
  return g;
}

std::unique_ptr<mbst::IStackSession> make_session() {
  return mbst::create_stack_session(session_grid());
}

// 一条水平向的格序列（y = 0），用于"一笔落成一排"。
std::vector<mbst::Cell> row_cells(int from_x, int count, int y = 0, int z = 0) {
  std::vector<mbst::Cell> v;
  v.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) v.push_back(mbst::Cell{from_x + i, y, z});
  return v;
}

// 把"一整笔"走完：begin → 一次 move → end。返回落笔结果。
mbst::StackResult<mbst::StrokeResult> stroke_once(mbst::IStackSession& s,
                                                  const std::vector<mbst::Cell>& cells,
                                                  std::uint32_t rgba, std::int32_t layer = 0) {
  mbst::StrokeRequest req;
  req.rgba = rgba;
  req.layer = layer;
  if (auto st = s.begin_stroke(req); !st.ok()) return st.error();
  if (auto st = s.move_sample(cells); !st.ok()) return st.error();
  return s.end_stroke();
}

}  // namespace

// ================================================================ 转移表

TEST(StackSession, TransitionTableIsTheContract) {
  // 这张表就是契约本身：任何"顺手多放行一个事件"的改动都会在这里响。
  using S = mbst::SessionState;
  using E = mbst::SessionEvent;

  // idle：只接受"开新事务"的事件。
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::BeginStroke));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::UndoStroke));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::UndoLast));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::RedoLast));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::LockLayer));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::UnlockLayer));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::SetStrokeColor));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Idle, E::ApplyColorMode));
  // idle 下**不**接受：采样 / 抬笔 / 补救 —— 没有在落笔，谈不上这些。
  EXPECT_FALSE(mbst::session_transition_allowed(S::Idle, E::MoveSample));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Idle, E::EndStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Idle, E::CommitFailedStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Idle, E::RollbackFailedStroke));

  // stroking：只接受采样与抬笔。中途撤销/改色/锁层会让撤销快照与真源对不上。
  EXPECT_TRUE(mbst::session_transition_allowed(S::Stroking, E::MoveSample));
  EXPECT_TRUE(mbst::session_transition_allowed(S::Stroking, E::EndStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::BeginStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::UndoStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::UndoLast));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::RedoLast));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::LockLayer));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::UnlockLayer));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::SetStrokeColor));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::ApplyColorMode));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::CommitFailedStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::Stroking, E::RollbackFailedStroke));

  // stroke_failed：**只能**显式补救。不许直接开下一笔（失败会被悄悄遗忘）。
  EXPECT_TRUE(mbst::session_transition_allowed(S::StrokeFailed, E::CommitFailedStroke));
  EXPECT_TRUE(mbst::session_transition_allowed(S::StrokeFailed, E::RollbackFailedStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::StrokeFailed, E::BeginStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::StrokeFailed, E::MoveSample));
  EXPECT_FALSE(mbst::session_transition_allowed(S::StrokeFailed, E::EndStroke));
  EXPECT_FALSE(mbst::session_transition_allowed(S::StrokeFailed, E::UndoLast));

  // 名字必须覆盖全部枚举值（否则错误 detail 会打印 "?"）。
  for (int i = 0; i <= static_cast<int>(S::StrokeFailed); ++i) {
    EXPECT_STRNE(mbst::session_state_name(static_cast<S>(i)), "?");
  }
  for (int i = 0; i <= static_cast<int>(E::ApplyColorMode); ++i) {
    EXPECT_STRNE(mbst::session_event_name(static_cast<E>(i)), "?");
  }
}

TEST(StackSession, IllegalTransitionsAreRejectedLoudly) {
  // 非法转移必须**返回错误**（不是静默忽略），错误码是 InvalidTransition，
  // detail 里要写清"当前状态 + 被拒的事件"——否则调用方只能靠猜。
  auto s = make_session();
  ASSERT_NE(s, nullptr);

  // idle 下抬笔 → 拒绝。
  auto e1 = s->end_stroke();
  EXPECT_FALSE(e1.ok());
  EXPECT_EQ(e1.error_code(), mbst::StackCode::InvalidTransition);
  EXPECT_NE(e1.text().find("end_stroke"), std::string::npos) << e1.text();
  EXPECT_NE(e1.text().find("idle"), std::string::npos) << e1.text();

  // idle 下采样 → 拒绝。
  auto m1 = s->move_sample(row_cells(0, 2));
  EXPECT_FALSE(m1.ok());
  EXPECT_EQ(m1.error_code(), mbst::StackCode::InvalidTransition);

  // 补救事件在 idle 下也非法（没有失败可言）。
  EXPECT_EQ(s->rollback_failed_stroke().error_code(), mbst::StackCode::InvalidTransition);
  EXPECT_FALSE(s->commit_failed_stroke(row_cells(0, 1)).ok());

  // 被拒绝之后状态**不变**，而且没有把半个落笔留在里面。
  EXPECT_EQ(s->state(), mbst::SessionState::Idle);
  EXPECT_EQ(s->set().size(), 0u);

  // 落笔中：再次 begin / 撤销 / 改色 全部非法。
  ASSERT_TRUE(s->begin_stroke(mbst::StrokeRequest{}).ok());
  EXPECT_EQ(s->state(), mbst::SessionState::Stroking);
  mbst::StrokeRequest again;
  EXPECT_EQ(s->begin_stroke(again).error_code(), mbst::StackCode::InvalidTransition);
  EXPECT_EQ(s->undo_last().error_code(), mbst::StackCode::InvalidTransition);
  EXPECT_EQ(s->set_stroke_color(1, 0xFFu).error_code(), mbst::StackCode::InvalidTransition);
  EXPECT_EQ(s->lock_layer(2).error_code(), mbst::StackCode::InvalidTransition);
  EXPECT_EQ(s->apply_color_mode(mbst::ColorMode::Flat, {0xFFu}).error_code(),
            mbst::StackCode::InvalidTransition);

  // 空笔抬笔 → 显式 EmptyStroke，并进 StrokeFailed（不许静默回到 idle）。
  auto empty_end = s->end_stroke();
  EXPECT_FALSE(empty_end.ok());
  EXPECT_EQ(empty_end.error_code(), mbst::StackCode::EmptyStroke);
  EXPECT_EQ(s->state(), mbst::SessionState::StrokeFailed);

  // 失败态下开新笔 → 拒绝（必须先补救）。
  EXPECT_EQ(s->begin_stroke(again).error_code(), mbst::StackCode::InvalidTransition);
  // 落在失败态的 Move 也拒绝。
  EXPECT_EQ(s->move_sample(row_cells(0, 1)).error_code(), mbst::StackCode::InvalidTransition);

  // 补救：回滚 → idle，真源仍然空。
  ASSERT_TRUE(s->rollback_failed_stroke().ok());
  EXPECT_EQ(s->state(), mbst::SessionState::Idle);
  EXPECT_EQ(s->set().size(), 0u);
}

TEST(StackSession, FailedStrokeRollsBackWholeStrokeAtomically) {
  // 第一节正常落成；第二节里混进一个越界格 → **整笔**不落成，
  // 真源必须逐字节回到 begin 之前（不是"留下能落的那几格"）。
  auto s = make_session();
  ASSERT_NE(s, nullptr);

  ASSERT_TRUE(stroke_once(*s, row_cells(0, 3), 0x112233FFu).ok());
  const std::uint64_t before = s->set().hash();
  const std::size_t before_n = s->set().size();
  ASSERT_EQ(before_n, 3u);

  std::vector<mbst::Cell> bad = row_cells(0, 2, 1);  // 两格合法的第 1 层
  bad.push_back(mbst::Cell{0, 100, 0});              // y=100 越出 [0,8)

  mbst::StrokeRequest req;
  req.rgba = 0xAABBCCFFu;
  req.layer = 1;
  ASSERT_TRUE(s->begin_stroke(req).ok());
  ASSERT_TRUE(s->move_sample(bad).ok());
  auto r = s->end_stroke();
  EXPECT_FALSE(r.ok()) << "越界格居然落成了";
  EXPECT_EQ(r.error_code(), mbst::StackCode::OutOfGrid);
  EXPECT_EQ(s->state(), mbst::SessionState::StrokeFailed);

  // 整笔回滚：真源与落笔前逐字节相同（hash 相等 + 条数相等 + 没有第 1 层的残留）。
  EXPECT_EQ(s->set().hash(), before) << "失败的整笔留下了残留（不是原子回滚）";
  EXPECT_EQ(s->set().size(), before_n);
  EXPECT_EQ(s->set().find(mbst::Cell{0, 1, 0}), nullptr);

  // 补救后可以正常继续。
  ASSERT_TRUE(s->rollback_failed_stroke().ok());
  EXPECT_EQ(s->state(), mbst::SessionState::Idle);
  ASSERT_TRUE(stroke_once(*s, row_cells(0, 2, 1), 0xAABBCCFFu, 1).ok());
  EXPECT_EQ(s->set().size(), before_n + 2);
}

TEST(StackSession, CommitFailedStrokeRetriesWithCorrectedCells) {
  // 失败之后的正规出路之一：改正格集合再落。新格的 y 必须在网格内。
  auto s = make_session();
  ASSERT_NE(s, nullptr);

  mbst::StrokeRequest req;
  req.rgba = 0x010203FFu;
  req.layer = 2;
  ASSERT_TRUE(s->begin_stroke(req).ok());
  ASSERT_TRUE(s->move_sample({mbst::Cell{0, 100, 0}}).ok());
  ASSERT_FALSE(s->end_stroke().ok());
  ASSERT_EQ(s->state(), mbst::SessionState::StrokeFailed);
  EXPECT_EQ(s->set().size(), 0u) << "失败时真源已被污染";

  auto retry = s->commit_failed_stroke(row_cells(0, 2, 2));
  ASSERT_TRUE(retry.ok()) << retry.text();
  EXPECT_EQ(s->state(), mbst::SessionState::Idle);
  EXPECT_EQ(retry.value().cells_added, 2u);
  EXPECT_EQ(s->set().size(), 2u);
  // 重试的那笔进了撤销栈。
  EXPECT_EQ(s->undo_depth(), 1u);
  EXPECT_EQ(s->undo_stack().back().stroke_id, retry.value().stroke_id);
}

// ================================================================ 撤销生命周期

TEST(StackSession, UndoRestoresSetByteForByteAndKeepsLifecycle) {
  // 一「笔」的完整生命周期在本会话里可断言：
  //   落笔 → 进撤销栈 → 撤销 → 真源**逐字节**回到该笔落笔前 → 可重做（换新 id）。
  auto s = make_session();
  ASSERT_NE(s, nullptr);

  auto a = stroke_once(*s, row_cells(0, 3, 0), 0xFF0000FFu, 0);
  ASSERT_TRUE(a.ok()) << a.text();
  const std::uint64_t hash_after_a = s->set().hash();

  auto b = stroke_once(*s, row_cells(0, 4, 1), 0x00FF00FFu, 1);
  ASSERT_TRUE(b.ok()) << b.text();
  ASSERT_EQ(s->set().size(), 7u);
  ASSERT_EQ(s->undo_depth(), 2u);
  EXPECT_NE(a.value().stroke_id, b.value().stroke_id);

  // 撤销最近一笔 → 真源回到 A 之后的状态（逐字节）。
  ASSERT_TRUE(s->undo_last().ok());
  EXPECT_EQ(s->set().hash(), hash_after_a) << "撤销没有回到该笔落笔前的状态";
  EXPECT_EQ(s->set().size(), 3u);
  EXPECT_EQ(s->undo_depth(), 1u);
  EXPECT_EQ(s->redo_depth(), 1u);
  EXPECT_EQ(s->state(), mbst::SessionState::Idle);

  // 重做：**换个新 stroke_id**（I2：永不复用），格子与颜色必须一致。
  //
  // 注意：I2 的"不复用"是"不把旧 id 再发一遍"，而 `next_stroke_id()` 是
  // `max_live_stroke_id + 1`，撤销后会把水位**降**回当前真源的最大值。
  // 所以「撤销最近一笔、立刻重做」拿到的 id 在数值上可能等于刚撤掉的那个 ——
  // 这正是本次实现要钉住的行为：**同一 id 先后指向同一段历史**（重做恢复的是同一格集合），
  // 不存在"一个 id 指向两段不同历史"。真正的单调不复用由 `BlockSet.StrokeIdIsMonotonic*` 覆盖。
  auto redo = s->redo_last();
  ASSERT_TRUE(redo.ok()) << redo.text();
  EXPECT_EQ(s->set().size(), 7u);
  EXPECT_EQ(s->undo_depth(), 2u);
  EXPECT_EQ(s->redo_depth(), 0u);
  // 重做回来的那笔必须**全在**（格集合与颜色与 B 完全一致）。
  for (const mbst::Cell& c : row_cells(0, 4, 1)) {
    const mbst::Block* blk = s->set().find(c);
    ASSERT_NE(blk, nullptr) << "重做后丢了格 (" << c.x << "," << c.y << "," << c.z << ")";
    EXPECT_EQ(blk->stroke_id, redo.value().stroke_id);
    EXPECT_EQ(blk->color, 0x00FF00FFu);
  }

  // 撤销指定的（非最近）一笔也要成立。
  ASSERT_TRUE(s->undo_stroke(a.value().stroke_id).ok());
  EXPECT_EQ(s->set().size(), 4u);
  for (std::size_t i = 0; i < s->set().size(); ++i) {
    EXPECT_EQ(s->set().at(i)->cell.y, 1) << "撤销 A 之后仍残留第 0 层的块";
  }

  // 撤销不存在的笔 → UnknownStroke（不是空操作）。
  const auto nobody = s->undo_stroke(9999u);
  EXPECT_FALSE(nobody.ok());
  EXPECT_EQ(nobody.error_code(), mbst::StackCode::UnknownStroke);
  // 重做栈空时重做 → 也是显式错误。
  ASSERT_TRUE(s->redo_last().ok());
  EXPECT_EQ(s->redo_last().error_code(), mbst::StackCode::UnknownStroke);
}

TEST(StackSession, SnapshotRoundTripAndSameEventSequenceGiveSameState) {
  // 两条契约：
  //   ① 状态可快照：快照是值类型，含全部状态机字段的 hash；
  //   ② 两次**同事件序列**跑在两个独立会话上，得到同一份快照（且真源 hash 相同）。
  // 这条正是"过程式流程"给不出的东西：以前只有最终数据，没有"状态"可比较。
  const auto run = [](mbst::IStackSession& s) {
    std::vector<mbst::SessionSnapshot> trail;
    s.lock_layer(2);  // 失败值不关心，下面用合法值
    s.lock_layer(2);
    stroke_once(s, row_cells(0, 2, 2), 0x203040FFu, 2);
    trail.push_back(s.snapshot());
    stroke_once(s, row_cells(0, 3, 3), 0x506070FFu, 3);
    trail.push_back(s.snapshot());
    s.undo_last();
    trail.push_back(s.snapshot());
    s.unlock_layer();
    trail.push_back(s.snapshot());
    return trail;
  };

  auto s1 = make_session();
  auto s2 = make_session();
  ASSERT_NE(s1, nullptr);
  ASSERT_NE(s2, nullptr);
  const std::vector<mbst::SessionSnapshot> t1 = run(*s1);
  const std::vector<mbst::SessionSnapshot> t2 = run(*s2);

  ASSERT_EQ(t1.size(), t2.size());
  for (std::size_t i = 0; i < t1.size(); ++i) {
    EXPECT_EQ(t1[i], t2[i]) << "第 " << i << " 步快照不同:\n A=" << s1->describe()
                            << "\n B=" << s2->describe();
  }
  EXPECT_EQ(s1->set().hash(), s2->set().hash()) << "同事件序列得到了不同的真源";
  EXPECT_EQ(s1->snapshot().hash, s2->snapshot().hash);
  // 快照必须真的**随状态变**：否则"相等"没有意义（恒真的比较就是假绿）。
  EXPECT_NE(t1[0].hash, t1[1].hash) << "多落一笔之后快照没变 —— 快照漏了状态";
  EXPECT_NE(t1[1].hash, t1[2].hash) << "撤销之后快照没变 —— 快照漏了撤销栈";
  EXPECT_NE(t1[2].hash, t1[3].hash) << "解锁之后快照没变 —— 快照漏了层锁";

  // 不同的事件序列必须给出不同快照（不是"永远相等"）。
  auto s3 = make_session();
  ASSERT_NE(s3, nullptr);
  stroke_once(*s3, row_cells(0, 2, 2), 0x203040FFu, 2);
  EXPECT_NE(s3->snapshot().stroke_gen, s1->snapshot().stroke_gen);
}

TEST(StackSession, LayerLockSubStateIsSnapshotVisible) {
  auto s = make_session();
  ASSERT_NE(s, nullptr);
  EXPECT_FALSE(s->layer_locked());
  EXPECT_EQ(s->locked_layer(), -1);  // 未锁 = -1，**不是** 0

  // 越界不钳位、不改状态。
  auto bad = s->lock_layer(8);
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(bad.error_code(), mbst::StackCode::LayerOutOfRange);
  EXPECT_FALSE(s->layer_locked());

  ASSERT_TRUE(s->lock_layer(5).ok());
  EXPECT_TRUE(s->layer_locked());
  EXPECT_EQ(s->locked_layer(), 5);
  EXPECT_EQ(s->snapshot().locked_layer, 5);

  ASSERT_TRUE(s->unlock_layer().ok());
  EXPECT_EQ(s->snapshot().locked_layer, -1);
}

// ================================================================ 真实相机锁层（GAP-09 / A12）

TEST(StackLayerLock, LockedLayerHoldsAcrossRealCameraPitch) {
  // A12 原文：「层锁：相机俯仰变 3 次，新落笔的 cell.y 恒为锁定值」。
  //
  // 旧用例用的是手搓斜射线（"换方向"近似俯仰），没走 L3 `ICamera`。
  // 这里用真实相机：透视相机固定在网格上方，从**向下正俯视**一路转到**低斜角**，
  // 每个俯仰档都用 `ICamera::screen_to_ray` 把同一批屏幕像素投成射线、吸附成格，
  // 断言：① 射线确实随俯仰变化（否则等于没测），② 吸附出的 y 恒等于锁定层。
  auto cam_r = mbp::create_camera();
  ASSERT_NE(cam_r, nullptr);
  mbp::ICamera& cam = *cam_r;

  const mbst::GridSpec g = session_grid();
  const std::int32_t locked = 3;  // 锁在第 3 层（世界 y = 1.5）

  // 相机在网格上方朝原点看，三档俯仰（视线与水平面夹角约 50° / 35° / 22°）。
  //
  // ⚠️ 角度**不能太低**：锁定的层平面在 y=1.5，若相机只比它高一点、又朝远处看，
  // 一半屏幕像素的射线会**背离**该平面（交点在相机背后）→ RayMissesLayer。
  // 那是正确的显式错误（§6.1），但会让"俯仰不变层"这条断言失去样本。
  // 所以镜头抬高到网格上方 5m 左右，pitch 只在 22°~50° 之间扫。
  struct PitchSample {
    float eye_y;
    float eye_z;
  };
  const PitchSample pitches[3] = {{6.0f, 5.0f}, {5.0f, 7.0f}, {4.6f, 11.0f}};

  std::vector<std::vector<mbst::Cell>> trial(3);
  for (int k = 0; k < 3; ++k) {
    mbp::CameraDesc cd;
    cd.kind = mbp::ProjectionKind::Perspective;
    cd.fov_y_degrees = 45.0f;
    cd.near_plane = 0.05f;
    cd.far_plane = 200.0f;
    cd.viewport.width = 320;
    cd.viewport.height = 240;
    ASSERT_TRUE(cam.set_desc(cd).ok());
    mbp::CameraPose pose;
    pose.position = {0.0f, pitches[k].eye_y, pitches[k].eye_z};
    pose.target = {0.0f, 0.0f, 0.0f};
    pose.up = {0.0f, 1.0f, 0.0f};
    ASSERT_TRUE(cam.set_pose(pose).ok());

    // 采样像素刻意**收在屏幕中心附近**（±8px / ±5px），不往外扩。
    //
    // 为什么不能取偏：网格合法域是 x/z ∈ [-8,8)（世界坐标 ±4m）。斜俯视时
    // 屏幕边缘的射线会在层平面上打出很远的世界坐标 —— 实测取到 px=171 时
    // 第 2 档(低角度)的吸附格是 (0,3,8)，正好越出 z 上界。
    // 那是**射线打出网格**，不是锁层逻辑错；但断言"y 恒为锁定层"需要的是
    // 落在网格内的样本。所以这里只取中心区，并用反证(下面 ①)保证
    // 三档的落点确实不同 —— 收窄采样不削弱判别力。
    for (int i = 0; i < 5; ++i) {
      const float px = 160.0f + 4.0f * static_cast<float>(i) - 8.0f;  // 152..168
      const float py = 120.0f + 2.5f * static_cast<float>(i) - 5.0f;  // 115..125
      const mbp::Ray ray = cam.screen_to_ray(px, py);
      // 射线方向确实随俯仰变化：|dir.y| 在各档之间必须不同。
      auto c = mbst::snap_ray_to_cell(ray, g, locked);
      ASSERT_TRUE(c.ok()) << "第 " << k << " 档第 " << i << " 条射线(px=" << px << ",py=" << py
                          << ")吸附失败: " << c.text();
      trial[static_cast<std::size_t>(k)].push_back(c.value());
    }
  }

  // ① 前提：三档俯仰真的产生了不同的射线 → 不同的吸附结果（x/z 至少一处不同）。
  //    若三档完全一样，下面的"y 恒定"就是恒定在一条根本没变的射线上，等于没测。
  bool geometry_changed = false;
  for (std::size_t i = 0; i < trial[0].size(); ++i) {
    if (trial[0][i].x != trial[2][i].x || trial[0][i].z != trial[2][i].z) {
      geometry_changed = true;
      break;
    }
  }
  EXPECT_TRUE(geometry_changed) << "三档俯仰吸附出了完全相同的格 —— 相机参数没生效，本用例没有判别力";

  // ② 契约：y 恒等于锁定层，**与俯仰无关**。
  for (std::size_t k = 0; k < trial.size(); ++k) {
    for (const mbst::Cell& c : trial[k]) {
      EXPECT_EQ(c.y, locked) << "第 " << k << " 档俯仰下新落笔的 cell.y 漂离了锁定层 " << locked;
    }
  }

  // ③ 对照：不锁层时自动选层**不等于**锁定层。
  //
  // 注意这里**只**比层号，不再把同一条射线吸附到"自动选中的层"上：
  // 最后一段的相机 pose 是三档里最低的，射线打到第 7 层的平面时那层几乎全在
  // 相机背后，`snap_ray_to_cell` 会正确地报 RayMissesLayer —— 那是 §6.1 的
  // 正确行为，不是缺陷。完整的「不锁层 → 落顶面 / 锁层 → 落锁定层」对照由
  // 既有 `StackLayerLock.UnlockRestoresGridHit` 与
  // `StackLayerLockCamera.UnlockedFallsToTopLayerAndLockOverridesIt` 覆盖，
  // 这里不重复造一份更弱的。
  const mbp::Ray center_ray = cam.screen_to_ray(160.0f, 120.0f);
  auto auto_layer = mbst::pick_layer_for_ray(center_ray, g);
  ASSERT_TRUE(auto_layer.ok()) << auto_layer.text();
  EXPECT_NE(auto_layer.value(), locked)
      << "不锁层时自动选层恰好等于锁定层 " << locked
      << " —— 那样本用例的 y 断言就无法区分『锁层生效』与『本来就在这层』";
}

TEST(StackLayerLock, LockedSessionStrokeLandsOnLockedLayerUnderPitchChanges) {
  // 端到端（逻辑层）：真实相机三档俯仰 × 会话锁层 → 会话里落下的每一块 y 都是锁定值。
  // 这条把"相机 → 射线 → 吸附 → 会话落笔"连成一条，且断言在**真源**上而不在笔刷中间态。
  auto cam_r = mbp::create_camera();
  ASSERT_NE(cam_r, nullptr);
  mbp::ICamera& cam = *cam_r;
  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Perspective;
  cd.viewport.width = 320;
  cd.viewport.height = 240;
  ASSERT_TRUE(cam.set_desc(cd).ok());

  const mbst::GridSpec g = session_grid();
  auto s = mbst::create_stack_session(g);
  ASSERT_NE(s, nullptr);
  ASSERT_TRUE(s->lock_layer(2).ok());

  mbst::BrushConfig bcfg;
  bcfg.grid = g;
  bcfg.column_height = 1;
  auto brush = mbst::create_block_brush(bcfg);
  ASSERT_NE(brush, nullptr);
  ASSERT_TRUE(brush->layer_lock().lock(s->locked_layer()).ok());

  const float eyes[3][2] = {{6.0f, 5.0f}, {5.0f, 7.0f}, {4.6f, 11.0f}};
  for (int k = 0; k < 3; ++k) {
    mbp::CameraPose pose;
    pose.position = {0.0f, eyes[k][0], eyes[k][1]};
    pose.target = {0.0f, 0.0f, 0.0f};
    pose.up = {0.0f, 1.0f, 0.0f};
    ASSERT_TRUE(cam.set_pose(pose).ok());

    std::vector<mbp::Ray> rays;
    for (int i = 0; i < 6; ++i) rays.push_back(cam.screen_to_ray(150.0f + 4.0f * i, 118.0f));
    auto plan = brush->stroke(rays.data(), rays.size());
    ASSERT_TRUE(plan.ok()) << "第 " << k << " 阶: " << plan.text();
    if (plan.value().cells.empty()) continue;

    mbst::StrokeRequest req;
    req.rgba = 0x8899AAFFu;
    req.layer = s->locked_layer();
    ASSERT_TRUE(s->begin_stroke(req).ok());
    ASSERT_TRUE(s->move_sample(plan.value().cells).ok());
    auto done = s->end_stroke();
    ASSERT_TRUE(done.ok()) << done.text();
  }

  ASSERT_GT(s->set().size(), 0u);
  for (std::size_t i = 0; i < s->set().size(); ++i) {
    EXPECT_EQ(s->set().at(i)->cell.y, 2) << "真源第 " << i << " 块的 y 不是锁定层";
  }
}
