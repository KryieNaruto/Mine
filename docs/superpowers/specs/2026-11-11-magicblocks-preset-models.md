# 预设测试模型与资源接入

- 日期：2026-11-11
- 任务：t11（预设测试模型 + 资源接入：OBJ 模型进左侧列表并可渲染）
- 依赖：[t8 资源清单与采样策略](2026-11-11-magicblocks-ui-assets-inventory.md)
- 范围：L3 `primitives/model_catalog`（能力）+ `tools/`（资源获取与像素断言）+ `src/model_cli`（出图工具）。
  **不改 L0/L1/L2，不改 UI 层**——UI 只拿到一串模型名。

---

## 0. 一页纸结论

| 议题 | 结论 |
|---|---|
| 用哪几个模型 | `models/{bunny,cube,panel,sphere,teapot}.obj` 五个，全部实测可加载可上传 |
| 资源怎么进仓 | **二进制不入 git**；`assets/assets.lock.json`（1 KB，路径+字节数+sha256）+ `tools/fetch-assets.ps1` |
| 运行时怎么找资源 | `--asset-root DIR` 或环境变量 `MB_ASSET_ROOT`；**找不到就 exit 2**，不降级 |
| 模型名怎么给 UI | `list_available_models(dir)` / CLI `--list`（输出 JSON）；UI 不认识模型名 |
| 尺寸归一化 | 按**包围盒对角线**缩放到 `target_diagonal`（默认 1.0）并居中 |
| 列表失败的模型 | **仍出现在列表里**，带 `ok=false` + 原因；不静默跳过 |

---

## 1. 模型实测（本机，Release）

`magicblocks_model_probe`（一次性探针，未入库）对 `res/models` 的实测：

| 模型 | 文件字节 | 三角形 | 输出顶点 | 索引 | 文件 v / vn / vt | 缺法线重算 | 加载耗时 | 包围盒 min → max |
|---|---|---|---|---|---|---|---|---|
| `cube`   |    717 |    12 |    24 |    36 | 8 / 6 / 0     | 否 | 0.2 ms | (-0.0833,-0.0833,-0.0833) → (0.0833,0.0833,0.0833) |
| `panel`  |    350 |     2 |     6 |     6 | 4 / 1 / 0     | 否 | 0.1 ms | (-0.5000,-0.5000,0.0000) → (0.5000,0.5000,0.0000) |
| `sphere` | 170493 |  2208 |  4416 |  6624 | 1106 / 2203 / 0 | 否 | 2.6 ms | (-0.0500,-0.0500,-0.0500) → (0.0500,0.0500,0.0500) |
| `bunny`  | 417256 |  4968 | 14186 | 14904 | 2503 / 5587 / 0 | 否 | 6.8 ms | (-0.0717,-0.0768,-0.0717) → (0.0717,0.0768,0.0717) |
| `teapot` | 463325 |  6320 | 12958 | 18960 | 3644 / 5638 / 0 | 否 | 7.6 ms | (-0.0989,0.0000,-0.0659) → (0.1132,0.1039,0.0659) |

**三条要记住的**：

1. **包围盒差 1~2 个数量级**：`panel` 对角线 1.414，`sphere` 只有 0.173（8 倍差）。
   不归一化同屏陈列就是"一块板 + 一个像素点"。→ 这就是归一化的存在理由。
2. **五个文件的 `vn` 都齐全**（`normals_synthesized=false`），法线来自文件，不走重算分支。
3. **输出顶点数 ≠ 文件顶点数**（bunny 2503 → 14186，×5.7）：非平滑面跨面不复用顶点
   （`mesh_loader.cpp` 的既定行为）。算 GPU 内存要按**输出**顶点算：
   总约 1.7 MB（31,590 顶点 × 48 B + 40,530 索引 × 4 B）。

> 模型里没有 `vt`，因此没有 UV。本轮只做纯色渲染，不涉及贴图。

---

## 2. 资源落地策略（按 t8 结论执行）

### 2.1 git 里放什么

```
MagicBlocks/assets/.gitignore         # 忽略 res/ 与 *.obj
MagicBlocks/assets/assets.lock.json   # 1 KB：路径 + 字节数 + sha256（唯一的"资源是否就位"判据）
MagicBlocks/tools/fetch-assets.ps1    # 从源复制并按清单校验
```

**不放任何二进制**：res 全量 205 MB、六面图 168 MB，进 git 不可行。

