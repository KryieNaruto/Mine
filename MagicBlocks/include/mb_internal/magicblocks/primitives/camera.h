// MagicBlocks SDK —— L3 `ICamera`:基础摄像机(spec §5,用户要求 U1)
//
// 职责:**世界 ↔ 屏幕的全部换算只在这里**。
//
// 设计要点(spec §5.2 的 D-L3-1..D-L3-4):
//   - 矩阵用 POD `Mat4`(公共头零第三方依赖,可直接进命令流);
//   - **屏幕坐标原点左上**(与 device::InputEvent::position 一致,D-L3-2);
//   - `world_size_per_pixel()` 是"屏幕恒定尺寸"的唯一计算点 ——
//     gizmo 的恒定大小、屏幕恒宽线全部调它,不允许别处再推导一遍(用户要求 U6 的解耦前提);
//   - 矩阵惰性求值 + 脏标记:一帧内多次读取只算一次;
//   - `inverse_view_proj()` 缓存:screen_to_ray 不每次求逆。
//
// 性能:所有 getter 在稳态下一次调用只做一次比较(脏检查),不分配内存。
#pragma once

#include <memory>

#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::primitives {

enum class ProjectionKind : std::uint8_t {
  Perspective = 0,
  Orthographic = 1,
};

// 相机参数(值语义,可直接序列化进命令流)。
struct CameraDesc {
  ProjectionKind kind = ProjectionKind::Perspective;
  float fov_y_degrees = 45.0f;    // 仅透视:垂直视角
  float ortho_height = 2.0f;      // 仅正交:视口覆盖的世界高度
  float near_plane = 0.05f;
  float far_plane = 100.0f;
  bool reverse_z = false;         // 需要设备支持;不支持时 set_desc 返回 Unsupported
  Extent2D viewport{1, 1};        // 像素尺寸(必须 > 0)
  // 近/远平面校验失败时就地返回的明确错误,不做静默修正。
};

// 相机位姿(值语义)。
struct CameraPose {
  Vec3 position{0.0f, 2.0f, 5.0f};
  Vec3 target{0.0f, 0.0f, 0.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};

  bool operator==(const CameraPose& o) const {
    return position == o.position && target == o.target && up == o.up;
  }
  bool operator!=(const CameraPose& o) const { return !(*this == o); }
};

// 相机快照(命令流用):desc + pose,全部值类型。
struct CameraSnapshot {
  CameraDesc desc{};
  CameraPose pose{};
  bool operator==(const CameraSnapshot& o) const;
  bool operator!=(const CameraSnapshot& o) const { return !(*this == o); }
};

// 四个玩法都用的基础摄像机。**非线程安全**(G5 单线程)。
class ICamera {
 public:
  ICamera(const ICamera&) = delete;
  ICamera& operator=(const ICamera&) = delete;
  virtual ~ICamera() = default;

  // ------------------------------------------------------------ 配置

  virtual const CameraDesc& desc() const = 0;
  virtual const CameraPose& pose() const = 0;

  // 参数校验失败 → InvalidArgument(近/远平面、fov、视口);reverse_z 不支持 → Unsupported。
  virtual Status set_desc(const CameraDesc& desc) = 0;
  virtual Status set_pose(const CameraPose& pose) = 0;

  // ------------------------------------------------------------ 矩阵(惰性求值)

  virtual const Mat4& view_matrix() const = 0;
  virtual const Mat4& proj_matrix() const = 0;
  virtual const Mat4& view_proj_matrix() const = 0;
  virtual const Mat4& inverse_view_proj_matrix() const = 0;

  // ------------------------------------------------------------ 屏幕 ↔ 世界

  // 像素坐标(原点左上)→ 世界射线。像素越界**不**拒绝:UI 常常把手势拖到画布外,
  // 这时仍需要一个合理射线(begin_drag 之后持续手拖的常见情形)。
  virtual Ray screen_to_ray(float px, float py) const = 0;

  // 世界点 → 像素坐标(原点左上)+ 标准化深度。点在相机后方时返回 false 且不写 out。
  virtual bool world_to_screen(const Vec3& p, float* px, float* py, float* depth) const = 0;

  // **屏幕恒定尺寸的唯一原语**:给定世界点,返回"一个像素在该点处对应的世界长度"。
  //   透视:随深度线性增长;正交:常量(= ortho_height / viewport.height)。
  // gizmo 恒定尺寸、屏幕恒宽虚线都只用它,不在别处重复推导。
  virtual float world_size_per_pixel(const Vec3& world_pos) const = 0;

  // 视口像素宽高(aspect = w/h)。
  virtual float aspect() const = 0;

  // ------------------------------------------------------------ 交互(值语义,不读时钟)

  // 绕 target 旋转(yaw 绕世界 up,pitch 绕相机右轴)。pitch 会被夹到 ±89.9°,
  // 防止 up 与视线共线导致视图矩阵退化。
  virtual void orbit(float delta_yaw_degrees, float delta_pitch_degrees) = 0;
  // 平移(像素位移):沿相机右/上轴移动,移动量按 depth 处的世界尺度换算。
  virtual void pan(float delta_px_x, float delta_px_y) = 0;
  // zoom:透视沿视线推拉 distance(乘法),正交改 ortho_height;delta > 0 为放大。
  virtual void zoom(float delta) = 0;
  // 让包围球完整入画:把 target 设为球心,distance 设为包住半径所需距离。
  virtual void focus_on(const Vec3& center, float radius) = 0;

  // 视线距离(target 到 position)。
  virtual float distance() const = 0;
  // 相机前向(单位向量)。
  virtual Vec3 forward() const = 0;
  virtual Vec3 right() const = 0;

  // ------------------------------------------------------------ 快照(命令流)

  virtual CameraSnapshot snapshot() const = 0;
  virtual Status restore(const CameraSnapshot& snap) = 0;

  // 矩阵重算次数(性能断言:一帧内重复读取不得重复求值)。
  virtual u64 matrix_rebuild_count() const = 0;

 protected:
  ICamera() = default;
};

// 工厂。初始 desc/pose 由调用方再 set(默认透视 45°、位置 (0,2,5) 看向原点)。
std::unique_ptr<ICamera> create_camera();

}  // namespace magicblocks::primitives
