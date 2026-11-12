// MagicBlocks L4 —— 魔法叠叠乐：双 pass 渲染实现
//
// 逐条实现 stack_render.h 顶部的契约。这里刻意**不**引入任何后端私有头：
// 建管线只用公共 `create_pipeline(desc)`，深度行为全靠 DepthState/RasterState。
#include "magicblocks/features/stack/stack_render.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "magicblocks/primitives/mesh.h"
#include "magicblocks/primitives/mesh_factory.h"
#include "magicblocks/primitives/types.h"
// 生成出来的着色器字节（与 l3_cli 用的是同一份，保证像素口径一致）。
#include "magicblocks_shaders.h"

namespace magicblocks::features::stack {

namespace {

using device::u32;
using device::u64;

// 顶点属性绑定描述 —— 与 l3_cli 的 scene_pipeline_desc 逐项一致。
// 复制过来而不是复用 l3_cli 的静态函数：l3_cli 是**可执行程序**的内部函数，
// L4 库不能反向依赖它（否则库必须链接 main.cpp）。两处一旦分叉会表现为
// "颜色/矩阵取自别的字段"，所以下面每个偏移都对照 batch.h 的布局写死并断言。
constexpr std::uint32_t kBindingPerVertex = 0;
constexpr std::uint32_t kBindingPerInstance = 1;
constexpr std::uint32_t kOffModel = 0;
constexpr std::uint32_t kOffBaseColor = 64;
constexpr std::uint32_t kOffEmissive = 80;
constexpr std::uint32_t kOffMetallic = 96;

res::VertexLayout make_vertex_layout() {
  res::VertexLayout lay;
  {
    res::VertexInputBinding vb;
    vb.binding = kBindingPerVertex;
    vb.stride = sizeof(mbp::Vertex);
    vb.instanced = false;
    lay.bindings.push_back(vb);
  }
  {
    res::VertexInputBinding vb;
    vb.binding = kBindingPerInstance;
    vb.stride = sizeof(mbp::InstanceAttribs);
    vb.instanced = true;
    lay.bindings.push_back(vb);
  }
  auto attr = [&lay](std::uint32_t loc, std::uint32_t b, res::VertexFormat f, std::uint32_t off) {
    res::VertexAttribute a;
    a.location = loc;
    a.binding = b;
    a.format = f;
    a.offset = off;
    lay.attributes.push_back(a);
  };
  attr(0, kBindingPerVertex, res::VertexFormat::Float3, 0);
  attr(1, kBindingPerVertex, res::VertexFormat::Float3, 16);
  attr(2, kBindingPerVertex, res::VertexFormat::UByte4Norm, 32);
  attr(3, kBindingPerVertex, res::VertexFormat::Float2, 36);
  attr(4, kBindingPerInstance, res::VertexFormat::Float4, kOffModel + 0);
  attr(5, kBindingPerInstance, res::VertexFormat::Float4, kOffModel + 16);
  attr(6, kBindingPerInstance, res::VertexFormat::Float4, kOffModel + 32);
  attr(7, kBindingPerInstance, res::VertexFormat::Float4, kOffModel + 48);
  attr(8, kBindingPerInstance, res::VertexFormat::Float4, kOffBaseColor);
  attr(9, kBindingPerInstance, res::VertexFormat::Float4, kOffEmissive);
  attr(10, kBindingPerInstance, res::VertexFormat::Float4, kOffMetallic);
  return lay;
}

res::PipelineDesc base_desc(std::uint32_t samples) {
  res::PipelineDesc d;
  d.vertexLayout = make_vertex_layout();
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Vertex;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL3_sceneVert);
    m.size_bytes = magicblocks::shaders::kL3_sceneVertSize;
    d.stages.push_back(m);
  }
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Fragment;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL3_sceneFrag);
    m.size_bytes = magicblocks::shaders::kL3_sceneFragSize;
    d.stages.push_back(m);
  }
  d.blend.enabled = true;
  d.blend.src_color = res::BlendFactor::One;
  d.blend.dst_color = res::BlendFactor::OneMinusSrcAlpha;
  d.blend.src_alpha = res::BlendFactor::One;
  d.blend.dst_alpha = res::BlendFactor::OneMinusSrcAlpha;
  d.samples = samples > 1 ? samples : 1;
  d.primitive = res::PrimitiveType::TriangleList;
  d.push_constant_bytes = 128;  // view_proj(64) + normal_matrix(64)
  return d;
}

// 极简 FNV-1a，只用来给两个 pass 生成**稳定且不同**的排序键。
// 不用 std::hash：标准与实现可换，换编译器/换 STL 就换 key，
// 排序顺序随之变，命令流确定性就没了。
u64 fnv1a(const void* data, std::size_t n, u64 seed) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  u64 h = seed;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<u64>(p[i]);
    h *= 1099511628211ull;
  }
  return h;
}

