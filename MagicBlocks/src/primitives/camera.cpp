// MagicBlocks SDK —— L3 `ICamera` 实现
//
// 全部矩阵/换算逻辑只在这里一份。约定(spec §2.3 / D-L3-2):
//   屏幕像素坐标原点左上、+y 向下;世界右手系 +y 向上;NDC 为 Vulkan 惯例 z∈[0,1]。
//
// 惰性求值:
//   - pose 脏 → view 脏 → view_proj 脏 → inverse 脏(脏标记链)。
//   - 任何 getter 只做一次"若脏则重算",稳态下 repeated call 是 O(1)。
#include <algorithm>
#include <cmath>

#include "magicblocks/primitives/camera.h"

namespace magicblocks::primitives {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
// pitch 夹紧到略小于 90°:让 up 与视线永不共线(否则 look_at 退化)。
constexpr float kMaxPitchDegrees = 89.9f;

// 位置是否"实际上没变"(用于 zoom 触界时避免无意义置脏)。
// 阈值取 1e-6:远小于任何可见位移,又大于 float 往返噪声。
bool equal_pose_pos(const Vec3& a, const Vec3& b) {
  const Vec3 d = a - b;
  return std::fabs(d.x) < 1e-6f && std::fabs(d.y) < 1e-6f && std::fabs(d.z) < 1e-6f;
}

Status validate_desc(const CameraDesc& d) {
  if (d.viewport.width == 0 || d.viewport.height == 0) {
    return Status::Invalid("CameraDesc: viewport 宽高必须 > 0");
  }
  if (!(d.near_plane > 0.0f)) {
    return Status::Invalid("CameraDesc: near_plane 必须 > 0");
  }
  if (!(d.far_plane > d.near_plane)) {
    return Status::Invalid("CameraDesc: far_plane 必须 > near_plane");
  }
  if (d.kind == ProjectionKind::Perspective) {
    if (!(d.fov_y_degrees > 0.0f) || !(d.fov_y_degrees < 179.0f)) {
      return Status::Invalid("CameraDesc: Perspective 的 fov_y_degrees 必须在 (0, 179)");
    }
  } else {
    if (!(d.ortho_height > 0.0f)) {
      return Status::Invalid("CameraDesc: Orthographic 的 ortho_height 必须 > 0");
    }
  }
  return Status::Ok_();
}

class Camera final : public ICamera {
 public:
  Camera() {
    pose_ = CameraPose{};
    desc_ = CameraDesc{};
    // 预热:构造即把矩阵算好并**清脏标记**。
    // 若清了标记不清脏,首次 getter 会再算一遍(意味着每台相机多算两次),
    // 惰性求值的"稳态 0 重算"断言 (MatrixRebuiltOnceUntilDirty) 就会失败。
    rebuild_all();
    desc_dirty_ = false;
    pose_dirty_ = false;
    vp_dirty_ = false;
  }

  // ---------------------------------------------------------- 配置
  const CameraDesc& desc() const override { return desc_; }
  const CameraPose& pose() const override { return pose_; }

  Status set_desc(const CameraDesc& desc) override {
    Status s = validate_desc(desc);
    if (!s.ok()) return s;
    if (desc.reverse_z && !reverse_z_supported_) {
      return Status::Unsupported("CameraDesc: reverse_z 需要设备能力(DeviceCaps.rev_z_supported)");
    }
    const bool same = desc.kind == desc_.kind && desc.fov_y_degrees == desc_.fov_y_degrees &&
                      desc.ortho_height == desc_.ortho_height &&
                      desc.near_plane == desc_.near_plane && desc.far_plane == desc_.far_plane &&
                      desc.reverse_z == desc_.reverse_z && desc.viewport == desc_.viewport;
    if (same) return Status::Ok_();   // 无变化不置脏(避免无意义重算)
    desc_ = desc;
    desc_dirty_ = true;
    return Status::Ok_();
  }

  Status set_pose(const CameraPose& pose) override {
    // 视线退化(position == target)时拒绝:否则 look_at 产出 NaN 矩阵。
    if (length(pose.target - pose.position) < 1e-6f) {
      return Status::Invalid("CameraPose: position 与 target 重合,视线退化");
    }
    if (length(pose.up) < 1e-6f) {
      return Status::Invalid("CameraPose: up 为零向量");
    }
    if (pose_ == pose) return Status::Ok_();
    pose_ = pose;
    pose_dirty_ = true;
    return Status::Ok_();
  }

