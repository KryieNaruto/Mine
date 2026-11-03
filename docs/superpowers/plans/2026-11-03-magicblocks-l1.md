# MagicBlocks L1（resources / memory / command）落地计划 · 第 1 版

日期：2026-11-03 · 规格：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`
（§2、§4 L1 R1~R6 + M1~M6、§6、§7、§11、§13 第 1 阶段的后半）
前置：`docs/superpowers/plans/2026-11-03-magicblocks-l0.md`（L0 已落地，ctest 32/32 全绿）

## 0. 本轮范围

### 0.1 一句话范围

只落 **L1**：SDK 公共头 `magicblocks/resources/*`（`IMemoryAllocator` / `IBuffer` / `ITexture` /
`ISampler` / `IPipeline` / `ICommandList` / 资源句柄）+ Vulkan 后端实现（**VMA v3.5.0**
分页 + 桶内空闲链表偏移子分配）+ 把 L0 遗留的 9 处 `// VMA-injection` 裸分配**全部换成 VMA**，
使 G4 从「backend 外零裸分配」收紧到「**全仓零裸分配**」。

L2 scene / L3 primitives / L4 features 全部不做，接口位也不在本轮提交（与 L0 同口径）。

### 0.2 本轮要闭合的硬约束

| 约束 | 在本轮的落地 | 判定方式 |
|---|---|---|
| **C7** VMA v3.5.0 大块 + 偏移子分配 | `VulkanPagingAllocator`：64 MB 设备页 / 8 MB Host 页，桶内空闲链表切偏移 | 单测直接读写页/桶状态 |
| **M5** 稳态帧 VMA alloc/free == 0 | 每帧批量预留页，热路径只动空闲链表 | `Allocator.SteadyStateZeroVmaCalls` **硬断言计数 == 0** |
| **M3** 句柄 = 偏移 | `ResourceId{page_id, offset, size, id}`，命令流只记偏移 | `Handle.RoundTripStable` + 命令流序列化往返 |
| **M4** 稳定驻留 | 销毁 = 子块归还 + O(1) 合并；页空置 N 帧才退 VMA | `Allocator.PageReuseAfterFree` / `Allocator.MergeAdjacent` |
| **G4** 内存路径唯一 | backend 外零裸分配；backend 内只剩 VMA 注入点 | `check_no_raw_alloc.py` 口径收紧（见 §5） |
| **G1** 无几何着色器 | `ShaderStage` 枚举**只有** `Vertex/Fragment/Compute` | 门控 + `Pipeline.RejectsGeometryStage`（编译期不可能构造） |
| **R2** 管线无 geometry 字段 | `PipelineDesc` 无 geometry stage 字段 | 结构性：头文件里根本没有该字段 |

## 1. 关键事实核对（开工前实测）

| 断言 | 实测 |
|---|---|
| L0 基线全绿 | ✅ `ctest` **32/32**（0 失败 0 跳过） |
| 池内 VMA 可用 | ✅ `third_party/_install/vma-v3.5.0/{release,debug}/include/vk_mem_alloc.h`，`VMA_VERSION == (3,5,0)` |
| VMA 是否需 `VMA_IMPLEMENTATION` | 需在**恰好一个 TU** 定义；本轮放 `src/backend/vulkan/vulkan_allocator.cpp` |
| L0 裸分配位置 | ✅ 9 处，**全部在 `vulkan_target.cpp`**（ARCHITECTURE §7 已写明） |
| `deps.yaml` | `use: [googletest]` — **不含 vma**，因为 vma 是纯头文件库，由 §12 的池改动单独提供；本轮改为 `use: [googletest, vma]` 显式声明 |
| 现有门控 G4 口径 | 「backend 外零裸分配 + backend 内允许落在 `// VMA-injection` 块」——本轮 L1 完成后**不应再有裸分配**，口径要收紧 |

## 2. 产物布局（只列新增/修改）

```
MagicBlocks/
  include/magicblocks/resources/          # ← 新增：L1 公共面
    handles.h        ResourceId{page_id, offset, size, id} / MemoryKind / PageKind
    memory.h         IMemoryAllocator + 分配栈对象 / AllocatorStats / PageDesc
    buffer.h         IBuffer / BufferDesc / BufferUsage
    texture.h        ITexture / ISampler / TextureDesc / SamplerDesc
    pipeline.h       IPipeline / PipelineDesc / ShaderStage{Vertex,Fragment,Compute} / DepthState / BlendState
    command.h        ICommandList / RenderPassDesc / DrawArgs
  include/magicblocks/device/
    render_device.h  ← 改：加 memory_allocator() / create_buffer / create_texture /
                       create_sampler / create_pipeline / create_command_list
    types.h          ← 改：DeviceCaps 加 page_sizes / memory_type 相关能力（不重名）
  src/backend/vulkan/
    vulkan_allocator.h/.cpp   # ← 新增：VMA + 分页 + 桶内空闲链表（M1-M6）
    vulkan_buffer.cpp         # ← 新增
    vulkan_texture.cpp        # ← 新增：ITexture + ISampler
    vulkan_pipeline.cpp       # ← 新增
    vulkan_command.cpp        # ← 新增：ICommandList 录制 + 提交
    vulkan_target.cpp         # ← 改：9 处 VMA-injection 换成 VMA
  tests/magicblocks_test.cpp              # ← 改：加 L1 用例
  CMakeLists.txt                          # ← 改：加 vma include 路径与源文件
  vma_shim/                               # ← 新增（若需要）：VMA_IMPLEMENTATION 编译单元
tools/
  check_no_raw_alloc.py                   # ← 改：口径改为「全仓零裸分配」
```

## 3. L1 功能点 → 接口 → 用例（唯一事实来源）

| 功能点 | 接口 | 实现 | 用例 |
|---|---|---|---|
| **R1** `IBuffer` | `buffer.h`：`BufferDesc{usage, size, lifetime}`；`IBuffer::desc/id/mapped()` | `vulkan_buffer.cpp` | `Buffer.CreateAndDestroy` / `Buffer.HandleIsOffset` |
| **R1** `ITexture`/`ISampler` | `texture.h`：`TextureDesc{format,w,h,mips,usage}`、`SamplerDesc{min/magFilter,wrapU/V}` | `vulkan_texture.cpp` | `Texture.CreateAndDestroy` / `Texture.ReadbackRgba8` |
| **R2** `IPipeline` | `pipeline.h`：`PipelineDesc{vertexLayout, stages(Vertex/Fragment/Compute), blend, depthStencil, raster(cullMode, depthBias, **无 wireframe 多边形模式**), msaa, primitive(Rect/Line)}`；**无 geometry stage 字段** | `vulkan_pipeline.cpp` | `Pipeline.CreateGraphics` / `Pipeline.DepthStateDefaults` / `Pipeline.RejectsEmptyStages` |
| **R3** `ICommandList` | `command.h`：`bind_pipeline/bind_vertex_buffer/bind_index_buffer/draw/draw_indexed/draw_instanced/push_constants/set_viewport/set_scissor/begin_pass/end_pass` | `vulkan_command.cpp` | `Command.RecordsAllOps` / `Command.SubmitExecutes` |
| **R4** 描述符策略 | push constant 优先（`max_push_constant_size` 实测 ≥128）+ 少量 UBO 页 + 一张动态色表纹理；**本轮不建描述符集** | `vulkan_pipeline.cpp` | `Pipeline.UsesPushConstantsOnly` |
| **R5** 上传队列 | staging 缓冲 → GPU 副本，按帧批处理；上传也走分页分配器 | `vulkan_allocator.cpp` 的 `MemoryKind::Staging` 桶 | `Upload.StagingGoesThroughAllocator` |
| **M1** `IMemoryAllocator` | `memory.h`；SDK **不暴露** `vkAllocateMemory` | `vulkan_allocator.cpp` | `Allocator.ImplementsPublicInterface` |
| **M2** 分页 + 偏移子分配 | 设备 64 MB / Host 8 MB 页；桶 Vertex/Index/Uniform/Texture/Staging | 同上 | `Allocator.PageSizeMatchesSpec` / `Allocator.SubAllocatesWithinPage` |
| **M3** 句柄 = 偏移 | `ResourceId{page_id, offset, size, id}` | 同上 | `Handle.PackUnpack` / `Handle.CommandStreamRecordsOffset` |
| **M4** 稳定驻留 | 归还子块 + O(1) 相邻合并；页空置 >120 帧才退 VMA | 同上 | `Allocator.MergeAdjacent` / `Allocator.PageReuseAfterFree` |
| **M5** 计数门槛 | `AllocatorStats{vma_alloc_count, vma_free_count, ...}` | 同上 | `Allocator.SteadyStateZeroVmaCalls` |
| **M6** 预算与淘汰 | `DeviceCaps.memory_budget`；超预算 `OutOfMemory`；整页 LRU | 同上 | `Allocator.OverBudgetFails` / `Allocator.PageEvictionAfterIdle` |

## 4. 内存分配设计（spec §6 的逐条落地）

### 4.1 页与桶

```
IMemoryAllocator                        ← L1 公共接口（无 Vk*）
  └─ VulkanPagingAllocator              ← 唯一实现
       ├─ DeviceMemoryPool   64 MB 页 × N   (DEVICE_LOCAL)
       └─ HostVisiblePool     8 MB 页 × N   (HOST_VISIBLE|HOST_COHERENT，MAPPED 常驻)
            └─ Bucket[kind] → 空闲链表(first-fit) → 切 {offset, size}
```

- **大块优先**：`reserve_pages(MemoryKind, n)` 在初始化/切帧时一次性向 VMA 要整页；
  单资源永不直接触发 VMA。
- **偏移切分**：`allocate(kind, size, align)` 在桶内做 first-fit；
  对齐按 `VkMemoryRequirements::alignment` 上取。
- **O(1) 合并**：空闲块按 offset 有序插入链表，释放时与左右相邻空闲块合并。
- **页回收**：页内空闲率 100% 且持续 `page_idle_frames`（默认 **120**）帧且无在途帧引用
  → 退回 VMA（`vmaDestroyMemory` 等价的 pool 销毁路径）。
- **帧边界**：`begin_frame()` 调 `vmaSetCurrentFrameIndex`，让 VMA 自己的延迟释放与引擎帧边界一致。

### 4.2 与命令模式耦合（spec §6.3）

资源句柄 `ResourceId{page_id, offset, size, id}` 是可序列化的**值**，命令流记录它而不是指针，
因此回放时同一 `page_id/offset` 指向同一片内存。`id` 作为逻辑槽位一致性校验（复现时页可能重新分配，
但 `id` 不变）。本轮只落 `ResourceId` 与其序列化，**命令流本体属 L2**。

### 4.3 预算与淘汰（M6）

`DeviceCaps.memory_budget`（L0 已实测有双来源）。分配超出预算 → `StatusCode::OutOfMemory`，
message 带「需要 / 已有 / 预算」三个数。淘汰只做**整页 LRU**，不做页内碎片整理。

## 5. 门控口径收紧（本轮修改）

`tools/check_no_raw_alloc.py` 当前允许 backend 内 `// VMA-injection` 标记块出现裸分配（为 L0→L1 预留）。
**L1 完成后该预留失效**，口径收紧为：

| 版本 | 断言 |
|---|---|
| 现在（L0） | backend 外零裸分配；backend 内裸分配须在 `// VMA-injection` 块 |
| **本轮（L1）** | **全仓零裸分配**；`// VMA-injection` 标记保留为「注入点存档」但**不再豁免任何调用**；扫描范围扩展到 `src/backend/**`，且断言 `vk_mem_alloc.h` 只在 `vulkan_allocator.cpp` 内 include（VMA 唯一注入点） |

这样 G4 从「将来会收紧」变成「现在就收紧」，且新增一条结构性断言：**VMA 只能在分配器 TU 里**。

## 6. 测试设计（测试门：0 失败 0 跳过）

L0 原有 25 例必须保持全绿（**不回退**），新增 L1 用例：

| 用例 | 断言要点 |
|---|---|
| `Allocator.PageSizeMatchesSpec` | 设备页 64 MB、Host 页 8 MB；`page_size(MemoryKind)` 与 spec §M2 一致 |
| `Allocator.SubAllocatesWithinPage` | 同桶连分配 N 个小资源 → **同一 `page_id`**，offset 单调且互不重叠 |
| `Allocator.MergeAdjacent` | 释放中间块后左右相邻空闲块合并（空闲块数减少、最大空块变大） |
| `Allocator.PageReuseAfterFree` | 全部释放后重新分配 → **复用同一页**，`vma_alloc_count` 不增加 |
| `Allocator.SteadyStateZeroVmaCalls` | 稳态跑 60 帧（无资源变化）→ **`vma_alloc_count` 与 `vma_free_count` 增量均为 0**（M5 硬断言） |
| `Allocator.OverBudgetFails` | 请求超过预算 → `OutOfMemory`，message 含预算数字 |
| `Allocator.PageEvictionAfterIdle` | 页空置超过 120 帧 → 退回 VMA（free 计数 +1），且有在途引用时**不**退回 |
| `Handle.PackUnpack` | `ResourceId` 打包成紧凑整数再解包 → 字段全等（命令流记录偏移的前提）；**且必须对分配器真实产出的 ResourceId 断言 `pack()` 返回 true**（见 rulings 裁定 3） |
| `Buffer.CreateAndDestroy` | 创建/销毁；`id()` 为 `ResourceId` 且 `size` 与请求一致 |
| `Buffer.HandleIsOffset` | 句柄的 `page_id/offset` 指向真实子块；两块不重叠 |
| `Texture.CreateAndDestroy` | RGBA8 / R8 / 带 mip；上传 → 读回一致 |
| `Sampler.CreateAndDestroy` | 创建成功；非法 filter → `InvalidArgument` |
| `Pipeline.CreateGraphics` | V/F 管线创建成功 |
| `Pipeline.RejectsEmptyStages` | 空 stages / 缺顶点着色器 → `InvalidArgument` |
| `Pipeline.DepthStateDefaults` | 默认 `depthTest=true, depthWrite=true, depthOp=Less`；可表达 spec P12 的 `depthTest=OFF + depthWrite=ON(depthConst)` |
| `Pipeline.UsesPushConstantsOnly` | 管线创建后 `descriptor_set_count() == 0`（R4） |
| `Command.RecordsAllOps` | **R3 的 11 类命令各录一次 → `command_count() == 11`**（记录层，不提交）。裁决见 `2026-11-03-magicblocks-l1-rulings.md` 裁定 1（原「10 类」是把 begin/end_pass 合并计数的笔误） |
| `Command.SubmitExecutes` | 录制「清屏 + 画全屏三角」→ 提交 → 读回像素符合预期 |
| `Upload.StagingGoesThroughAllocator` | 上传后 `AllocatorStats` 的 staging 桶有分配记录，**未**直连 VMA |
| `OffscreenUsesVmaNoRawAlloc` | L0 的离屏目标创建/读回后，`vkAllocateMemory` 在 backend 中出现 0 次（由 G4 全仓扫描 + 本用例的分配计数佐证） |

**L0 回归**：25 例必须全绿；golden 逐像素不变（L1 不改渲染逻辑，只改内存路径）。

## 7. 执行顺序

1. 公共头 `include/magicblocks/resources/*`（纯接口，零 `Vk*`）→ 编译期红。
2. `vulkan_allocator.{h,cpp}`：VMA 初始化、页保留、桶、空闲链表、merge、统计、帧边界。
3. 分配器单测先绿（M1–M6）。
4. `vulkan_buffer` / `vulkan_texture` / `vulkan_pipeline` / `vulkan_command`。
5. **替换 L0 的 9 处裸分配** → G4 收紧后重跑 → 全绿。
6. `IRenderDevice` 增加 `memory_allocator()/create_*` 入口 → L0 用例回归。
7. 门控脚本收紧 + 自测更新（`tools/tests/test_check_scripts.py` 要加「裸分配在新口径下必红」的负向样本）。
8. 文档：`MagicBlocks/README.md`、`MagicBlocks/docs/ARCHITECTURE.md` 的 L1 段落与进度表。

## 8. 影响面核对

- **新增**：`include/magicblocks/resources/*`（6 个头）、`src/backend/vulkan/vulkan_{allocator,buffer,texture,pipeline,command}.*`。
- **修改**：`CMakeLists.txt`（vma include + 新源文件）、`src/backend/vulkan/vulkan_target.cpp`（9 处裸分配换 VMA）、
  `include/magicblocks/device/render_device.h`（加 L1 工厂入口）、`tests/magicblocks_test.cpp`、
  `tools/check_no_raw_alloc.py`（口径收紧）、`tools/tests/test_check_scripts.py`（负向样本）、两份文档。
- **不触碰**：`EasyPainter/`、`third_party/*`、`tools/deps_lib/*`、`tools/build-deps.py`、swapchain/instance/frame_sync 的既有逻辑。
- **既有回归**：`tools/tests/` 141 例必须保持 0 失败 0 跳过；ctest 从 32 项增加到 32 + 新增用例数。

## 9. 验收（spec §11 的 L1 子集）

1. **接口纯度**：G1/G3/G4 零违规（G4 收紧为全仓零裸分配），G5 既有测试全绿。
2. **内存门槛**：稳态帧 VMA alloc/free = 0（`Allocator.SteadyStateZeroVmaCalls` 硬断言）；页复用与合并单测通过。
3. **零裸调用**：全仓 `vkAllocateMemory`/`vkFreeMemory`/`vkMapMemory`/`vkUnmapMemory` = 0。
4. **无 GS**：`ShaderStage` 枚举无 geometry；`PipelineDesc` 无 geometry 字段。
5. **测试门**：L0 25 例 + L1 新用例 + 既有 141 例，**0 失败 0 跳过**。
6. **零回退**：golden 不变（L1 不渲染新东西，只换内存路径）。

## 10. 明确不做（本轮）

L2 场景/渲染图/命令录制回放/序列化（`CommandStream` 本体）、L3 primitives、L4 features、
描述符集与 bindless、纹理图集、MSAA 解析附件、多队列/多线程录制、GLES/Metal 后端、iOS 工程。
