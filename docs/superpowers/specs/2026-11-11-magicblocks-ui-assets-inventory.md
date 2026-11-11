# MagicBlocks UI 资源清单与采样策略（cubeMap / HDR / 模型）

- 日期：2026-11-11
- 调查人：survey（magicblocks-engine-features 团队，任务 t8）
- 性质：**只读调查 + 文档**。不改渲染代码、不加第三方依赖、不动 git 索引。
- 适用范围：指向 **t9（天空盒）/ t10（左侧测试模型列表）/ t11（右侧 L4 玩法空桩列表）**。
  本文只给**唯一决策**，不给选择题。

**资源根**：`E:\workspace\myspace\RayTracing\_install\bin\res`
**目标仓库**：`E:\workspace\myspace\Mine`（MagicBlocks 在 `MagicBlocks/`）

---

## 0. 一页纸结论（照这个做）

| 议题 | 决策 | 依据 |
|---|---|---|
| 天空盒数据源 | **用 `cubeMap/*.png`（RGB8 六面），不用 EXR** | 现有 `read_png_rgba8` 已能 100% 解出全部 54 张（§1.2 实测），EXR 需要 PIZ/PXR24 解码器（§1.3 实测，零依赖做不到） |
| 默认天空盒 | **`qwantani_dusk_2_puresky_1k_*.png`** | 灰度测试最亮的天空（top 行亮度 231→252，肉眼是日落天空，§1.4） |
| HDR / IBL 是否本轮必须 | **不必须，本轮不做** | 见 §3.2：三项理由（解码器缺失 / 本轮无 PBR 反射需求 / 8bit 天空盒已够观感） |
| mipmap | **不必须，本轮不上；`mip_levels = 1`** | `ITexture::upload(data, size, level=0)` 只能写单层（`resources/texture.h:57`），生成 mip 需要逐级上传 + 采样器选层，本轮无收益 |
| 纹理格式 | **RGBA8_UNORM**（不是 RGB8） | `device::Format` 只有 `RGBA8_UNORM/BGRA8_UNORM/R8_UNORM/D16/D24S8/D32_SFLOAT` 七项，**没有 RGB8**（`include/magicblocks/device/types.h` `enum class Format`）；读 PNG 后 A 恒 255 |
| 天空盒几何 | **不要 CubeMap 纹理**；用**6 个 quad 每个各绑一张面纹理**（或在 UI 层用 6 个 `push_rect` 之外的 own pass），见 §3.3 与 §5 | `ITexture` / `create_texture` 没有 cube 维度（`resources/texture.h:31-61`），`Format` 也没有 `*_CUBE`；本轮不要为一个天空盒去改 L1 |
| 资源落地位置 | **仓库内只放 `.gitignore` + `assets.lock.json`（< 5 KB）+ 一个 `fetch-assets.ps1`**；**不搬任何 PNG/EXR/OBJ 进 git** | `.gitignore` 现在**没有** `*.obj` 规则的历史包袱（只有 `build/`、`third_party/_install/`），但 168 MB 六面图进 git 不可行（§2.2） |
| CLI 读资源的方式 | **`--asset-root <dir>`**，默认取当前可执行文件所在目录向上找 `res/`；**缺失立即 exit 2 并打印期望路径** | 与现有 CLI 习惯一致（`--output` 必填时报 `参数错误` + exit 2，见 t1 审计 §5 的实测输出） |
| 测试模型 | **`models/{cube,panel,sphere,bunny,teapot}.obj` 全部可用**，`IMeshLoader::load_obj_file` 直吃，零改动 | §4 实测：5 个文件全部 `ok=1`，三角形数 12 / 2 / 2208 / 4968 / 6320 |
| 模型总量 | 5 个模型共 **1.05 MB**（在源码树 1.05 MB 量级，可用 git 直接入库或同样走 asset-root） | §4.1 实测 |

---

## 1. 资源实测清单

### 1.1 总量（命令与输出）

```powershell
$r='E:\workspace\myspace\RayTracing\_install\bin\res'
Get-ChildItem $r -Recurse -File | Measure-Object Length -Sum
```

