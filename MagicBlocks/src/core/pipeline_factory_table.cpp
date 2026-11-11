// MagicBlocks SDK —— L1 按形状派生管线的后端工厂表(G8, 2026-11-11)
//
// 背景(t13/g8):
//   L3 的 IShaderLibrary 需要按目标附件形状派生管线。此前它直接 #include
//   "backend/vulkan/vulkan_pipeline.h" 拿 backend::AttachmentShape,并
//   dynamic_cast 到 backend::VulkanRenderDevice 调 create_pipeline_for_shape。
//   这是 L3 反向依赖 L0 实现,G8 门控判红。
//
// 本文件实现单向注入:
//   - 后端在初始化时调用 register_pipeline_factory(ResolverFn),把我们解析器
//     登记进来(实现 see backend/vulkan/vulkan_pipeline_factory.cpp);
//   - L3 只调 resources::resolve_pipeline_factory(device),拿纯虚接口;
//   - 本文件**零** backend / Vk 符号,只有一份函数指针。
#include "magicblocks/resources/pipeline.h"

namespace magicblocks::resources {

namespace {
// 存的是**解析器**本身(接收 device、返回 factory),不是注册器。
// register_pipeline_factory 的参数类型是 PipelineFactoryRegisterFn(接收解析器的
// 注册器函数),所以这里必须用 PipelineFactoryResolveFn,否则 g_resolver_fn(device)
// 会把 device 当成"要注册的解析器函数"传进去 —— 类型不匹配(C2664)。
PipelineFactoryResolveFn g_resolver = nullptr;
}  // namespace

void register_pipeline_factory(PipelineFactoryRegisterFn fn) {
  // 注册器的语义是"把解析器交给我"。但本文件自己就是解析器的持有者,所以这里
  // 直接把它当作解析器存下来 —— 见下方 resolve_pipeline_factory 的调用方式。
  g_resolver = reinterpret_cast<PipelineFactoryResolveFn>(fn);
}

IPipelineFactory* resolve_pipeline_factory(device::IRenderDevice* device) {
  if (g_resolver == nullptr || device == nullptr) return nullptr;
  return g_resolver(device);
}

}  // namespace magicblocks::resources
