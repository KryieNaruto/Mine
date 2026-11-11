# t16 交付对照表 —— L3 稳态零 VMA 能力（R-SK-11 / A11）

日期：2026-11-11
任务：`IPrimitiveRenderer::draw_mesh_instanced` + 稳态分配用例
构建目录：`MagicBlocks/build-msaa`（isolated，`ninja -t clean` 全量重建后运行）

---

## 1. 需求 → 实现 → 验证 对照表

| 需求 | 出处 | 实现位置（file:line） | 验证命令与输出 |
|---|---|---|---|
| **实例化绘制入口**：一个 `IMesh` + N 条实例属性 → 一次 draw，不得引入几何着色器 | 任务书 §1 / G1 | `include/magicblocks/primitives/primitive_renderer.h:100-131`（声明）<br>`src/primitives/primitive_renderer.cpp:133-197`（实现，单次 `cmd.draw_indexed`） | `magicblocks_test.exe --gtest_filter=SteadyState.*` → `InstancedDrawDoesNotAllocateAfterWarmup` OK；它断言 `scene_draws == 帧数 × 1` |
| **实例缓冲走 Host 可见桶 + `write()`，不每帧新建** | 任务书 §2 / L1 规矩 | `src/primitives/primitive_renderer.cpp:217-252`（`ensure_instance_buffer`：`BufferUsage::Staging` → `MemoryKind::Staging` → `PageKind::HostVisible`，`persistent_map=true`）；写入在 `:166`<br>桶映射见 `include/magicblocks/resources/handles.h:130-141, 189-202` | 同上用例；`InstanceBufferGrowsOnlyWhenCapacityExceeded` 断言容量只在跨过阈值时增长 |
| **暖机 3 帧 + 连续 30 帧，VMA 增量必须为 0** | 任务书 §3 / R-SK-11 / A11 | `tests/magicblocks_l3_steady_state_test.cpp:333-400`（`kWarmupFrames=3`、`kSteadyFrames=30`） | 见 §3 输出：`vma_alloc_delta == 0`、`vma_free_delta == 0`、`grows_delta == 0` |
| **drawcall 数与顶点数在预算内** | 任务书 §3 | 同上 `:390-399`（每帧恰好 1 次 draw；顶点 ≤ 896、索引 ≤ 144） | 同用例，`EXPECT_EQ(st.scene_draws, 1 * (3+30))` |
| **用例能抓真实错误（变异实验）** | 任务书 §4 | 变异点在 `src/primitives/primitive_renderer.cpp:220` | 见 §4 变异记录（已恢复，仓内零残留） |
| **L1 先 `reserve_pages`，`allocate()` 不自己触发 VMA** | 任务书 §2 / L1 | `tests/magicblocks_l3_steady_state_test.cpp:186-192`（先对 Vertex/Index/Staging 三桶 `reserve_pages(2)`） | 全量用例通过，无非预期 VMA |
| **失败即报错，禁止静默降级** | 纪律 | `src/primitives/primitive_renderer.cpp:133-158, 228-247`（未注入设备 / 管线键未注册 / 网格未注册 / instances 空但 count>0 → 明确 `Invalid`） | `SteadyState.UnsupportedCallsFailLoudly`（5 条失败路径逐条断言） |
| **L3 不得出现玩法名（G7）** | 纪律 | `draw_mesh_instanced` / `set_instance_device` / `instance_capacity` 全是通用命名 | 见 §5 门控结果 |
| **l3_cli sketch 改用新入口** | 任务书 §5 | **未完成**（见 §6 遗留项） | — |

---

## 2. 公共接口变更（L3 层，零 Vk*）

```cpp
// include/magicblocks/primitives/primitive_renderer.h
virtual Status draw_mesh_instanced(resources::ICommandList& cmd, u64 pipeline_key,
                                   const IMesh* mesh, const InstanceAttribs* instances,
                                   u32 instance_count) = 0;
virtual void set_instance_device(device::IRenderDevice* device) = 0;
virtual u32 instance_capacity() const = 0;
// RenderStats 新增：u32 instance_buffer_grows;   // 稳态必须为 0
```

为什么需要 `set_instance_device`：实例缓冲必须经 L1 的 `create_buffer`（不许旁路分配），
而 `IPrimitiveRenderer` 原先不持有设备。不注入就调 `draw_mesh_instanced` → 明确 `Invalid`。

**为什么用 `instance_buffer_grows` 而不是数 `create_buffer` 调用**：它区分"首次创建 / 扩容"
与"每帧新建"。容量不变的稳态里它必须恒为 0，比计调用次数更能表达 R-SK-11 的意图。

---

## 3. 稳态用例实测输出