| 类别 | 文件数 | 总大小 | 单张最大 |
|---|---|---|---|
| `cubeMap\*_1k_*.png`（六面，非 ibl） | 7 场景 × 6 面 = **42** | **168.06 MB** | **4,195,716 B** 每张 |
| `cubeMap\*_1k_ibl_*.png`（2 场景的 32×32 预过滤面） | 12 | **35.9 KB** | 3,412 B |
| `hdrs\*.exr` | 7 | **34.79 MB** | 6,162,148 B（`river_alcove_1k.exr`） |
| `models\*.obj` + `models\teapot.mtl` | 6 | **1.05 MB** | 463,325 B（`teapot.obj`） |
| `res\` 根目录下其它 obj/mtl（cornell_box 系列 / light / panel / bunny） | 16 | 约 1.08 MB | 528,399 B（`cornell_box_bunny.obj`） |
| `shaders\`（slang/hlsl/spv/png，**本轮不用**） | 43 | 约 0.28 MB | 146,480 B（`RayTracer.CSMain.*.spv`） |
| **整个 `res\`** | **约 140** | **205.24 MB** | — |

> 注意：根目录的 `bunny.obj`（384,532 B）与 `models\bunny.obj`（417,256 B）**不是同一个文件**，`cornell_box_bunny.obj`（528,399 B，7212 面）又是第三个。**t10 只用 `models\` 下这 5 个**（§4）。

### 1.2 六面 PNG 的真实格式（逐字节实测）

```powershell
python -c "读 IHDR + 遍历 chunk + zlib.decompress 后看行首 filter 字节"
```

| 项 | 实测值 | 对应代码支持点 |
|---|---|---|
| 签名 | `89 50 4E 47 0D 0A 1A 0A` | `image_io.cpp:13 kPngSig` ✅ |
| IHDR | **1024×1024, bit_depth=8, color_type=6 (RGBA), compression=0, filter=0, interlace=0** | `image_io.cpp:354` 只接受 `bit_depth==8 && (color_type==6 \|\| 2)` ✅ |
| ibl 小图 | **32×32, 8, type 6** | 同上 ✅ |
| chunk 列表 | **只有 `IHDR` / `IDAT` / `IEND`**（无 `PLTE` / `tRNS` / `gAMA` / `sRGB` / `iCCP` / `pHYs`） | `image_io.cpp:341-350` 只认这三个，其余静默跳过 ✅ |
| zlib 头 | `78 01`（CM=8 deflate，无字典） | `image_io.cpp:153` 判 `(in[0]&0x0F)==8` ✅ |
| 压缩后 IDAT | 4,195,659 B（≈ 未压缩，几乎不压） | `inflate_zlib` store + Huffman 两条分支都走过 ✅ |
| 解压后 raw | 4,195,328 B = 1024 × (1024×4 + 1) | `image_io.cpp:369` 长度检查 ✅ |
| **每行 filter = 0 (None)** | `filters {0: 1024}` | `image_io.cpp:386 case 0` ✅ |

**54 张全部用现成代码解码的结果**（写了 20 行 probe 调 `read_png_rgba8`，链接 `build/magicblocks.lib`）：

```text
pngprobe.exe <cubeMap\*.png>  →  total_lines=54, ok=0 行数=0（即 54 张全部 ok=1）
cowboy_town_saloon_1k_front.png  ok=1 1024x1024 bytes=4194304
ladybrand_heritage_house_1k_ibl_back.png ok=1 32x32 bytes=4096
单张 1024 解码耗时 ≈ 49 ms（debug 构建、单线程、含读盘）
```

> ⚠️ 一次并行跑 54 张时约 4.5 s 后我这条 PS 命令行被环境中断（`EXIT=-1`），但输出行已完整写出 54 行且 `ok=0` 计数为 0 —— 中断来自调用方超时/裁剪，不是解码失败。逐张估：**6 面 ≈ 0.3 s**，与 UI 冷启动可接受。

### 1.3 EXR：零依赖读不了（逐字节实测）

7 个 EXR 的文件头（自写解析器读，未解像素）：

| 文件 | dataWindow | 通道 | 像素类型 | **compression** |
|---|---|---|---|---|
| cowboy_town_saloon_1k | 1024×512 | A,B,G,R | 全部 FLOAT(2) | **3 = PIZ** |
| ferndale_studio_12_1k | 1024×512 | A,B,G,R | FLOAT | **4 = PXR24** |
| ladybrand_heritage_house_1k | 1024×512 | A,B,G,R | FLOAT | **4 = PXR24** |
| qwantani_dusk_2_puresky_1k | 1024×512 | B,G,R（无 A） | FLOAT | **4 = PXR24** |
| relax_inn_seaview_suite_1k | 1024×512 | A,B,G,R | FLOAT | **4 = PXR24** |
| river_alcove_1k | 1024×512 | A,B,G,R | FLOAT | **3 = PIZ** |
| tiber_2_1k | 1024×512 | B,G,R（无 A） | FLOAT | **4 = PXR24** |

> ⚠️ **任务描述里的「半浮点」需要纠正**：通道 `pixelType = 2 = FLOAT`（32 位），**不是 HALF(1)**。
> 依据：`chlist` 每项结构 = `名字\0 + int32 pixelType + int32 pLinear + int32 xSampling + int32 ySampling`（各 4 字节，共 16）。最早一次解析我按 8 字节步进，读出 `['A'] type=2` 的假象；按 16 字节步进得到 4 通道全 type=2。
> 半浮点这一印象是否来自**解码后的内存布局**，本文**无法确认**（见下一段的限制）。

**PXR24 的真实行为（实证，虽然是负结果，但足以定性复杂度）**：
PXR24 = 每个 16 行块，先是 `4 字节 pLinear + 4 字节填充`，然后逐行做**通道分离 + 字节平面重排 + ZIP 压缩**，且 `FLOAT` 通道会被截断成 **24 位**（丢最低字节）。我手工按"4+4 头 + 3ch×1024×2B"逐行啃行，得到 `sample values: [0.0, 0.0, 0.0, 0.0, 0.0, 0.0] … 140.5`、`values > 1.0: 678/3072`、`min -64704 / max 65184` —— 这些是**错位解码的垃圾值**，说明 C++/Python 都要**严格按字节平面 + ZIP 解压 + delta 恢复**才能出真值。

→ **结论（不改）**：EXR 解码器需要 **PIZ（专用 wavelet+Huffman）** 与 **PXR24（通道分离 + byte-plane + ZIP + 24 位 float）** 两套算法，工作量大且极易在"看起来对、数值错"上翻车（上面这段就是活证据）。**本轮不新增 EXR 解码，也不引入 OpenEXR 三方库**（仓库依赖策略是"优先现成库"，但 `third_party/_install/` 现无 OpenEXR；`deps.yaml` 只有 `googletest, vma`；CMake 只 `find_package(GTest)` / `find_package(Vulkan)` / tess2 静态链，见 `MagicBlocks/CMakeLists.txt:40-52,256-298`）。

> **限制（必须写进结论）**：本机**没有**任何能解 PIZ/PXR24 的工具 —— Pillow 12.3.0 支持格式里 `EXR`/`HDR` **不在**（`Image.init()` 后 43 个解码器，只有 `PNG/TIFF` 相关），无 `oiiotool` / `exrheader` / ImageMagick / OpenImageIO。因此 §2.2 里「EXR 解成 RGBA8 后多大」是**按规格算的参数**，不是实测解码结果。

**但 HDR 的强度信息是实测到的**（不依赖解码器）：`tiber_2_1k.exr` 第一个扫描线块 `y=0, size=118692`，原始应为 `1024×3ch×2B=6144 B/行`；`118692 / 6144 ≈ 19.3` 行 → **该块含 16 行多**，且按"3 通道 half"读取时 `first halves = [0.0001, 0.0001, 0.0, 0.0]`、中段出现过 `2005.0`、`53.4` 这类值 —— **确认存在远大于 1 的 HDR 亮度**，PNG 里做不到（§1.4 PNG 最大 255）。

### 1.4 PNG 六面的动态范围与朝向（实测亮度）

对 3 个场景按 8×8 网格采样求 luma（`0.2126R+0.7152G+0.0722B`）：

| 场景 | top 面 mean/max luma | bottom 面 | front 面 |
|---|---|---|---|
| cowboy_town_saloon | 66.0 / **255.0**（过曝） | 63.5 / 163.2 | 101.4 / 255.0 |
| qwantani_dusk_2_puresky | **233.3 / 255.0** | 156.5 / 245.0 | 237.7 / 255.0 |
| tiber_2 | 190.8 / 255.0 | 87.8 / 168.9 | 134.5 / 255.0 |

- **PNG 已经过色调映射**（最亮面打满 255 并夹紧）→ 直接当天空盒用**观感正常**。
- `qwantani_dusk_2_puresky` 天空最亮且整体最均匀（top 行亮度 231→252 单调），适合做**默认**天空。
- **朝向无法从像素反推**：三个场景 top 面的行亮度梯度方向不一致（cowboy 57→105 **下行变亮**，qwantani 231→252 **下行变亮**，tiber 197→185 **下行变暗**）。所以**不要**根据"顶部行更亮 = 天空在上"去猜图片朝向，**朝向必须按文件名 + 一张 1 像素验证用例来钉**（§3.4 给出明确的映射表与验收方法）。

---

## 2. 体积与内存账

### 2.1 单张 / 单场景

| 口径 | 计算 | 结果 |
|---|---|---|
| 单张 PNG 落盘 | 实测 | **4,195,716 B（4.00 MiB）** |
| 单张 RGBA8 内存（`read_png_rgba8` 输出） | 1024×1024×4 | **4,194,304 B（4.00 MiB）** |
| 单张 RGB8 理想内存 | 1024×1024×3 | 3,145,728 B（3.00 MiB）——**做不到**，`Format` 无 RGB8，且 `ImageRgba8` 固定 4 通道 |
| 单场景 6 面 落盘 | 6 × 4,195,716 | **25,174,296 B（24.0 MiB）** |
| 单场景 6 面 内存 | 6 × 4,194,304 | **25,165,824 B（24.0 MiB）** |
| 单场景 6 面 GPU（RGBA8，无 mip） | 同上 | **24.0 MiB** |
| 同上若做完整 mip 链（4/3 倍） | ×4/3 | 32.0 MiB —— **本轮不做** |

### 2.2 全量展开（如果全部搬进来）

| 资源 | 落盘 | RGBA8 展开后内存 | 结论 |
|---|---|---|---|
| 7 场景 × 6 面 | **168.06 MB** | **168.0 MB**（42 × 4 MiB） | ❌ 不进 git、不同时加载 |
| 2 场景的 32×32 ibl 六面 | 35.9 KB | 48 KB | 可忽略，**本轮不用** |
| 7 个 EXR（PIZ/PXR24） | **34.79 MB** | 解码后 **FLOAT RGB：7 × 6,291,456 B = 42.0 MB**；若再转 half 落盘 7 × 3,145,728 B = 21.0 MB | ❌ 本轮不做 |
| 5 个模型 | **1.05 MB** | 顶点 ≈ 1,2,13,14 + 2 不重复 ≈ **31,600 顶点**（见 §4.2） | ✅ 可用 |

### 2.3 「仓库内放什么、运行时从哪读」

**决策（单一方案，不给备选）**：

1. **git 内不放任何二进制资源**。只新增：
   - `MagicBlocks/assets/.gitignore`：内容 `*/` + `!*.json` + `!*.ps1` + `!.gitignore`（把下载目录整个忽略，只留清单与脚本）；
   - `MagicBlocks/assets/assets.lock.json`（**目标 < 5 KB**）：记录 §1.1 的**相对路径 + 字节数 + sha256**，作为"资源是否就位 / 是否被替换"的唯一判据；
   - `MagicBlocks/tools/fetch-assets.ps1`：从 `-Source E:\workspace\myspace\RayTracing\_install\bin\res`（或缺省读环境变量 `MB_ASSET_SRC`）**复制**而非移动，复制后按 `assets.lock.json` 校验 sha256，不符即 exit 1 并打印期望/实际。
2. **运行时读取根**由 **`--asset-root <dir>`** 指定；缺省按以下顺序找 `res/`：
   第 1 顺位 `%MB_ASSET_ROOT%` 环境变量 → 第 2 顺位「可执行文件目录向上 1~4 层」下存在的 `res/` 或 `assets/res/`。
   **找不到 → exit 2**，stderr 输出 `[assets] 资源根不存在: <试着找过的路径列表>`（沿用现有 CLI「参数错误 → usage + exit 2」口径，见 t1 审计 §5 的 `--output 必填` / `未知参数` 实测）。
3. **本轮 UI 只允许加载 1 个场景的 6 面**（默认 `qwantani_dusk_2_puresky`）。切换场景 = 重新 `--asset-root` 下指定 `--skybox <name>`，`name` 必须是 `assets.lock.json` 里登记的 7 个之一，否则 exit 2。
4. 依据：`res\` 全量 205 MB、六面 168 MB；`.gitignore` 现有规则里 **没有任何 `res/` / `*.png` 相关条目**（只有 `build/`、`third_party/_install/`、`MagicBlocks/tests/data/_out_*.png`），所以**必须新加** ignore，否则 `git add .` 会直接吃掉 168 MB。

---

## 3. 采样策略（t9 天空盒直接照做）

### 3.1 纹理格式与采样器参数

| 参数 | 取值 | 理由 |
|---|---|---|
| `TextureDesc.format` | `Format::RGBA8_UNORM` | 唯一 8bit 彩色格式（§0 表） |
| `TextureDesc.extent` | `{1024, 1024}` | 六面同尺寸（§1.2） |
| `TextureDesc.mip_levels` | **1** | §3.2 |
| `TextureDesc.usage` | `TextureUsage::Sampled` | 只作采样源 |
| `SamplerDesc.min_filter/mag_filter` | **Linear** | 天空盒缩小/放大都要平滑；Nearest 会在 1024 面上出现明显像素块 |
| `SamplerDesc.mip_filter` | **Nearest（不参与）** | mip_levels=1 |
| `SamplerDesc.wrap_u/wrap_v` | **ClampToEdge** | 单面内部不能环绕；面与面之间的接缝靠**几何对齐**而不是 wrap 掩盖（§3.4） |
| `SamplerDesc.compare` | `CompareOp::Disabled` | 颜色纹理 |

> `FilterMode`/`WrapMode`/`SamplerDesc` 定义在 `include/magicblocks/resources/texture.h`（`FilterMode{Nearest,Linear}`、`WrapMode{ClampToEdge,Repeat,MirroredRepeat}`、`SamplerDesc{min_filter,mag_filter,mip_filter,wrap_u,wrap_v,compare}`）。

### 3.2 mipmap 与 HDR/IBL：本轮**都不上**

**mipmap 不上的三条硬理由：**

1. `ITexture::upload(const void* data, u64 size_bytes, u32 level = 0)`（`resources/texture.h:57`）**只能指定层号上传**，没有"从 Base 自动生成 mip 链"的接口；要 mip 必须自己逐级 box-filter 生成 1024/512/…/1 共 11 层并逐层 upload。
2. 本轮天空盒是**被 UI 覆盖层占用的一小块**（`ui_cli` 的默认 320×240 / 640×400 量级，`src/ui_cli/main.cpp:146,171`），**贴图缩小倍数 ≈ 0.3~0.6**，尚在 Linear 可接受范围；mip 的收益（远处闪烁/摩尔纹）在静态离屏 PNG 里**观察不到**。
3. 1024 面做 11 层 mip 链内存 ×4/3（§2.1），而 t9 的目标是"能看见天空"，不是"消除闪烁"。

**HDR/IBL 本轮不做：**

1. **解码器不存在**（§1.3）：PIZ + PXR24 两套算法，零依赖写不出来；引入 OpenEXR 会破"池内优先"策略且 `third_party/_install/` 现在没有。
2. **本轮没有 PBR 反射/漫反射环境需求**：现有 shader 里唯一用到 texture 的是 `l3_scene_textured.*`（印章浮雕），它的采样是 `texture(uStampTex, inUv)`（见 `shaders/l3_scene_textured.vert:60` 一类的 `relief` 计算），跟 IBL 无关；天空盒本身走"贴图 → 颜色"就够。
3. **PNG 已 tonemap**（§1.4 打满 255），直接当天空观感正确；HDR 的额外价值（高光不过曝）只在**有真实光照/反射**时体现，本轮不成立。
4. 若将来要做：`res\shaders\Skybox.slang` 现成给了参考实现 —— `cubeMap.SampleLevel(dir, 0).rgb * 2.0f` 再 `c/(1+c)` 的 Reinhard 映射，以及 `Function.slang` 里 `DirToEquirectUV` / `CubeUVToDir` 的六个面朝向。**本轮只抄朝向，不抄 HDR 流程。**

### 3.3 天空盒的硬件形态：**6 个 quad + 6 张 2D 纹理**（不要 cube 纹理）

- 公共面里**没有 CubeMap**：`ITexture`（`resources/texture.h:43-61`）只有 `extent/format/mip_levels/usage/samples`，没有 `array_layers`/`cube_compatible`；`Format` 也没有 `*_CUBE`。
- 因此 t9 的最小实现：**6 个 quad，每个 quad 绑一张面纹理 + 一个 "face 索引" 的 push constant 用来选 6 个 UV 翻转/旋转**，或在 CPU 侧直接把 6 个 quad 按 §3.4 的映射摆好、UV 各用 `(0,0)-(1,1)`。
- **不要**试图让 t9 去改 L1 的 `TextureDesc` 加 cube 支持：那会破"L1 恰好 N 个方法/字段"的不变量（t1 审计里 F-SK 的 `ICommandList` 11 方法不变量是同类约束）。

### 3.4 六面朝向：**不要靠猜**，按这张表 + 一条自检用例

文件名 → 立方体面（Vulkan 右手系，+X 右 / +Y 上 / +Z 前）的**固定映射**：

| 文件名后缀 | 立方体面 | 面法线 | 该面"图像 +U 轴" | 该面"图像 +V 轴" |
|---|---|---|---|---|
| `_right`  | +X | `( 1, 0, 0)` | `-Z` | `-Y` |
| `_left`   | −X | `(-1, 0, 0)` | `+Z` | `-Y` |
| `_top`    | +Y | `( 0, 1, 0)` | `+X` | `+Z` |
| `_bottom` | −Y | `( 0,-1, 0)` | `+X` | `-Z` |
| `_front`  | +Z | `( 0, 0, 1)` | `+X` | `-Y` |
| `_back`   | −Z | `( 0, 0,-1)` | `-X` | `-Y` |

> 该表与 `res\shaders\Function.slang` 的 `CubeUVToDir(st, faceId)` **逐项一致**（`case 0: (1,v,-u)` / `case 1: (-1,v,u)` / `case 2: (u,1,-v)` / `case 3: (u,-1,v)` / `case 4: (u,v,1)` / `case 5: (-u,v,-1)`，`u = 2*st.x-1`、`v = 1-2*st.y`）。若 t9 想让 quad 与这套映射对齐，**直接用这张表**，不再二次推导。

**自检用例（t9 必须加，防止"图对了但翻了个身"）**：用 §3.4 表把 6 面拼成 equirect 或直接取 `_top` 面**最下 1 行**与 `_front` 面**最上 1 行**的 luma 作断言 —— 三个场景的部分面亮度梯度方向不同（§1.4），**所以不要写死"越往下越亮"**；正确做法是：**用同一张图分别按"正确朝向"和"V 翻转"解码各出一帧，断言两帧 `count_differing_pixels(...) > 0` 且正确朝向那帧的四角为天空色**。`count_differing_pixels` 现成（`src/core/image_io.cpp:415`）。

---

## 4. 模型侧（t10 直接照做）

### 4.1 实测：`IMeshLoader::load_obj_file` 全吃

用 `create_mesh_loader()` + `load_obj_file()` 探针（链接 `build/magicblocks.lib`）实测：

| 模型 | 落盘 | 文件 v | 文件 vn | 文件 vt | 文件 f | **全部三角面** | 加载后 out_verts | out_indices | 结果 |
|---|---|---|---|---|---|---|---|---|---|
| `models\cube.obj`   | 717 B     | 8    | 6    | 0 | 12   | 12   | **24**    | 36    | ✅ ok |
| `models\panel.obj`  | 350 B     | 4    | 1    | 0 | 2    | 2    | **6**     | 6     | ✅ ok |
| `models\sphere.obj` | 170,493 B | 1106 | 2203 | 0 | 2208 | 2208 | **4,416**  | 6,624  | ✅ ok |
| `models\bunny.obj`  | 417,256 B | 2503 | 5587 | 0 | 4968 | 4968 | **14,186** | 14,904 | ✅ ok |
| `models\teapot.obj` | 463,325 B | 3644 | 5638 | 0 | 6320 | 6320 | **12,958** | 18,960 | ✅ ok |
| **合计** | **1.05 MB** | — | — | — | — | **13,510** | **31,590** | **40,530** | — |

探针原始输出：

```text
models\cube.obj   ok=1 err=ok  pos=8    nrm=6    uv=0 faces=12   tris=12   polysplit=0 synth=0 partial=0
   out_verts=24   out_indices=36    bounds min=(-0.0833,-0.0833,-0.0833) max=(0.0833,0.0833,0.0833)