  // 测试/玩法用:允许调用方声明"设备支持 reverse_z"(默认 false)。
  void set_reverse_z_supported(bool v) { reverse_z_supported_ = v; }

  // ---------------------------------------------------------- 矩阵
  const Mat4& view_matrix() const override {
    sync();
    return view_;
  }
  const Mat4& proj_matrix() const override {
    sync();
    return proj_;
  }
  const Mat4& view_proj_matrix() const override {
    sync();
    return view_proj_;
  }
  const Mat4& inverse_view_proj_matrix() const override {
    sync();
    return inv_vp_;
  }

  // ---------------------------------------------------------- 屏幕 ↔ 世界
  Ray screen_to_ray(float px, float py) const override {
    sync();
    // 像素 → NDC。像素原点左上、+y 向下;投影矩阵已把世界 +y 向上翻成
    // NDC +y 向下(见 make_perspective),所以两者同向,**不取反**:
    //   像素 y=0    → ndc_y=-1(屏幕顶)
    //   像素 y=h    → ndc_y=+1(屏幕底)
    const float w = static_cast<float>(desc_.viewport.width);
    const float h = static_cast<float>(desc_.viewport.height);
    const float ndc_x = (px / w) * 2.0f - 1.0f;
    const float ndc_y = (py / h) * 2.0f - 1.0f;

    Ray r;
    if (desc_.kind == ProjectionKind::Perspective) {
      // 由原点穿过近平面点,等价于穿过 far 平面点(方向相同,数值更稳)。
      const Vec4 near_p = inv_vp_ * Vec4{ndc_x, ndc_y, desc_.reverse_z ? 1.0f : 0.0f, 1.0f};
      const Vec4 far_p = inv_vp_ * Vec4{ndc_x, ndc_y, desc_.reverse_z ? 0.0f : 1.0f, 1.0f};
      const Vec3 a = {near_p.x / near_p.w, near_p.y / near_p.w, near_p.z / near_p.w};
      const Vec3 b = {far_p.x / far_p.w, far_p.y / far_p.w, far_p.z / far_p.w};
      r.origin = pose_.position;
      r.direction = normalize(b - a);
    } else {
      // 正交:近平面点即起点,方向恒为相机前向。
      const Vec4 near_p = inv_vp_ * Vec4{ndc_x, ndc_y, desc_.reverse_z ? 1.0f : 0.0f, 1.0f};
      r.origin = {near_p.x / near_p.w, near_p.y / near_p.w, near_p.z / near_p.w};
      r.direction = forward();
    }
    return r;
  }

  bool world_to_screen(const Vec3& p, float* px, float* py, float* depth) const override {
    sync();
    const Vec4 clip = view_proj_ * Vec4{p.x, p.y, p.z, 1.0f};
    if (clip.w <= 1e-6f) return false;  // 点在相机后方或恰在相机平面上
    const float inv_w = 1.0f / clip.w;
    const float ndc_x = clip.x * inv_w;
    const float ndc_y = clip.y * inv_w;
    const float w = static_cast<float>(desc_.viewport.width);
    const float h = static_cast<float>(desc_.viewport.height);
    if (px != nullptr) *px = (ndc_x * 0.5f + 0.5f) * w;
    // 投影矩阵已把"世界 +y 向上"翻成 Vulkan 的"NDC +y 向下"(见 make_perspective 注释),
    // 而像素坐标也是 +y 向下 —— 两个方向一致,**这里不再取反**。
    // 若这里再 (0.5f - ndc_y*0.5f),就等于翻转两次,画面会上下颠倒。
    if (py != nullptr) *py = (ndc_y * 0.5f + 0.5f) * h;
    if (depth != nullptr) *depth = clip.z * inv_w;        // [0,1]
    return true;
  }

  float world_size_per_pixel(const Vec3& world_pos) const override {
    if (desc_.kind == ProjectionKind::Orthographic) {
      return desc_.ortho_height / static_cast<float>(desc_.viewport.height);
    }
    sync();
    // 透视:视锥半高在距离 d 处 = d * tan(fov/2);每像素 = 2*d*tan(fov/2) / viewport H。
    // 距离取"相机到该世界点沿视线方向的投影",保证点在相机侧后方时也不会给负值。
    const float d = std::max(1e-4f, dot(world_pos - pose_.position, forward()));
    return (2.0f * d * std::tan(desc_.fov_y_degrees * 0.5f * kDegToRad)) /
           static_cast<float>(desc_.viewport.height);
  }