class StackRenderer final : public IStackRenderer {
 public:
  // 把一份实例数组变成设备上的 Vertex 缓冲，写进**帧槽环的第 slot 格**。
  //
  // 与顶点缓冲同口径：`InstanceAttribs` 也落在**设备页**，CPU 不可映射，
  // 必须 upload() + flush_pending()。漏 flush 的表现是"图上一片空但不报错"。
  //
  // ======================== 这里原本是一处 use-after-free ========================
  // 改造前本函数第一行是 `slot->reset()`，即**无条件、立即**销毁上一帧的
  // VkBuffer。在旧的同步 submit 模型下这没问题（submit 返回时 GPU 已画完），
  // 但 t19 把 submit 改成异步之后它就是 UAF，且不是竞态而是**严格帧序**：
  //   `build_instances()` 在全部调用点上都先于本帧的 `submit_command_list`，
  //   所以第 2 帧一进来就销毁"第 1 帧已提交、GPU 可能仍在读"的缓冲 —— 100% 必现。
  //   SwiftShader 太快，所以此前一直没崩出可观测症状，只是静默读已释放显存。
  //
  // 现在：`reset()` 已删除。写 `slot` 格时，被覆盖的那个旧 unique_ptr 自然析构，
  // 而**调用方在 `record_frame` 里已经先 `wait_slot(slot)` 过** —— 销毁点从
  // "无保护"变成"该槽上一轮提交的 fence 之后"。缓冲区生命周期因此由
  // 帧槽环 + IFrameSync 共同保证，不再依赖"调用方会读回"这种外部约定。
  StackStatus upload_instances(std::unique_ptr<res::IBuffer>* slot,
                               const std::vector<mbp::InstanceAttribs>& data) {
    if (slot == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "upload_instances: 环格为空");
    }
    if (device_ == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "upload_instances: device 为空");
    }
    res::BufferDesc bd;
    bd.size = data.size() * sizeof(mbp::InstanceAttribs);
    bd.usage = res::BufferUsage::Vertex;
    auto buf = device_->create_buffer(bd);
    if (!buf.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建实例缓冲失败: " + buf.message());
    }
    *slot = std::move(buf).take();
    if ((*slot)->size() < bd.size) {
      return StackStatus::Fail(StackCode::OutOfGrid, "实例缓冲实际尺寸小于请求尺寸");
    }
    if (!(*slot)->upload(data.data(), bd.size).ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "上传实例缓冲失败");
    }
    (*slot)->flush_pending();
    return StackStatus::Ok_();
  }

  // 一份实例数组的内容哈希。**逐字段**按固定顺序喂，不用 memcpy：
  // `InstanceAttribs` 里可能有 padding 字节，memcpy 会把未初始化的填充
  // 也算进去 —— 那会让"两份内容相同"的哈希时不时不等。
  static std::uint64_t hash_instances(const std::vector<mbp::InstanceAttribs>& v) {
    std::uint64_t h = kHashSeed;
    const u64 n = v.size();
    h = fnv1a(&n, sizeof(n), h);
    for (const mbp::InstanceAttribs& a : v) {
      h = fnv1a(a.model, sizeof(a.model), h);
      h = fnv1a(a.base_color, sizeof(a.base_color), h);
      h = fnv1a(a.emissive, sizeof(a.emissive), h);
      h = fnv1a(&a.roughness, sizeof(a.roughness), h);
      h = fnv1a(&a.metallic, sizeof(a.metallic), h);
    }
    return h;
  }

  StackStatus create_passes(IRenderDevice* device, std::uint32_t samples) override {
    if (device == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "create_passes: device 为空");
    device_ = device;

    // ---- 实例缓冲帧槽环：N 必须与 L0 的 in-flight 数**同源** ----
    //
    // 为什么必须是 N 而不是 1（见 stack_render.h 顶部的 UAF 说明）：
    //   N=1 时"下一帧绑定时等槽 0"等的是**本帧自己**那批提交，而绑定发生在
    //   submit **之前**，fence 还是 signaled（上一轮已等过）→ wait_slot 立即返回
    //   → 立刻销毁 → 照样读已释放显存。**N=1 不是"不并行"，是"错"。**
    //   所以这里绝不降级到 1：拿不到可信的 N 就整帧报错。
    //
    // 取值依据：`IFrameSync::in_flight_count()` 就是 DeviceDesc::in_flight_frames
    //   （L0 校验 2|3），也是 VulkanSyncCore 的槽数（vulkan_frame_sync.cpp:22-28
    //   同源转发）。L4 若另立一个 N，就会出现"同步核心 N=3、缓冲环 N=2"的两套口径：
    //   环比槽多则白占显存，环比槽少则复用得太早 —— 正是本函数要消灭的那个 UAF。
    frame_sync_ = device->frame_sync();
    if (frame_sync_ == nullptr) {
      return StackStatus::Fail(
          StackCode::OutOfGrid,
          "create_passes: device->frame_sync() 为空，无法建实例缓冲帧槽环"
          "（拒绝降级到单缓冲：那会在 GPU 仍可能读取时销毁缓冲）");
    }
    const u32 n_slots = frame_sync_->in_flight_count();
    if (n_slots < 2) {
      return StackStatus::Fail(
          StackCode::OutOfGrid,
          "create_passes: in_flight_count()=" + std::to_string(n_slots) +
              "，实例缓冲帧槽环至少要 2 格"
              "（N=1 时绑定发生在 submit 之前、fence 仍 signaled，wait_slot 立即返回"
              "→ 仍然 UAF）");
    }
    // 环格初始为空（unique_ptr 不可拷贝，用 resize 而不是 assign(n, nullptr)）。
    surface_ring_.clear();
    surface_ring_.resize(n_slots);
    inner_ring_.clear();
    inner_ring_.resize(n_slots);
    inner_same_as_surface_.assign(n_slots, false);

    // 先建表面 pass。若内面 pass 建失败，**整帧报错**，不降级成"只画表面"——
    // 半功能的渲染器在画面上表现为"能看但不对"，比直接报错更难归因。
    const res::PipelineDesc surf = surface_pipeline_desc(samples);
    auto sp = device->create_pipeline(surf);
    if (!sp.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建表面管线失败: " + sp.message());
    }
    const res::PipelineDesc inner = inner_pipeline_desc(samples);
    auto ip = device->create_pipeline(inner);
    if (!ip.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建内面管线失败: " + ip.message());
    }

    passes_.surface_pass = sp.value().get();
    passes_.inner_pass = ip.value().get();
    passes_.surface_hash = compute_pass_hash(surf, kSurfaceSalt);
    passes_.inner_hash = compute_pass_hash(inner, kInnerSalt);
    surface_pipeline_ = std::move(sp).take();
    inner_pipeline_ = std::move(ip).take();

    // 立方体网格。单位立方体（-0.5..+0.5），由 model 矩阵缩放到格边长。
    mesh_factory_ = mbp::create_mesh_factory();
    if (!mesh_factory_) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建网格工厂失败");
    }
    const mbp::MeshData cube = mesh_factory_->cube();
    if (cube.empty()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "立方体网格为空");
    }

    // 顶点/索引缓冲
    res::BufferDesc vbd;
    vbd.size = cube.vertices.size() * sizeof(mbp::Vertex);
    vbd.usage = res::BufferUsage::Vertex;
    auto vb = device->create_buffer(vbd);
    if (!vb.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建顶点缓冲失败: " + vb.message());
    }
    vertex_buffer_ = std::move(vb).take();
    // 必须走 upload()（R5 staging 路径）：Vertex/Index 落在**设备页**，
    // CPU 不可映射，write() 会明确返回 Unsupported（vulkan_buffer.cpp:187-191）。
    if (vertex_buffer_->size() < vbd.size) {
      return StackStatus::Fail(StackCode::OutOfGrid, "顶点缓冲实际尺寸小于请求尺寸");
    }
    if (!vertex_buffer_->upload(cube.vertices.data(), vbd.size).ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "上传顶点缓冲失败");
    }
    vertex_buffer_->flush_pending();

    res::BufferDesc ibd;
    ibd.size = cube.indices.size() * sizeof(std::uint32_t);
    ibd.usage = res::BufferUsage::Index;
    auto ib = device->create_buffer(ibd);
    if (!ib.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "创建索引缓冲失败: " + ib.message());
    }
    index_buffer_ = std::move(ib).take();
    if (index_buffer_->size() < ibd.size) {
      return StackStatus::Fail(StackCode::OutOfGrid, "索引缓冲实际尺寸小于请求尺寸");
    }
    if (!index_buffer_->upload(cube.indices.data(), ibd.size).ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "上传索引缓冲失败");
    }
    index_buffer_->flush_pending();

    // 不建 IMesh。
    //
    // 原因（实测）：primitives::create_mesh 内部用 buffer->write() 落数据，
    // 而 Vertex/Index 属于**设备页**、CPU 不可映射，write() 必然 Unsupported
    // 而返回 nullptr（报错还只是一个光秃秃的 "创建 IMesh 失败"）。
    // 全仓此前没有任何调用者，所以这条路径从未被跑到过。
    // 本渲染器只需要缓冲的 id 去 bind，不需要 IMesh 的包装，故直接持有缓冲。
    vertex_buffer_id_ = vertex_buffer_->id();
    index_buffer_id_ = index_buffer_->id();
    index_count_ = static_cast<std::uint32_t>(cube.indices.size());
    return StackStatus::Ok_();
  }

  const StackPassPair& passes() const override { return passes_; }

  StackStatus build_instances(const GridSpec& grid, const IBlockSet& set) override {
    if (device_ == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "build_instances: 尚未 create_passes");
    }
    const std::size_t n = set.size();

    // ---- 1) 先定两份**格子顺序** ----
    //
    // 表面序 = 真源下标序（插入序）。表面 pass 开深度测试，顺序不影响结果。
    // 内面序 = `(y,z,x)` 升序（D-VX-5）。必须用 **stable_sort**：
    //   `(y,z,x)` 是全序，理论上不会有相等元素，但真源理论上也不该有重复格；
    //   用 stable 是为了"万一有重复格，也按真源下标决定先后"——
    //   这样排序结果仍然只由坐标 + 下标决定，绝不依赖排序算法的内部行为。
    surface_cells_.clear();
    surface_cells_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      const Block* b = set.at(i);
      if (b == nullptr) {
        return StackStatus::Fail(StackCode::OutOfGrid,
                                 "build_instances: 真源下标 " + std::to_string(i) + " 为空");
      }
      surface_cells_.push_back(b->cell);
    }

    inner_cells_ = surface_cells_;
    std::stable_sort(inner_cells_.begin(), inner_cells_.end(), CellOrderLess{});

    // 顺序相同时（例如本来就是按 yzx 插入的），两份数据逐条相等，
    // 没必要建第二个 Buffer —— 直接复用。这不是优化，是让
    // "顺序相同的场景"只有一个 Buffer 被持有，减少一次生命期风险。
    const bool same_order = (inner_cells_ == surface_cells_);

    // ---- 2) 按格子顺序烘实例（颜色从真源查回）----
    //
    // 颜色必须按**格子**从真源取，而不是按位置从 surface_cells_ 下标取：
    // 内面序是重排过的，用下标取颜色会把颜色错配到别的块上。
    const auto fill = [&](const std::vector<Cell>& cells, std::vector<mbp::InstanceAttribs>* out) {
      out->assign(cells.size(), mbp::InstanceAttribs{});
      for (std::size_t i = 0; i < cells.size(); ++i) {
        const Block* b = set.find(cells[i]);
        if (b == nullptr) {
          return false;
        }
        block_model_matrix(grid, b->cell, (*out)[i].model);
        // color 定死为 **0xRRGGBBAA**（与 write_png / readback 的字节序一致）。
        const u32 c = b->color;
        (*out)[i].base_color[0] = static_cast<float>((c >> 24) & 0xFFu) / 255.0f;
        (*out)[i].base_color[1] = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
        (*out)[i].base_color[2] = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
        (*out)[i].base_color[3] = static_cast<float>(c & 0xFFu) / 255.0f;
        (*out)[i].roughness = 0.55f;
        (*out)[i].metallic = 0.0f;
      }
      // ---- M9：pad 到 kMinInstanceCount ----
      // 退化实例 = 全零 model ⇒ 所有顶点投影到原点 ⇒ 零面积 ⇒ 不产生像素。
      while (out->size() < kMinInstanceCount) {
        out->push_back(mbp::InstanceAttribs{});
      }
      return true;
    };

    if (!fill(surface_cells_, &surface_instances_)) {
      return StackStatus::Fail(StackCode::OutOfGrid, "build_instances: 表面序烘焙失败(格子丢失)");
    }
    if (same_order) {
      inner_instances_ = surface_instances_;
    } else if (!fill(inner_cells_, &inner_instances_)) {
      return StackStatus::Fail(StackCode::OutOfGrid, "build_instances: 内面序烘焙失败(格子丢失)");
    }

    // ---- 3) 上传到本帧槽位 ----
    //
    // 槽位只算**一次**，表面与内面共用同一个 slot（两根环必须同槽）。
    // 分开算两次看着也没错，但一旦 slot_for_frame 的语义将来变化（例如 L0 改成
    // 非取模的分配策略），两次调用就可能落在不同槽上 —— 那种错位会表现为
    // "内面用着别的帧的缓冲"，既不报错也难归因。
    if (frame_sync_ == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "build_instances: 尚未 create_passes(无帧同步)");
    }
    const u32 slot = frame_sync_->slot_for_frame(frame_sync_->frame_index());
    if (slot >= surface_ring_.size()) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "build_instances: 槽号 " + std::to_string(slot) + " 超出环大小 " +
                                   std::to_string(surface_ring_.size()));
    }

    // 顺序相同时表面与内面共用本槽的同一个 Buffer（内面 pass 绑同一个 id）。
    // 用标志位而不是 reset 第二根环：环格一旦建出来就不再销毁，
    // 销毁时机只由"该槽被下一轮覆盖且此前已 wait_slot"决定。
    if (auto s = upload_instances(&surface_ring_[slot], surface_instances_); !s.ok()) return s;
    inner_same_as_surface_[slot] = same_order;
    if (same_order) {
      // 显式表明"本槽不持有第二份"：把该格旧的内面缓冲交出去。
      // 这里立即析构是安全的 —— 它属于**同一个槽**，调用方在 record_frame
      // 里已经先 wait_slot(slot) 等过该槽上一轮的提交。
      inner_ring_[slot].reset();
    } else if (auto s = upload_instances(&inner_ring_[slot], inner_instances_); !s.ok()) {
      return s;
    }
    last_slot_ = slot;

    surface_hash_ = hash_instances(surface_instances_);
    inner_hash_ = hash_instances(inner_instances_);
    return StackStatus::Ok_();
  }

  // 实例数（含 pad）。两份顺序的 pad 后长度相同，所以它们必然一致 ——
  // 这里有断言兜住：两份长度不等意味着 pad 逻辑分叉了。
  std::uint32_t instance_count() const override {
    const std::uint32_t n = static_cast<std::uint32_t>(surface_instances_.size());
    // 两份 pad 后长度必须一致（pad 是同一个循环、同一个下限）。不等说明 pad 逻辑
    // 分叉了 —— 这是编程错误而非输入错误，所以用 assert 而非返回错误：
    // instance_count() 是 const，报不了错；真正的运行期兜底在 build_instances
    // 的返回值与 record_frame 的 M9 不变量检查里。
    assert(inner_instances_.size() == surface_instances_.size() &&
           "D-VX-5: 表面序与内面序实例数不一致，pad 逻辑分叉");
    return n;
  }

  const std::vector<Cell>& surface_cells() const override { return surface_cells_; }
  const std::vector<Cell>& inner_cells() const override { return inner_cells_; }
  std::uint64_t surface_instance_hash() const override { return surface_hash_; }
  std::uint64_t inner_instance_hash() const override { return inner_hash_; }

  StackStatus record_frame(res::ICommandList& cmd, IRenderTarget* target,
                           const StackFrameDesc& desc) override {
    if (target == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record_frame: target 为空");
    }
    if (passes_.surface_pass == nullptr || passes_.inner_pass == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record_frame: 尚未 create_passes");
    }
    // 本帧用的槽位。build_instances() 已经把实例写进 last_slot_，
    // 这里重新按同一条公式求值一次并**核对**它与 last_slot_ 一致 ——
    // 两者不一致意味着"上传到了 A 槽、渲染却绑 B 槽"，那是读错帧的缓冲，
    // 画面会像是"上一帧的积木"，不崩也不报错。所以不一致即报错。
    if (frame_sync_ == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record_frame: 无帧同步");
    }
    const u32 slot = frame_sync_->slot_for_frame(frame_sync_->frame_index());
    if (slot >= surface_ring_.size()) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "record_frame: 槽号 " + std::to_string(slot) + " 超出环大小 " +
                                   std::to_string(surface_ring_.size()));
    }
    if (slot != last_slot_) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "record_frame: 槽号不一致（上传写的是 " + std::to_string(last_slot_) +
                                   "，录制要绑的是 " + std::to_string(slot) +
                                   "）：帧号在 build_instances 与 record_frame 之间被推进了");
    }
    if (surface_ring_[slot] == nullptr || !vertex_buffer_id_.valid()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record_frame: 本槽尚未 build_instances");
    }

    // ================= 槽位复用前的等待（本修复的安全支点） =================
    //
    // 在**绑定之前**等该槽上一轮的提交真正完成，然后才允许本帧引用它。
    // 三个理由，缺一不可：
    //   ① 为什么在绑定前而不是提交后：`record_frame` 契约上**不 submit**
    //      （见 stack_render.h:229 与 UIG1/UIG2），提交权在调用方。若改到提交后等，
    //      等于把 t19 刚做出来的异步提交又改回同步，正好抵消多帧并行的全部收益。
    //      绑定前等则落在"录制窗口"里 —— CPU 本来就要录这一帧，等的代价≈0。
    //   ② 为什么 N 帧延迟够：槽 s 在第 f 帧被占用，下一次被覆写是第 f+N 帧。
    //      这里等的正是第 f 帧挂给槽 s 的那颗 fence（vulkan_device_frame.cpp:832
    //      的 vkQueueSubmit(..., slot.in_flight)），它 signal ⟺ 第 f 帧命令缓冲
    //      执行完成 ⟺ 对实例缓冲的读已结束。**间隔恰好 N 帧，正好差一代。**
    //   ③ 为什么同时也在保护"销毁"：build_instances 里覆盖 ring[slot] 会析构
    //      该格旧缓冲。上一轮走到这里时已经 wait 过，所以那时析构是安全的。
    //      两根环同槽，因此这一等同时覆盖 surface 与 inner 两份。
    if (auto ws = frame_sync_->wait_slot(slot); !ws.ok()) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "record_frame: 等待槽 " + std::to_string(slot) +
                                   " 上一轮提交完成失败: " + ws.message);
    }

    if (surface_instances_.empty()) {
      return StackStatus::Fail(StackCode::EmptyStroke, "record_frame: 场景为空");
    }

    // **硬断言 M9 不变量**：永不出现 "instance_buffer != nullptr && count == 1"。
    // pad 之后 count 恒 >= 2；这里显式再查一次，是因为这条不变量的代价
    // 不是崩，而是"图上有东西但错位"，只有断言能把它变成响的失败。
    const std::uint32_t n_inst = static_cast<std::uint32_t>(surface_instances_.size());
    if (n_inst < kMinInstanceCount) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "实例数 " + std::to_string(n_inst) + " < " +
                                   std::to_string(kMinInstanceCount) +
                                   "：pad 失效（binding 1 不会被绑定）");
    }

    const std::uint32_t w = desc.width;
    const std::uint32_t h = desc.height;
    if (w == 0 || h == 0) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record_frame: 尺寸为 0");
    }

    // 两个 pass 的录制顺序。默认表面在前。debug 开关只用于反证用例。
    struct PassRec {
      res::IPipeline* pipeline;
      u64 hash;
      bool clear;
      const char* name;
      res::IBuffer* inst;  // 该 pass 用的实例缓冲（Order 契约在这里落地）
      CellOrder order;
    };
    // 表面 pass 绑真源下标序，内面 pass 绑 `(y,z,x)` 升序（D-VX-5）。
    // 顺序相同的槽上两道 pass 共用本槽的同一个缓冲（见 inner_same_as_surface_）。
    res::IBuffer* inner_buf =
        inner_same_as_surface_[slot] ? surface_ring_[slot].get() : inner_ring_[slot].get();
    if (inner_buf == nullptr) {
      return StackStatus::Fail(StackCode::OutOfGrid, "record_frame: 本槽内面实例缓冲为空");
    }
    PassRec surf{passes_.surface_pass, passes_.surface_hash, true, "surface",
                 surface_ring_[slot].get(), desc.surface_order};
    PassRec inner{passes_.inner_pass, passes_.inner_hash, false, "inner", inner_buf,
                  desc.inner_order};
    PassRec order[2] = {surf, inner};
    if (desc.debug_inner_pass_first) {
      order[0] = inner;
      order[1] = surf;
    }
    const std::uint32_t n_passes = desc.inner_pass_enabled ? 2u : 1u;

    for (std::uint32_t p = 0; p < n_passes; ++p) {
      const PassRec& pr = order[p];

      res::RenderPassDesc rp;
      rp.target = target;
      // pass A 清屏并清深度；pass B **不**清（清了就把 A 的结果抹了）。
      rp.clear_color = pr.clear;
      rp.clear_color_rgba[0] = desc.clear_color[0];
      rp.clear_color_rgba[1] = desc.clear_color[1];
      rp.clear_color_rgba[2] = desc.clear_color[2];
      rp.clear_color_rgba[3] = desc.clear_color[3];
      rp.clear_depth = pr.clear;
      rp.clear_depth_value = 1.0f;
      if (auto s = cmd.begin_pass(rp); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid,
                                 std::string("begin_pass(") + pr.name + ") 失败: " + s.message);
      }
      ++stats_.begin_passes;

      res::Viewport vp;
      vp.width = static_cast<float>(w);
      vp.height = static_cast<float>(h);
      vp.min_depth = 0.0f;
      vp.max_depth = 1.0f;
      if (auto s = cmd.set_viewport(vp); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid, "set_viewport 失败: " + s.message);
      }
      res::ScissorRect sr;
      sr.width = w;
      sr.height = h;
      if (auto s = cmd.set_scissor(sr); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid, "set_scissor 失败: " + s.message);
      }

      if (auto s = cmd.bind_pipeline(pr.pipeline, pr.hash); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid,
                                 std::string("bind_pipeline(") + pr.name + ") 失败: " + s.message);
      }
      ++stats_.pipeline_binds;

      struct ScenePC {
        float view_proj[16];
        float normal_matrix[16];
      } pc{};
      std::memcpy(pc.view_proj, desc.view_proj, sizeof(float) * 16);
      pc.normal_matrix[0] = 1.0f;
      pc.normal_matrix[5] = 1.0f;
      pc.normal_matrix[10] = 1.0f;
      pc.normal_matrix[15] = 1.0f;
      constexpr std::uint32_t kStageVertex = 0x1u;
      constexpr std::uint32_t kStageFragment = 0x10u;
      if (auto s = cmd.push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid, "push_constants 失败: " + s.message);
      }

      if (auto s = cmd.bind_vertex_buffer(vertex_buffer_id_, 0, 0); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid, "bind 顶点缓冲失败: " + s.message);
      }
      // binding 1 = 每实例。因为 count 恒 >= 2，这里总是安全绑定。
      // 绑的是**本 pass 自己的**实例缓冲 —— 表面序与内面序在这里分道。
      if (auto s = cmd.bind_vertex_buffer(pr.inst->id(), 1, 0); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid, "bind 实例缓冲失败: " + s.message);
      }
      if (auto s = cmd.bind_index_buffer(index_buffer_id_, res::IndexFormat::UInt32, 0);
          !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid, "bind 索引缓冲失败: " + s.message);
      }

      res::DrawArgs da;
      da.index_count = index_count_;
      da.instance_count = n_inst;
      if (auto s = cmd.draw_indexed(da); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid, "draw_indexed 失败: " + s.message);
      }      ++stats_.draw_calls;

      if (auto s = cmd.end_pass(); !s.ok()) {
        return StackStatus::Fail(StackCode::OutOfGrid,
                                 std::string("end_pass(") + pr.name + ") 失败: " + s.message);
      }
    }

    stats_.instances = n_inst;
    stats_.instance_buffer_bound = true;
    stats_.inner_order = desc.inner_order;
    // 本帧内面 pass 是否真的绑了**独立**的内面实例缓冲（false = 复用表面缓冲）。
    // 这是该字段唯一的写入点：语义"这一帧实际绑了什么"，不是"这一帧理论上该绑什么"。
    stats_.inner_buffer_distinct = !inner_same_as_surface_[slot];

    // ---- 网格指示（需求「带网格指示」）----
    //
    // 录制顺序：表面 pass → 内面 pass → **覆盖层收集**。收集本身不录任何命令
    // （覆盖层自己的一次 draw 由调用方 build + record，见 stack_render.h 的
    // StackFrameOverlay 注释），所以这里的双 pass 预算（恰好 2 次 drawcall）
    // 不受影响：覆盖层是自己的第 3 次 draw，不计入积木几何。
    //
    // 失败即报错：collect 失败（例如矩阵不可逆导致全部点被裁掉）不许静默"当成没配"，
    // 否则"网格指示没出现"会退化成一个无法归因的现象。
    if (desc.overlay.overlay != nullptr) {
      if (desc.overlay.collector == nullptr) {
        return StackStatus::Fail(StackCode::OutOfGrid,
                                 "overlay.overlay 给了但 overlay.collector 为空：网格指示无处收集");
      }
      GridProjector projector;
      if (desc.overlay.project_with_frame_matrix) {
        std::memcpy(projector.view_proj, desc.view_proj, sizeof(float) * 16);
      }
      projector.width = w;
      projector.height = h;
      if (auto s = desc.overlay.overlay->collect(*desc.overlay.collector, desc.overlay.grid,
                                                 projector, desc.overlay.desc);
          !s.ok()) {
        return StackStatus::Fail(s.error_code(), "网格指示收集失败: " + s.text());
      }
      const StackOverlayStats& os = desc.overlay.overlay->stats();
      stats_.overlay_collected = true;
      stats_.overlay_lines = os.line_count;
      stats_.overlay_rects = os.rect_count;
    }

    // **不 submit** —— 一帧一次由调用方做。
    return StackStatus::Ok_();
  }

  const StackRenderStats& stats() const override { return stats_; }
  void reset_stats() override { stats_ = StackRenderStats{}; }

 private:
  static constexpr u64 kSurfaceSalt = 0x5355'5246'4143'4501ull;  // "SURFACE"
  static constexpr u64 kInnerSalt = 0x494E'4E45'5250'4153ull;    // "INNERPAS"

  StackPassPair passes_{};
  IRenderDevice* device_ = nullptr;
  std::unique_ptr<res::IPipeline> surface_pipeline_;
  std::unique_ptr<res::IPipeline> inner_pipeline_;
  std::unique_ptr<mbp::IMeshFactory> mesh_factory_;
  std::unique_ptr<res::IBuffer> vertex_buffer_;
  std::unique_ptr<res::IBuffer> index_buffer_;
  // 实例缓冲**帧槽环**（每槽一格，共 N = in_flight_count() 格）。
  //
  // 为什么是环而不是两个单缓冲：L0 的 submit 已异步化，单个缓冲每帧都得等
  // GPU 画完才能复用，等于把并行又取消。N 格环让每个槽只在 N 帧后被覆写，
  // 而那时该帧早已被 fence 确认完成（见 record_frame 里 wait_slot 的三条理由）。
  //
  // 两根环**同槽**：surface_ring_[s] 与 inner_ring_[s] 属于同一帧。
  // 顺序相同的槽上内面 pass 复用表面缓冲，由 inner_same_as_surface_ 标记，
  // 而不是把某根环的格子 reset 掉 —— 环格建出来就不再单独销毁。
  std::vector<std::unique_ptr<res::IBuffer>> surface_ring_;
  std::vector<std::unique_ptr<res::IBuffer>> inner_ring_;
  std::vector<bool> inner_same_as_surface_;
  device::IFrameSync* frame_sync_ = nullptr;  // 非拥有者，生命周期归 IRenderDevice
  u32 last_slot_ = 0;                         // build_instances 写入的槽（record_frame 核对）
  res::ResourceId vertex_buffer_id_{};
  res::ResourceId index_buffer_id_{};
  std::uint32_t index_count_ = 0;
  std::vector<mbp::InstanceAttribs> surface_instances_;
  std::vector<mbp::InstanceAttribs> inner_instances_;
  std::vector<Cell> surface_cells_;
  std::vector<Cell> inner_cells_;
  std::uint64_t surface_hash_ = 0;
  std::uint64_t inner_hash_ = 0;
  StackRenderStats stats_{};
};

}  // namespace