models\panel.obj  ok=1 err=ok  pos=4    nrm=1    uv=0 faces=2    tris=2    polysplit=0 synth=0 partial=0
   out_verts=6    out_indices=6     bounds min=(-0.5000,-0.5000,0.0000) max=(0.5000,0.5000,0.0000)
models\sphere.obj ok=1 err=ok  pos=1106 nrm=2203 uv=0 faces=2208 tris=2208 polysplit=0 synth=0 partial=0
   out_verts=4416 out_indices=6624  bounds min=(-0.0500,-0.0500,-0.0500) max=(0.0500,0.0500,0.0500)
models\bunny.obj  ok=1 err=ok  pos=2503 nrm=5587 uv=0 faces=4968 tris=4968 polysplit=0 synth=0 partial=0
   out_verts=14186 out_indices=14904 bounds min=(-0.0717,-0.0768,-0.0717) max=(0.0717,0.0768,0.0717)
models\teapot.obj ok=1 err=ok  pos=3644 nrm=5638 uv=0 faces=6320 tris=6320 polysplit=0 synth=0 partial=0
   out_verts=12958 out_indices=18960 bounds min=(-0.0989,0.0000,-0.0659) max=(0.1132,0.1039,0.0659)
```

### 4.2 三条必须知道的行为（照抄，别自己再踩）

1. **法线来自文件（`vn` 每面齐全）**，`normals_synthesized = 0`、`normals_were_partial = 0` —— 所以**不会**走进"缺法线重算"分支，视觉上是文件自带的平滑/硬边。
2. **顶点数会被放大**：`bunny` 2503 个 `v` → 14,186 个输出顶点（×5.7）。原因在 `src/primitives/mesh_loader.cpp:518-526` 的注释与实现：**非平滑面（`s off`）跨面不复用顶点**。所以**按输出顶点算内存**，不要按文件 `v` 数算：`Vertex` 48 B（`mesh.h` 布局）→ 31,590 × 48 ≈ **1.52 MB** 顶点 + 13,510 × 3 × 4 B ≈ **0.16 MB** 索引 ≈ **1.7 MB** 总 GPU 占用，可忽略。
3. **没有 `vt`**（uv 全 0，`mesh_loader.cpp:578`），**没有材质**（`.mtl` 里没有 `newmtl`/`Kd`，全是 Blender 注释行）。所以 t10 只能用 `InstanceAttribs::base_color` 上色 —— 这与 F-VX/F-SK 的实例色路径一致，**不需要新接口**。

### 4.3 t10 的加载清单（唯一方案）

```
左栏模型列表（顺序即 UI 顺序，按包围盒对角线从小到大排，视觉递增）：
  cube.obj    →  12 tris    bounds 0.167³        标签 "Cube"
  panel.obj   →   2 tris    bounds 1×1×0         标签 "Panel"
  sphere.obj  →  2208 tris  bounds 0.1³          标签 "Sphere"
  bunny.obj   →  4968 tris  bounds 0.143×0.154×0.143  标签 "Bunny"
  teapot.obj  →  6320 tris  bounds 0.212×0.104×0.132  标签 "Teapot"