  float aspect() const override {
    return static_cast<float>(desc_.viewport.width) / static_cast<float>(desc_.viewport.height);
  }

  // ---------------------------------------------------------- 交互
  void orbit(float delta_yaw_degrees, float delta_pitch_degrees) override {
    const float dist = distance();
    if (dist < 1e-6f) return;
    // 球坐标:yaw 绕世界 up,pitch 自水平面起。
    const Vec3 v = pose_.position - pose_.target;
    float yaw = std::atan2(v.x, v.z);
    float pitch = std::atan2(v.y, std::sqrt(v.x * v.x + v.z * v.z));
    yaw += delta_yaw_degrees * kDegToRad;
    pitch += delta_pitch_degrees * kDegToRad;
    const float max_pitch = kMaxPitchDegrees * kDegToRad;
    pitch = std::clamp(pitch, -max_pitch, max_pitch);
    const float cp = std::cos(pitch);
    pose_.position = pose_.target + Vec3{dist * cp * std::sin(yaw), dist * std::sin(pitch),
                                         dist * cp * std::cos(yaw)};
    pose_dirty_ = true;
  }

  void pan(float delta_px_x, float delta_px_y) override {
    // 平移量按"target 处的世界尺度"换算(而不是相机处),手感才稳定。
    const float scale = world_size_per_pixel(pose_.target);
    const Vec3 r = right();
    const Vec3 u = normalize(cross(r, forward()));
    const Vec3 delta = r * (-delta_px_x * scale) + u * (delta_px_y * scale);
    pose_.position += delta;
    pose_.target += delta;
    pose_dirty_ = true;
  }

  void zoom(float delta) override {
    if (delta == 0.0f) return;
    // 缩放因子取 **指数对称** 形式:k = exp(delta)。
    //   放大 → delta>0 → k>1;缩小 → delta<0 → k<1;
    //   且 exp(-delta) == 1/exp(delta) → zoom(d) 与 zoom(-d) **严格互逆**,
    //   连续滚轮缩放也不会累积漂移。
    // 之前用 k = 1+delta 不互逆(1.1 的倒数是 0.909 而非 0.9),
    // 导致"推近再拉远"回不到原位 —— 这是被单元测试抓出来的真实缺陷。
    const float k = std::exp(delta);
    if (!(k > 0.0f) || !std::isfinite(k)) return;

    if (desc_.kind == ProjectionKind::Orthographic) {
      // 正交:delta > 0 放大 → ortho_height 变小(除以 k)。
      float h = desc_.ortho_height / k;
      h = std::max(h, desc_.near_plane * 4.0f);
      if (h == desc_.ortho_height) return;   // 已到夹紧边界
      desc_.ortho_height = h;
      desc_dirty_ = true;
      return;
    }
    // 透视:沿视线推拉,给上下限防止穿到目标背后或飞出去。
    const Vec3 dir = normalize(pose_.position - pose_.target);
    float dist = distance() / k;
    dist = std::clamp(dist, 0.05f, 1.0e5f);
    const Vec3 new_pos = pose_.target + dir * dist;
    if (equal_pose_pos(new_pos, pose_.position)) return;   // 已到夹紧边界,不置脏
    pose_.position = new_pos;
    pose_dirty_ = true;
  }

  void focus_on(const Vec3& center, float radius) override {
    const float r = std::max(radius, 1e-3f);
    pose_.target = center;
    if (desc_.kind == ProjectionKind::Perspective) {
      const float half_fov = desc_.fov_y_degrees * 0.5f * kDegToRad;
      const float half_fov_x = std::atan(std::tan(half_fov) * aspect());
      const float need = r / std::sin(std::min(half_fov, half_fov_x));
      const Vec3 dir = normalize(pose_.position - center);
      const Vec3 d = (length(dir) < 1e-6f) ? Vec3{0.0f, 0.0f, 1.0f} : dir;
      pose_.position = center + d * std::max(need, desc_.near_plane * 2.0f);
    } else {
      desc_.ortho_height = r * 2.2f;
      desc_dirty_ = true;
    }
    pose_dirty_ = true;
  }

