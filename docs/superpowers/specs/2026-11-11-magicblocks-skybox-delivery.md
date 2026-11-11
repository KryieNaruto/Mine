# t10 交付记录：引擎天空盒能力（六面立方体贴图 + 天空盒 pass）

日期：2026-11-11
负责人：skybox
状态：**完成**（12/12 用例绿，零 Vulkan 校验错误）

## 1. 分层落点（按裁定）

| 层 | 文件 | 职责 |
|---|---|---|
| L1 资源 | `include/magicblocks/resources/texture.h`、`src/backend/vulkan/vulkan_cubemap.*` | `ICubeMap` 六面纹理：`upload_face` / `face_uploaded` / `desc` |
| L0 设备 | `include/magicblocks/device/render_device.h` | `create_cube_map` + `IMemoryAllocator::map_image_page/unmap_image_page` |
| L3 原语 | `include/magicblocks/primitives/sky.h`、`src/primitives/sky.cpp`、`shaders/sky.vert|frag` | `ISkyPass`：`prepare()` / `record()`，“相机朝向相关的背景渲染” |
| 工具 | `src/sky_cli/main.cpp`（`magicblocks-sky-cli`） | 出图 / 开关对比 / 确定性校验 |

L0–L2 **不引用** sky（已用 grep 验证）。

## 2. 与 t8 清单的一处偏离（有意为之）

t8 §3.3 建议“不要 CubeMap 纹理，用 6 个 quad 各绑一张面纹理”，理由是
`ITexture`/`Format` 无 cube 维度。

实现改为**真正的六面立方体贴图**，原因是那一约束只存在于旧版 L1：
- Vulkan 里 `VIEW_TYPE_CUBE` 要求六层在**同一个** VkImage 内，6 个 quad 做不到正确的
  边缘过滤与 `(s,t)` 朝向连续性；
- 用 `allocate_image_page()` + `bind_image_page()`（整图页）即可，
  **不需要**改动桶式分分配器；
- 没有厂商侧的 `VK_SAMPLER_CREATE_FLAG_CUBE_COMPATIBLE_BIT`，cube 语义完全由**视图类型**决定，
  采样器 `flags` 必须为 0（`0x10` 是 `VK_SAMPLER_CREATE_IMAGE_PROCESSING_BIT_QCOM`，
  SwiftShader 会报 `UNSUPPORTED`）。

## 3. 采样策略（照 t8 结论）

- 场景：`qwantani_dusk_2_puresky_1k`
- `RGBA8_UNORM` + `Linear` + `ClampToEdge`，`mip_levels = 1`
- HDR/IBL 本轮不做

## 4. 关键约束（后来人必读）

1. **L1 分页分配器的硬契约**：`allocate()` 不会自己触发 VMA，调用方必须先
   `reserve_pages(MemoryKind, count)`。`SkyPass::prepare()` 曾漏掉这一步，
   表现为 `upload()` 返回“当前页无空位 —— 请先 reserve_pages() 预取整页”，
   连带所有 `Sky.*` 像素用例全红。现已修复（带注释）。
2. **`submit_command_list()` 拒绝零 draw**（“提交成功但没有任何 draw 被下发”）。
   `--sky off` 想得到纯清屏基线时，要补一条**被 1×1 越界 scissor 完全裁掉**的天空盒 draw；
   不能用 `set_scissor(0,0,0,0)`（拒绝零宽高）。
3. **`submit_and_wait()` 只记录、不执行**，pass 的 clear 值不会落到目标上。
4. **管线/SkyPass 必须活过 `submit_command_list()`**：命令流存的是裸指针，
   提前析构会让 `vkCmdEndRenderPass` 撞到已失效的命令缓冲（SEH 0xc0000005）。

## 5. 验证证据

### 用例（`build-skybox`，全量 clean 重建后）

```
[  PASSED  ] 12 tests.   (Sky.* 10 + SkyCubeMap.* 2)
```
`--gtest_repeat=3` 连续三轮均为 10/10 通过、0 失败。
零 `[vk-ERROR][VALIDATION]`。

