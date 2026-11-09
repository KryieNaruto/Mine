# 给 L1 会话：构建阻塞状态更新（2026-11-09 16:3x）

> 来源：Lead。
> **你的 `vulkan_command.cpp` 已经编译通过了**，感谢。
> 目前只剩两处桩实现，**已授权 MSAA 侧补，你不用管**。下面说清楚哪些是你的、哪些不是。

---

## 1. 已解决（你做的）

```
vulkan_command.h:174  Status remember_texture_sampler(...)   ✅ 已声明
vulkan_command.h:232  std::vector<std::pair<ResourceId, ITexture*>> textures_;   ✅
vulkan_command.h:233  std::vector<std::pair<ResourceId, ISampler*>> samplers_;   ✅
```

`src/backend/vulkan/vulkan_command.cpp` 现在**编译通过、零错误**。

---

## 2. 剩余两处 —— 已授权 MSAA 侧补，**你不用动**

```
src/scene/recorder.cpp(431):  NullCommandList  无法实例化抽象类
tests/ui_test.cpp(677,692,...): FakeCommandList 无法实例化抽象类
```

这两个是 **桩 / 测试替身**，不是生产实现：
- `NullCommandList` 是回放时的空实现
- `FakeCommandList` 是 gtest 替身

MSAA 侧只加 `bind_sampled_texture` 这一个方法的实现，不改这两个文件的任何其它内容。

> **重要**：如果你也在改这两个文件，请**以 MSAA 侧的版本为准**，不要覆盖。
> 如果你已经写好了，直接告诉 Lead，我让它撤回。

---

## 3. 你的 P0 还欠什么（这是你真正的活）

构建解锁后，这三项是 P0 验收的核心，请优先：

1. **`bind_sampled_texture` 的 Vulkan 实现是否真的能下发**
   —— `vulkan_command.cpp` 编译过 ≠ 逻辑对。要验证：
   `vkCmdBindDescriptorSets` 的参数正确、`write_sampled_texture` 写的
   `VkDescriptorImageInfo` 指向正确的 image view / sampler。

2. **命令录制回放覆盖新方法**
   —— 回放路径漏写 `bind_sampled_texture`，画面只是"没有纹理"、**不报错**。
   这是最隐蔽的一类 bug，必须有专门用例。

3. **`sampled_texture_slots == 0` 时零行为变化**
   —— 既有 3 张 golden（`golden_l0.png` / `golden_l2.png` / `golden_ui.png`）
   必须**逐字节不变**。这是 P0-1 的硬验收口径。

---

## 4. 环境提醒（会浪费你时间的坑）

- **机器上只有 VS 18**，没有 VS 2022。
  vcvars 用：
  `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`
- **编译失败过的文件，之后的增量构建结果不可信。**
  ninja 不为失败目标更新 `.ninja_deps`，`.obj` 停在旧时间戳。
  实测过：改了类成员布局后增量构建不重编，`make_unique` 用旧 `sizeof`，
  对象布局错位，`samples=3` 本该被拒绝却在析构时段错误。
  **改过公共头文件后必须先 clean 再全量重建。**
- `ctest -j 1` 串行验收（`RESOURCE_LOCK vulkan` 已加，但当前改动涉及 framebuffer，先串行）。

---

## 5. 另一个正在进行的改动：MSAA 抗锯齿

用户裁定新增，与你无关但要知悉（共享文件已划分区域）：

**裁定**：MSAA 4x、采样数跟随 `DeviceCaps.supported_msaa_levels` 可调（2/4/8）、
**默认关闭**（samples=1）、演示时 `--msaa N` 开启、自动回归继续关闭。

**MSAA 改的文件**（你改这些段时要小心）：

| 文件 | MSAA 改的部分 |
|---|---|
| `vulkan_draw.cpp` | `ms.rasterizationSamples` 不再硬编码 1；framebuffer 挂 resolve 视图 |
| `vulkan_target.h/.cpp` | `OffscreenTarget` 加 `resolve_`/`resolve_page_`、`resolve_image()`；`init()` 放开 samples 校验；`readback_rgba8` 改读 resolve 图 |
| `vulkan_resource_common.h/.cpp` | 新增全仓唯一的 `to_vk_samples()` |
| `vulkan_device_frame.cpp` | `create_pass()` 加 resolve 附件、`pass_compatible` 比较 resolve 有无 |
| `vulkan_frame_target.h` | `IFrameTargetImage` 加 `frame_resolve_image()`（默认回落 color，交换链不用改） |
| `vulkan_pipeline.cpp` 561-640 | `create_render_pass()` 加 resolve 附件；`Status ds`→`desc_status` 改名 |
| `src/l3_cli/main.cpp` | `--msaa N` 参数 |
| `CMakeLists.txt` | 测试源列表 + `magicblocks_msaa_test.cpp` |

**你的 `vulkan_pipeline.cpp` 改动（描述符集 451-537、758-781）与 MSAA 的 561-640 不重叠。**

**注意 `vulkan_target.h` 的成员布局变了** —— 你若在别处 `make_unique<OffscreenTarget>`
或按值持有它，务必 clean 重建。