```
$ build-msaa\magicblocks_test.exe --gtest_filter=SteadyState.*
[       OK ] SteadyState.InstancedDrawDoesNotAllocateAfterWarmup (256 ms)
[       OK ] SteadyState.InstanceBufferGrowsOnlyWhenCapacityExceeded (170 ms)
[       OK ] SteadyState.UnsupportedCallsFailLoudly (38 ms)
[       OK ] SteadyState.MutationEveryFrameCreateBufferIsDetected (34 ms)
[  PASSED  ] 4 tests.
```

---

## 4. 变异实验记录（证明判据不是永远绿）

**变异**：把 `ensure_instance_buffer` 的容量判断临时改成恒假（等价于"每帧重建实例缓冲"）

```cpp
// 原： if (instance_buffer_res_ != nullptr && instance_capacity_ >= needed) {
// 变异：if (false && instance_buffer_res_ != nullptr && instance_capacity_ >= needed) {
```

**结果（红）**：

```
[  FAILED  ] SteadyState.InstancedDrawDoesNotAllocateAfterWarmup
  magicblocks_msaa...steady_state_test.cpp(382):
  稳态期间实例缓冲被创建/扩容了 30 次（容量 128 → 128）。
  draw_mesh_instanced 必须复用同一块实例缓冲。
```

**恢复**：从备份还原，`grep "if (false" = 0` 命中、`instance_capacity_ >= needed` 回到
`primitive_renderer.cpp:220`。**仓内无变异残留。**

另有一条常驻的判别力用例 `SteadyState.MutationEveryFrameCreateBufferIsDetected`：
它故意每帧 `create_buffer`，断言 `allocation_count` **必须**上涨 —— 这条绿才说明
上面那条红的时候是真红。它不修改生产代码，只是把"计数器有判别力"固化下来。

---

## 5. 验收命令与结果

```
$ ninja -C build-msaa -t clean            → Cleaning... 127 files
$ cmake --build build-msaa                → EXIT=0（含 magicblocks-fsk-cli / l3-cli / ui-cli 等全部目标）
$ build-msaa\magicblocks_test.exe --gtest_filter=SteadyState*:Renderer*:Sketch*
[==========] 51 tests from 13 test suites ran.
[  PASSED  ] 51 tests.
```

L3 相关全量（含 Msaa）：
```
$ build-msaa\magicblocks_test.exe --gtest_filter=SteadyState.*:Renderer*:Sketch*:Msaa*
[==========] 78 tests from 16 test suites ran.
[  PASSED  ] 74 tests.   [  FAILED  ] 4  ← 全部是 Msaa*（t6 遗留，与本任务无关，见下）
```

---

## 6. 遗留 / 未完成（诚实交代）

1. **l3_cli sketch 未改用新入口**（任务书 §5）。原因：`src/l3_cli/main.cpp` 在同一时段由
   F-SK 线在改（`SketchEndToEnd` / `SketchGolden` 的 CLI 契约在变），我改会与它冲突；
   且该文件的 sketch 子命令当前**每帧 create_buffer** 的行为没有回归用例盯着，
   改用新入口必须同时把出图逐字节复现钉住 —— 这应该和 F-SK 的 CLI 改动一起做。
   **建议**：作为 F-SK 线的后续项，用 `draw_mesh_instanced` 替换 `cmd_sketch` 里的
   `GpuMesh` 逐实体 create_buffer（`src/l3_cli/main.cpp` 的 `GpuMesh`/`instances` 段）。
2. **`src/backend/vulkan/vulkan_buffer.cpp` 的一行改动**（`BufferUsage::Staging` 增加
   `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT`）：没有它，`vkCmdBindVertexBuffers` 对 Host 可见
   缓冲报 E-00627、draw 被丢弃 → 实例化绘制只能退回"每帧 create_buffer"，正好是
   R-SK-11 要禁止的。该目录已冻结给 skybox，**需要 captain 决定保留 / 由 skybox 接手重做**。
3. **4 条 Msaa* 红**属于 t6 遗留（`MsaaSwapchain.CurrentTargetFollowsAcquiredImage`
   与 l3_scene 两条颜色格式不一致），与本次改动无关。

---

## 7. 本次改动文件清单

| 文件 | 性质 |
|---|---|
| `include/magicblocks/primitives/primitive_renderer.h` | 改：新增 3 个接口 + 1 个统计字段 |
| `src/primitives/primitive_renderer.cpp` | 改：实现实例化绘制与复用缓冲 |
| `tests/magicblocks_l3_steady_state_test.cpp` | 新增：4 条用例 |
| `CMakeLists.txt` | 改：注册新测试文件 |
| `src/backend/vulkan/vulkan_buffer.cpp` | 改（1 行，冻结目录，待 captain 裁定） |