### 确定性（判据 4）

| 文件 | SHA256(前 24) |
|---|---|
| `sky_on.png` | `1990820DA82965213D6B153E` |
| `sky_on_again.png`（同参数重跑） | `1990820DA82965213D6B153E` ← **逐字节相同** |
| `sky_pan.png`（`--pan 12,0,0`） | `1990820DA82965213D6B153E` ← **平移不变** |
| `sky_yaw.png`（`--yaw 40`） | `218E8D6827CE02638EBD36BC` ← **旋转才变** |

四个基本朝向 `yaw 0/90/180/270` 哈希两两不同 → 六面确实各自成像。

### 开/关差异覆盖背景（判据 5）

`png_inspect.py --diff`：
```
diff_pixels = 256000
diff_ratio  = 1.000000
diff_bbox   = x[0,639] y[0,399]  640x400
```

### 背景不是纯色（判据 6）

`png_inspect.py --region 0,0,640,400`：

| | 天空盒 on | 天空盒 off |
|---|---|---|
| 色种数 | **30246** | **1** |
| 亮度跨度 | **123**（min 132 / max 255 / mean 221.7） | 0 |

### 失败即报错（不降级成纯色清屏）

| 场景 | 退出码 | 是否出图 |
|---|---|---|
| 未设 `MB_ASSET_ROOT` / `--asset-root` | `2` | 否 |
| `--scene no_such_scene` | `2` | 否 |

错误信息带**完整预期路径**：
`create_cube_map_from_faces: 读第 0 面失败 (.../cubeMap/no_such_scene_right.png): read_png: 无法打开 ...`

## 6. 本轮踩到的两个方法论坑（比结论本身更值得记）

### 6.1 增量构建会给出「假绿」

同一份源码：

| 构建目录 | `Texture.*` / `Device.*` / `Buffer.*` |
|---|---|
| `build-qa`（他人已 clean 重建） | **全过** |
| `build-skybox`（我方增量） | **全 SEH 0xc0000005** |

`ninja -t clean` + 全量重建后两边一致正常（复跑
`Sky.*:SkyCubeMap.*:ModelCatalog.*:Texture.*:Device.*:Buffer.*` = **39 passed**）。

教训：类型/链接布局一改，增量产物即不可信。**ARCHITECTURE §10.12 的
`ninja -t clean` 是硬要求，不是建议。**
定位方法：出现「大部分用例集体 SEH」时，先用**另一个构建目录**跑同一批用例做
cross-build-dir 对照 —— 若对方正常，问题在构建状态而不在源码。

### 6.2 判断文件编码只看字节，不要看 `Get-Content` 的显示

我曾经把 `docs/superpowers/specs/2026-11-11-magicblocks-ui-assets-inventory.md`
误判为「GBK 乱码」并上报要求重存。实际它是**合法 UTF-8**：

```
bytes=27602  BOM=False  U+FFFD=0
严格解码 UTF8_VALID=TRUE  chars=19323
首行: # MagicBlocks UI 资源清单与采样策略（cubeMap / HDR / 模型）
```

「鑼冨洿」只是 PowerShell **用 GBK 控制台解码 UTF-8** 的显示假象。
**结论：该文档不需要重存，不要动它。**

正确的判定方式（只看字节 + 严格解码）：

```powershell
$b = [System.IO.File]::ReadAllBytes($f)
$enc = New-Object System.Text.UTF8Encoding($false, $true)   # $true = 严格，非法字节即抛
try { $s = $enc.GetString($b); "VALID chars=$($s.Length)" } catch { "INVALID" }
```

## 7. 门控

- **G1**：`sky.vert` / `sky.frag` 无任何几何着色器声明。
- **G3**：`src/primitives/sky.cpp` 无 `Vk*` 依赖（仅一处注释提到枚举名）。
- **G7**：L3 无玩法词（逐词边界检查通过）。
- **G8**：`sky.cpp` / `sky.h` 不 `#include` 任何 `backend/` 头。
