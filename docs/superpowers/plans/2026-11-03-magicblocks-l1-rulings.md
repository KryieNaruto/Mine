# L1 公共头裁定（Lead 对 task-1 三个待决点的裁决）

日期：2026-11-03 · 针对 `MagicBlocks/include/magicblocks/resources/*`

## 裁定 1：命令条数口径 = **11**，用例名改为 `Command.RecordsAllOps`

**结论：同意 l1-headers 的实现（11 个方法），计划文档与用例名对齐到 11。**

依据：spec §4 R3 逐条枚举为 `bindPipeline / bindVertexBuffer / bindIndexBuffer / draw /
drawIndexed / drawInstanced / pushConstants / setViewport / setScissor / beginPass / endPass`
= **11 项**。`plans-l1 §6` 的用例名 `Command.RecordsAllOps` 里写的「10 类」是把
`begin_pass`/`end_pass` 当成一对合并计数，属于笔误。

- 计划文档 §6 该行改为：「10 类命令各录一次 → 命令条数 == 10」→「**R3 的 11 类命令各录一次 → `command_count() == 11`**」
- gtest 断言按 **11** 写。
- 额外加一条断言：**11 类命令的枚举全覆盖**（对 `ICommandList` 的每个虚方法各调一次，缺一个计数就对不上），
  这样"未来有人加便捷命令"会让用例立刻红，而不是悄悄漂移。

## 裁定 2：Texture / Staging 归 `HostVisible` 页 —— **全部采纳**

**结论：采纳，Layout 不变（`page_kind_of(Texture) == HostVisible`）。**

理由：
1. **R5 明文要求**：staging 缓冲在 CPU 侧写、GPU 侧读，必须 HOST_VISIBLE。Staging 归 HostVisible 无争议。
2. **R1 读回路径**：`ITexture::readback_rgba8` 与 L0 的 `IRenderTarget::readback_rgba8` 同口径，
   纹理需要 CPU 读回能力 → HostVisible 页 MAPPED 常驻使这条路径恒可用。
3. **本轮范围**：L1 的纹理是**采样/上传/读回**用的资源，真正的渲染附件（颜色/深度）仍走 L0 的
   `OffscreenTarget`（`vulkan_target.cpp` 的 `create_image`，DEVICE_LOCAL）。
   spec 并没有要求 L1 的 `ITexture` 替换掉 L0 的渲染附件路径——那是 L2 渲染图的事。
4. **不新增"纹理走设备页"的分支**：那会让 `ITexture::allocation_kind()` 的语义随 usage 变化，
   变成调用方无法预测的行为。保持"纹理一律 HostVisible"是**可预测且可测**的。

**遗留约束（写进 ARCHITECTURE 的 L1 段）**：L1 的 `ITexture` **不是**渲染附件；
渲染附件继续走 `IRenderTarget`。L2 启用离屏渲染图时，若要把附件纳入 L1 资源体系，
需要引入 `PageKind::DeviceTexture` 或类似的第三页类，**届时同步改 `page_kind_of()` 与 `pack()`**。

## 裁定 3：`pack()` 的 64 KiB 粒度 —— **可行，但必须由后端保证对齐**

**结论：Layout 保留，但把"对齐"从"约定"升级为"backend 强制保证"。**

已核对算术（无冲突）：
- 设备页 **64 MB / 64 KiB = 1024 单位**，16 位足以表达（上限 4 GiB），**留 64× 余量**
- Host 页 **8 MB / 64 KiB = 128 单位**，同样远在 16 位内
- 两个页尺寸都是 64 KiB 的整数倍 → 页内 offset 以 64 KiB 为粒度切分是可行的

**强制的落地方式**（写入 task-2 的验收项）：
- `VulkanPagingAllocator::allocate()` **对每个桶强制 64 KiB（`ResourceId::kPackGranularity`）
  粒度的 offset 对齐**，即调用方要的 align 与 64 KiB 取大：`align = max(requested_align, kPackGranularity)`。
  这样任何成功返回的 `ResourceId` 都满足 `offset % 64 KiB == 0`。
- 代价：8 MB Host 页在 64 KiB 粒度下只有 128 个槽位，**小资源会浪费**。
  这是**已知且有意的取舍**：本轮数量级（纹理/staging 各几十个）完全够用；
  若将来 Host 页槽位成为瓶颈，正确的解法是**改 pack 布局把 offset 粒度降到 1 KiB 并把页号降到 8 位**，
  而不是放松对齐——**放松对齐会让 `pack()` 在运行期静默失败**，那正是 spec §6.3 要避免的陷阱。
- **加一条单测**：`Handle.PackUnpack` 必须对**分配器真实产出的**若干 `ResourceId` 断言
  `pack()` 成功（返回 true），而不只是构造字面量测往返。否则"分配器对齐"与"pack 要求"
  可能各自自洽、合起来却失败。

## 附：本轮不改的（明确的非目标）

- `ResourceId` 的 `size` 不参与打包（解包后为 0，靠资源表按 id 反查）——**接受**。
  命令流每多带一个 u64 换取"资源表某个 id 缺失就不再是自洽的"这个失败模式，不划算。
- `CompareOp` 放 `handles.h`（pipeline 与 texture 共用）——**接受**，避免两头互 include 成环。
- `TextureUsage::TransferSrc → MemoryKind::Staging`：这意味着"上传源纹理"落在 staging 桶。
  **接受**，与裁定 2 一致。