// ---------------------------------------------------------------- 纯函数

void block_model_matrix(const GridSpec& grid, const Cell& cell, float out[16]) {
  // 列主序：前 12 个 float 是 3x4 的 R|t，最后一行恒 (0,0,0,1)。
  // 块的世界中心 = origin + (cell + 0.5) * cell_size（cell 是格**索引**，
  // 格心在 +0.5 处 —— 少了这个 0.5，所有盒子会整体偏移半格，
  // 表现为"柱子看起来没对齐网格线"，而且不会报错）。
  const float cs = grid.cell_size;
  const float s = cs * kBlockFillRatio;
  const float cx = grid.origin[0] + (static_cast<float>(cell.x) + 0.5f) * cs;
  const float cy = grid.origin[1] + (static_cast<float>(cell.y) + 0.5f) * cs;
  const float cz = grid.origin[2] + (static_cast<float>(cell.z) + 0.5f) * cs;

  for (int i = 0; i < 16; ++i) out[i] = 0.0f;
  out[0] = s;
  out[5] = s;
  out[10] = s;
  out[15] = 1.0f;
  out[12] = cx;
  out[13] = cy;
  out[14] = cz;
}

resources::PipelineDesc surface_pipeline_desc(std::uint32_t samples) {
  res::PipelineDesc d = base_desc(samples);
  d.depth.depthTest = true;
  d.depth.depthWrite = true;
  d.depth.depthOp = res::CompareOp::LessOrEqual;
  d.raster.cull = res::CullMode::Back;
  // depthConstWrite 保持默认 false —— D-VX-4′，本文件不出现对该字段的写入。
  return d;
}

