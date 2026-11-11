// MagicBlocks UI 消费层 —— SDK 渲染接口的**桩**(spec §2 分层、§4 IFeatureHost)
//
// 这是本轮唯一的"跨层"说明,读完这段再动下面的头:
//
// 1. spec §4 要求 `IFeatureHost`「只依赖 SDK 的 IScene、IInputEvent、IRenderFeature」,
//    但 SDK 目前**只落地了 L0**(`include/magicblocks/device/`),L2 的 `IScene` /
//    `IRenderFeature` / `ICommandList` 还不存在(reference 设计 §4 L2,计划里属后续阶段)。
// 2. 因此本目录下的 `scene_stub.h` / `overlay.h` 是 **L2/L3 接口在本仓库的桩声明**,
//    命名与语义逐字对齐 reference 设计 §4 的 S1/S9/P5,等 SDK 真落地时**按名字替换实现**,
//    UI 层调用点不需要改。
// 3. 桩**只声明不实现**(纯虚 / 返回 Unsupported),不合入 SDK 库,不参与门控 G3
//    (G3 只看 `src/backend` 与 `src/features`)。
//
// UI 层对桩的唯一硬约束:**桩不得成为第二条上屏路径**。UI pass 作为
// `IRenderFeature` 的又一个注册项进渲染图,附件 = 当前 target 的颜色附件(spec §5.3)。
#pragma once

// 输入事件直接用 SDK 的 L0 公共类型,UI 层不复制一份(复制会破坏命令复现回放)。
#include "mb_internal/magicblocks/device/events.h"
#include "mb_internal/magicblocks/ui/types.h"