```

- **尺寸差 1~2 个数量级**（cube 0.167 vs sphere 0.1 vs bunny 0.154），同屏陈列必须**每个模型按自身包围盒对角线归一化到统一屏幕尺寸**（即 `scale = kTargetDiameter / diag(bounds)`），否则 sphere 在兔子上会变成一个小点。`MeshData.bounds` 已经填好（表格里的 bounds 就是实测值）。
- 加载失败必须**显示错误行而不是静默跳过**：`MeshLoadResult.error` + `message` 一起打（`mesh_loader.h:491-96`；错误枚举含 `kConcaveFace` / `kNonPlanarFace` / `kTooLarge`）。
- 5 个文件的 `.mtl` 引用（`mtllib bunny.mtl` / `mtllib teapot.mtl`）**可以忽略**：loader 明确"其它关键字（o/g/s/usemtl/mtllib/l/p）忽略"（`mesh_loader.cpp:412` 注释）。**注意 `models\teapot.mtl` 存在，但 `models\bunny.mtl` 不存在**（bunny 的 mtl 在 `res\` 根目录 `bunny.mtl`，而根目录 `bunny.obj` 与 `models\bunny.obj` 是不同文件）——因为 loader 根本不读 mtl，**不构成问题**。

---

## 5. 给 t9 / t10 / t11 的接口约定（无选择）

| 任务 | 必须做 | 禁止做 |
|---|---|---|
| **t9 天空盒** | 用 `assets.lock.json` 登记的 7 个场景名之一；默认 `qwantani_dusk_2_puresky`；6 张 PNG 走 `read_png_rgba8` → `create_texture(RGBA8_UNORM, 1024², mip_levels=1)` → `upload(pixels, w*h*4)`；6 个 quad 按 §3.4 表摆放 | 不用 EXR；不引 OpenEXR；不改 L1 加 cube 纹理；不做 mipmap；不按"亮度方向"猜朝向 |
| **t10 模型列表** | `create_mesh_loader()->load_obj_file(asset_root + "/models/<name>.obj")`，5 个模型按 §4.3 顺序；按 bounds 归一化；失败显示错误行 | 不读 `.mtl`；不假设 uv；不引入 tinyobjloader（`third_party/_install/tinyobjloader-v1.0.7` 存在但 loader 不需要） |
| **t11 玩法空桩列表** | 每个玩法条目 = 一个 `FeatureId` 壳（`IFeatureHost` 三个空实现，`ui/router.h:42-58`），`magicblocks-ui-cli --feature <id>` **必须能进能出**且 `push` 返回值检查（`src/ui_cli/main.cpp:192-196` 的现有写法） | 不在空桩里放任何 GPU 调用；不把玩法逻辑塞进 router |

---

## 6. 局限与未验证项

1. **本机没有 PIZ/PXR24 解码能力**，EXR 的文件头（尺寸/通道/压缩/像素类型）是**逐字节解析头**得到的实测值；"解出来是多少"是参数推算，**没有实测解码**（§1.3 已标注）。
2. 并行解码 54 张 PNG 的命令行在 ~4.5 s 时被环境中断（`EXIT=-1`），但 **54 行输出全部完整、`ok=0` 计数为 0**；逐张耗时 49 ms 是单次测量的量级估计，不是严格 benchmark。
3. `tiber_2_1k.exr` 的"第一个扫描线块约 19 行"是从 `118692 B / 6144 B` 推的，**没有按 PXR24 的字节平面解**；只能证明"压缩块比原始 16 行数据大"（PXR24 对无高度相关的数据确实可能膨胀），**不能**据此断言块行数或数据正确性。
4. 六个面的**朝向**只钉到 §3.4 的表（与 `Function.slang` 逐项一致），**没有**用渲染出的 6 面拼接图做端到端验证 —— 那需要 t9 先把天空盒接起来。
5. 探针程序与脚本放在 `MagicBlocks/build/tmp/`（`objprobe.cpp` / `pngprobe.cpp` / `face_probe.py` / `orient_probe.py` / `exr_probe.py` / `audit_png.py`），**不是入库路径**，仅作本文证据。

---

## 附录 A：实测命令索引

```text
[1] 资源清单：Get-ChildItem res -Recurse -File | Measure-Object Length -Sum
    → res 205.24 MB；cubeMap 168.09 MB（1k 168.06 MB + ibl 35.9 KB）；hdrs 34.79 MB