### 2.2 怎么把资源弄进来

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File MagicBlocks/tools/fetch-assets.ps1
# 或指定源/目标
powershell ... -File MagicBlocks/tools/fetch-assets.ps1 -Source D:\other\res -Dest MagicBlocks\assets\res
```

行为：
- 默认源 = 清单里的 `source_root`（`E:\workspace\myspace\RayTracing\_install\bin\res`），可用 `-Source` 或 `MB_ASSET_SRC` 覆盖；
- 默认目标 = `MagicBlocks/assets/res`；
- **复制不移动**（源是别的工程的安装产物）；
- **幂等**：目标已存在且 sha256 通过就跳过复制；
- 每个文件校验 `字节数 + sha256`，不符打印 `expected / actual` 并 exit 1；
- 源目录不存在 → exit 2。

实测：首次 5/5 `OK`，二次运行同样 5/5 `OK` 且未重拷。

### 2.3 运行时怎么找到资源根

两条路，**语义相同**：

1. **CLI 参数**：`--asset-root DIR`（优先）
2. **环境变量**：`MB_ASSET_ROOT`（缺省）

`DIR` 是含 `models/` 的目录，即 `res` 本身。工具自己拼 `DIR + "/models"`。

**找不到目录时**：`scan_model_dir` 返回 `IoError` + 完整路径，CLI **exit 2**。
**不会**返回空列表，也**不会**退化成"画个空场景"——空列表与"目录没找到"是不同的事。

```powershell
# 用 RayTracing 原始 res
$env:MB_ASSET_ROOT = "E:\workspace\myspace\RayTracing\_install\bin\res"
# 或用仓库内 fetch 下来的副本
$env:MB_ASSET_ROOT = "E:\workspace\myspace\Mine\MagicBlocks\assets\res"
magicblocks-model-cli --asset-root "$env:MB_ASSET_ROOT" --output-dir out
```

---

## 3. L3 接口（`include/magicblocks/primitives/model_catalog.h`）

### 3.1 扫描

```cpp
Result<std::vector<ModelEntry>> scan_model_dir(const std::string& dir,
                                               const ModelScanOptions& = {});

Result<std::vector<std::string>> list_available_models(const std::string& dir,
                                                       const ModelScanOptions& = {});
```

`ModelEntry = { name, path, bytes, ok, message }`。

**三条契约**：

| 场景 | 行为 |
|---|---|
| 目录不存在 / 不是目录 | `IoError`，message 带路径 |
| 目录存在但空 | 返回**空列表**（不是错误） |
| 某个文件不可用（空文件 / 超 `max_file_bytes`） | **仍在列表里**，`ok=false` + 原因 |

`.obj` 是唯一默认扩展名（引擎只有 OBJ 加载器，列 `.mtl` 只会让列表出现点不动的条目）。
结果按名字排序——列表每次刷新顺序一致，golden 截图才可复现。

### 3.2 加载与归一化

```cpp
ModelLoadResult load_model(const std::string& path, IMeshLoader& loader,
                           const ModelNormalize& = {}, const MeshLoadOptions& = {});
```

`ModelLoadResult = { data, stats, source_bounds, applied_scale, error, message }`。

- `error` **原样来自** `IMeshLoader`（`kFileNotFound` / `kParseFailed` / `kEmptyGeometry` / ...）；
- `source_bounds` 是**归一化前**的包围盒（对齐/诊断用）；
- `applied_scale` 是实际用的缩放系数（关掉归一化时恒 1.0）。

**归一化口径只有一条**：`scale = target_diagonal / length(bounds.max - bounds.min)`，
再把包围盒中心平移到原点。不做 Y-up 转换、不改单位。

> ⚠️ **一个已修掉的坑**：`IMeshLoader` 的 `compute_bounds()` 只填 `min/max`，
> **从不把 `Box::empty` 置 false**。所以 loader 产出的网格 `bounds.empty` 恒为 `true`。
> 早先归一化里判 `bounds.empty` 就整段跳过（实测 `applied_scale` 恒 1.0，五个模型尺寸差照旧）。
> 现在 `normalize_in_place` 按顶点重算一份真实包围盒并补写 `data.bounds`，
> 用例 `ModelCatalog.LoadNormalizesByBoundingBoxDiagonal` 守着这条。

### 3.3 上传

```cpp
Result<std::unique_ptr<ILoadedModel>> load_and_upload_model(device, path, name,
                                                            const ModelNormalize& = {}, ...);
Result<std::unique_ptr<ILoadedModel>> upload_model_from_text(device, obj_text, name, ...);
```

`ILoadedModel = { name(), mesh(), cpu_data(), load(), gpu_bytes() }`。

- 缓冲走 **Staging 桶**（HostVisible、CPU 可映射）：`create_mesh()` 内部用
  `IBuffer::write()` 往缓冲写字，而 `Vertex`/`Index` 桶是设备页、`write()` 返回
  `Unsupported`。Vulkan 不区分缓冲的"用途"，只区分字节内容。
- 任何一步失败**不产出对象**（不留半成品网格）；
- 没预取页时返回 `OutOfMemory` 并带"需要/已有/预算"三个数。

---

## 4. 出图验证

### 4.1 工具

```powershell
# 只列名字（结构：{"models": ["bunny","cube",...]}）—— UI 左侧列表的数据源
magicblocks-model-cli --asset-root "$env:MB_ASSET_ROOT" --list