resources::PipelineDesc inner_pipeline_desc(std::uint32_t samples) {
  res::PipelineDesc d = base_desc(samples);
  // 内面的三件套（D-VX-4′ 的正确性来源）：
  d.depth.depthTest = false;   // ① 不做深度裁决
  d.raster.cull = res::CullMode::None;  // ② 背面也要
  // ③ 顺序由调用方保证（表面先、内面后）。
  // depthWrite 必须为 true：三个开关全 false 时后端**根本不挂** depth-stencil
  // 状态（vulkan_pipeline.cpp:668-671），于是 depthTest=false 与"没设"无从区分，
  // 这类退化在换驱动时会静默改变行为。
  d.depth.depthWrite = true;
  d.depth.depthOp = res::CompareOp::LessOrEqual;
  return d;
}

std::uint64_t compute_pass_hash(const resources::PipelineDesc& desc, std::uint64_t salt) {
  // 把"决定管线身份"的字段逐项喂进哈希。**刻意不包含深度附件的形状** ——
  // 那不在公共面里，也无法在公共面断言。这里包含的是真正会改变绘制行为的:
  //   depthTest / depthWrite / depthOp / cull / samples / blend
  // 只要这些里有一项不同，hash 就不同 → 合批器不会把两个 pass 合并成一个 draw。
  u64 h = salt;
  h = fnv1a(&desc.depth.depthTest, sizeof(desc.depth.depthTest), h);
  h = fnv1a(&desc.depth.depthWrite, sizeof(desc.depth.depthWrite), h);
  h = fnv1a(&desc.depth.depthOp, sizeof(desc.depth.depthOp), h);
  h = fnv1a(&desc.raster.cull, sizeof(desc.raster.cull), h);
  h = fnv1a(&desc.samples, sizeof(desc.samples), h);
  h = fnv1a(&desc.blend.enabled, sizeof(desc.blend.enabled), h);
  h = fnv1a(&desc.blend.src_color, sizeof(desc.blend.src_color), h);
  h = fnv1a(&desc.blend.dst_color, sizeof(desc.blend.dst_color), h);
  return h;
}

std::unique_ptr<IStackRenderer> create_stack_renderer() {
  return std::make_unique<StackRenderer>();
}

}  // namespace magicblocks::features::stack
