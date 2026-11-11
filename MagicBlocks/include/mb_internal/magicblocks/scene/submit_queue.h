// MagicBlocks SDK —— L2 S2 `ISubmitQueue`:提交队列与稳定排序
//
// 设计约束(spec §4 L2 S2 / plans-l2 §4.3):
//   - **不透明按 (material, depth) 排**:同一个 material 的绘制项聚在一起(减少状态切换),
//     同 material 内按深度从近到远(early-z 友好)。
//   - **透明按相机距离从远到近排**(B6):距离必须由**相机位置**算出来,
//     因此 `sort()` **接收相机参数**。同一队列配两个不同相机必须得到相反的顺序
//     (SubmitQueue.DistanceKeyUsesCamera)。
//   - **tie_break_id 兜底**:全部排序键都相同时按 `tie_break_id` 升序。
//     这让排序对插入顺序完全免疫 —— 最后一个键相同的一对仍能确定性地排列
//     (SubmitQueue.TieBreakIsDrawItemId)。
//   - **浮点量化**:距离/深度在参与排序前先量化成整数键,避免 `-0.0` vs `0.0`、
//     NaN、以及跨编译器浮点细微差异导致顺序漂移(spec §6.3 / R7)。
//   - **两次 sort 逐字节相同**(SubmitQueue.SortIsDeterministic):排序器不得依赖
//     哈希表遍历序、指针值或任何不稳定量。
//   - 本接口不读时钟、不碰 GPU。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mb_internal/magicblocks/scene/types.h"

namespace magicblocks::scene {

// 一个绘制项。**只用值语义**,不含资源指针 —— 命令流要能把它原样落盘。
struct DrawItem {
  std::uint64_t tie_break_id = 0;   // 全键相同时的最后判据。由调用方给,必须唯一。
  std::uint32_t material = 0;       // 材质槽位(排序主键之一)
  bool transparent = false;         // true → 走远到近;false → 走 material+depth
  // 世界空间中心点(相机距离由它算出来)。单位与世界坐标一致。
  float center[3] = {0.0f, 0.0f, 0.0f};
  // 该绘制项自身的深度值(同 material 内近到远用)。调用方按需填。
  float depth = 0.0f;

  // 网格 / 索引缓冲的句柄(值语义,M3)。是否填充由调用方决定,提交队列不解释它。
  std::uint64_t mesh_key = 0;
  // 第一索引与索引数量(绘制调用直接用量)。
  std::uint32_t first_index = 0;
  std::uint32_t index_count = 0;
};

// 相机参数(只取排序需要的部分)。
struct SubmitCamera {
  float position[3] = {0.0f, 0.0f, 0.0f};
};

class ISubmitQueue {
 public:
  ISubmitQueue(const ISubmitQueue&) = delete;
  ISubmitQueue& operator=(const ISubmitQueue&) = delete;
  virtual ~ISubmitQueue() = default;

  // 加入一个绘制项。不排序 —— 必须先 add 完全部再调 sort()。
  virtual void add(const DrawItem& item) = 0;

  // 清空(帧开始时调)。
  virtual void clear() = 0;

  virtual std::size_t size() const = 0;

  // 对已加入的全部绘制项排序。**不透明项排在前,透明项排在后**。
  // 返回排序后的只读列表。相机只影响透明项的顺序。
  virtual const std::vector<DrawItem>& sort(const SubmitCamera& camera) = 0;

  // 上一次 sort() 之后的顺序(未 sort 过则为空)。
  virtual const std::vector<DrawItem>& sorted() const = 0;

  // 不透明 / 透明各自的项数(排序后才有意义;未排序时按加入时的标志统计)。
  virtual std::size_t opaque_count() const = 0;
  virtual std::size_t transparent_count() const = 0;

 protected:
  ISubmitQueue() = default;
};

}  // namespace magicblocks::scene