  float distance() const override { return length(pose_.position - pose_.target); }
  Vec3 forward() const override { return normalize(pose_.target - pose_.position); }
  Vec3 right() const override {
    const Vec3 f = forward();
    Vec3 r = cross(f, pose_.up);
    if (length(r) < 1e-6f) r = Vec3{1.0f, 0.0f, 0.0f};
    return normalize(r);
  }

  // ---------------------------------------------------------- 快照
  CameraSnapshot snapshot() const override {
    CameraSnapshot s;
    s.desc = desc_;
    s.pose = pose_;
    return s;
  }
  Status restore(const CameraSnapshot& snap) override {
    Status s = set_desc(snap.desc);
    if (!s.ok()) return s;
    return set_pose(snap.pose);
  }

  u64 matrix_rebuild_count() const override { return rebuilds_; }

 private:
  void sync() const {
    if (desc_dirty_) {
      recompute_proj();
      desc_dirty_ = false;
      vp_dirty_ = true;
      rebuilds_ = rebuilds_ + 1;
    }
    if (pose_dirty_) {
      recompute_view();
      pose_dirty_ = false;
      vp_dirty_ = true;
      rebuilds_ = rebuilds_ + 1;
    }
    if (vp_dirty_) {
      compute_view_proj();
      vp_dirty_ = false;
    }
  }

  void rebuild_all() {
    recompute_proj();
    recompute_view();
    compute_view_proj();
  }

  // 惰性求值:getter 是 const,所以重算函数也必须是 const(只写 mutable 缓存)。
  void recompute_view() const {
    view_ = make_look_at(pose_.position, pose_.target, pose_.up);
  }

  void recompute_proj() const {
    const float w = static_cast<float>(desc_.viewport.width);
    const float h = static_cast<float>(desc_.viewport.height);
    if (desc_.kind == ProjectionKind::Perspective) {
      proj_ = make_perspective(desc_.fov_y_degrees * kDegToRad, w / h, desc_.near_plane,
                               desc_.far_plane, desc_.reverse_z);
    } else {
      const float half_h = desc_.ortho_height * 0.5f;
      const float half_w = half_h * (w / h);
      proj_ = make_orthographic(-half_w, half_w, -half_h, half_h, desc_.near_plane,
                                desc_.far_plane, desc_.reverse_z);
    }
  }

  void compute_view_proj() const {
    view_proj_ = proj_ * view_;
    bool ok = true;
    inv_vp_ = view_proj_.inverse(&ok);
    // 投影/视图矩阵参数合法时不会奇异;万一奇异就用单位阵(不产生 NaN 传播)。
    if (!ok) inv_vp_ = Mat4::identity();
  }

  CameraDesc desc_{};
  CameraPose pose_{};
  bool reverse_z_supported_ = false;

  mutable bool desc_dirty_ = true;
  mutable bool pose_dirty_ = true;
  mutable bool vp_dirty_ = true;
  mutable Mat4 view_{};
  mutable Mat4 proj_{};
  mutable Mat4 view_proj_{};
  mutable Mat4 inv_vp_{};
  mutable u64 rebuilds_ = 0;
};

}  // namespace

bool CameraSnapshot::operator==(const CameraSnapshot& o) const {
  if (!(pose == o.pose)) return false;
  const CameraDesc& a = desc;
  const CameraDesc& b = o.desc;
  return a.kind == b.kind && a.fov_y_degrees == b.fov_y_degrees &&
         a.ortho_height == b.ortho_height && a.near_plane == b.near_plane &&
         a.far_plane == b.far_plane && a.reverse_z == b.reverse_z &&
         a.viewport == b.viewport;
}

std::unique_ptr<ICamera> create_camera() { return std::make_unique<Camera>(); }

// 非公共 API(只在 L3 内部/测试用):声明 reverse_z 能力。
// 放在这里而不是接口里,是为了不让"设备能力"污染相机语义。
void camera_set_reverse_z_supported(ICamera* camera, bool supported) {
  auto* c = dynamic_cast<Camera*>(camera);
  if (c != nullptr) c->set_reverse_z_supported(supported);
}

}  // namespace magicblocks::primitives