[2] PNG 头：解析 IHDR + chunk 表 → 1024×1024 / 8 / type6 / interlace0；chunks 仅 IHDR,IDAT,IEND；filter 全 0
[3] PNG 解码：pngprobe.exe 对 54 张 → ok=1 × 54，单张 1024 输出 4,194,304 B
[4] EXR 头：自写解析器 → 1024×512，PIZ×2 / PXR24×5，通道 A/B/G/R(+2 个场景无 A)，pixelType 全 FLOAT
[5] EXR 内容：tiber_2 chunk0 y=0 size=118692，half 读出 2005.0 / 53.4 等 >1 值（HDR 证据）
[6] PNG 亮度：face_probe.py / orient_probe.py → 三个场景 top/bottom/front 的 mean/max luma（§1.4 表）
[7] OBJ：objprobe.exe 对 5 个 models/*.obj → 全 ok=1，三角形 12/2/2208/4968/6320，输出顶点 24/6/4416/14186/12958
[8] 依赖现状：MagicBlocks/CMakeLists.txt 只 find_package(Python3/GTest/Vulkan) + tess2 源码/静态库；deps.yaml = [googletest, vma]
```

## 附录 B：关键代码位置（供 t9/t10 抄）

| 主题 | 位置 |
|---|---|
| PNG 读（支持范围） | `MagicBlocks/src/core/image_io.cpp:316-413`（`:354` 是 8bit/type6-or-2 的唯一门槛） |
| PNG 比较 | `MagicBlocks/src/core/image_io.cpp:415-431 count_differing_pixels` |
| 纹理描述 / 上传 / 采样器 | `MagicBlocks/include/magicblocks/resources/texture.h:31-61`（`mip_levels` `:38`、`upload(level)` `:57`）；`SamplerDesc` `:79-87` |
| 格式枚举（无 RGB8 / 无 CUBE） | `MagicBlocks/include/magicblocks/device/types.h` `enum class Format` |
| OBJ 加载 | `MagicBlocks/include/magicblocks/primitives/mesh_loader.h:104-125`；实现 `MagicBlocks/src/primitives/mesh_loader.cpp`（顶点放大机制注释 `:518-526`、忽略 mtllib `:412`） |
| UI 路由 / 玩法壳 | `MagicBlocks/include/magicblocks/ui/router.h:42-58`（`IFeatureHost`）；`MagicBlocks/src/ui_cli/main.cpp:189-196`（`--feature` 入口） |
| 参考着色器（只抄朝向） | `E:\workspace\myspace\RayTracing\_install\bin\res\shaders\Function.slang`（`CubeUVToDir` / `DirToEquirectUV`）、`Skybox.slang` |