# 每个模型单独一张 PNG
magicblocks-model-cli --asset-root "$env:MB_ASSET_ROOT" --output-dir out
```

`magicblocks-model-cli` **与 t7 的 UI 离屏路径无关**：不经过 `ui::UiApp` /
`UiOverlayPass`，直接用 `model_catalog` + `l3_scene` 实例化管线出图。

取景框按模型**自身**归一化后的包围盒半径反推：
`dist = (radius / tan(fov/2)) * 1.15`，固定三分之四视角方向 → 不同模型构图可比。

### 4.2 像素断言

```powershell
python MagicBlocks/tools/verify_model_renders.py ^
    --cli build-skybox/magicblocks-model-cli.exe ^
    --asset-root "$env:MB_ASSET_ROOT" ^
    --work-dir build-skybox/models_verify
```

断言三条（阈值都是**结构性**的，不是调出来的）：

| 断言 | 阈值 | 抓什么 |
|---|---|---|
| `r0_nonbg_ratio` | `> 0.03` | 模型真的画出来了（不是空图/纯背景） |
| `color_top_share` | `< 0.98` | 不是纯色填充 |
| `r0_colors` | `>= 2` | 有内容（cube 6 色是合法的） |

阈值来源：实测最小值 `teapot 0.0770` 取一半 ≈ 0.03，留驱动/相机微调余量。

### 4.3 实测结果（480×360，5 个模型）

| 模型 | `fg_ratio` | `top_color_share` | `colors` |
|---|---|---|---|
| bunny  | 0.0951 | 0.9049 | 456 |
| cube   | 0.2317 | 0.7683 |   6 |
| panel  | 0.1947 | 0.8053 |   2 |
| sphere | 0.1172 | 0.8828 | 216 |
| teapot | 0.0770 | 0.9230 | 425 |

**确定性**：同一参数连跑两次，5 张 PNG 的 SHA256 逐字节相同。

---

## 5. 失败场景覆盖（用例 `tests/magicblocks_l3_model_catalog_test.cpp`，14 条）

| 场景 | 用例 | 期望 |
|---|---|---|
| 目录不存在 | `MissingDirectoryIsAnErrorNotAnEmptyList` | `IoError` |
| 空目录 | `EmptyDirectoryYieldsEmptyList` | 空列表、成功 |
| 扩展名过滤 / 排序 | `ScansRealModelsSortedAndFilteredByExtension` | 只列 `.obj`、按名排序、`.mtl` 不出现 |
| 坏条目留在列表 | `BrokenEntriesStayInTheListWithAReason` | `ok=false` + message |
| 超大文件 | `OversizedFileIsFlaggedButStillListed` | `ok=false`，message 含"过大" |
| 归一化 | `LoadNormalizesByBoundingBoxDiagonal` | 五个模型输出对角线都 = 1.0、中心在原点 |
| 关掉归一化 | `DisabledNormalizationKeepsOriginalGeometry` | `applied_scale == 1.0`，对角线等于原始 |
| 加载错误透传 | `LoadPropagatesLoaderErrorsVerbatim` | 缺失 → `kFileNotFound` 且带路径 |
| 上传可用性 | `UploadProducesUsableMeshAndBuffers` | 24 顶点 / 36 索引 / 缓冲无残留 |
| 上传失败 | `UploadFailsLoudlyForMissingAndBrokenFiles` | `IoError` / `InvalidArgument`，不留对象 |
| 未预取页 | `UploadWithoutReservedPagesReportsOutOfMemory` | `OutOfMemory` |
| 内存文本路径 | `UploadFromTextWorksWithoutFilesystem` | 不碰磁盘也能归一化+上传 |
| 全量 | `AllRealModelsLoadAndUpload` | 三角形数 = 12/2/2208/4968/6320 |

实测：**14/14 通过**。

---

## 6. 给 UI 层的接入方式（t11 只提供数据，不做 UI）

```cpp
// CLI / 壳层：
auto names = mbp::list_available_models(asset_root + "/models");
// → 把 names 转成 UI 的条目数据

// UI 层：只显示字符串，不认识 bunny/teapot 是什么。
```

**UI 层不认识模型名**——这是刻意的分层：模型清单是**数据**，不是 UI 逻辑。
`src/` 下的 UI 代码里不出现任何模型名。

---

## 7. 局限与未做

1. **不做 mipmap / 贴图**：五个模型没有 `vt`，本轮纯色渲染。
2. **不做 LOD / 实例化批量**：每次 `load_and_upload_model` 一个模型一份缓冲。
   左侧列表最多几十个模型，量级可接受。
3. **`scan_model_dir` 不递归**：只扫给定目录一层。子目录模型需要调用方自己遍历。
4. **`teapot.mtl` 被忽略**：`IMeshLoader` 本来就不读 mtl（`mesh_loader.cpp` 明文）。
