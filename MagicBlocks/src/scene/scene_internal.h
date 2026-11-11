// MagicBlocks SDK —— L2 scene 内部实现声明(G3:本目录不得出现 Vk*)
#pragma once

#include <memory>

#include "mb_internal/magicblocks/scene/feature.h"
#include "mb_internal/magicblocks/scene/recorder.h"
#include "mb_internal/magicblocks/scene/render_graph.h"
#include "mb_internal/magicblocks/scene/scene.h"
#include "mb_internal/magicblocks/scene/submit_queue.h"

namespace magicblocks::scene {

// S1:场景节点树。NodeId 单调不复用;快照按 id 升序。
std::unique_ptr<IScene> create_scene();

// S2:提交队列。排序稳定、量化键、相机驱动透明距离。
std::unique_ptr<ISubmitQueue> create_submit_queue();

// S3:渲染图。先写后读、附件解析、幂等 validate。
std::unique_ptr<IRenderGraph> create_render_graph();

// S9:渲染特性注册表。注册序即录制序。
std::unique_ptr<IRenderFeatureRegistry> create_feature_registry();

}  // namespace magicblocks::scene
