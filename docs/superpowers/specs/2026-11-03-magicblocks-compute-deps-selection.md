# 魔法功能预览 —— 计算类依赖选型（以"引入已验证的高性能库，不自研"为原则）

日期：2026-11-03
状态：用户已定原则，本文件落地为选型 + 池接入状态
关联：[渲染引擎 SDK 设计](2026-11-03-magicblocks-render-engine-sdk-design.md)、
[技术路线对比](2026-11-03-magicblocks-tech-route-comparison.md)

## 0. 原则

> **所有计算相关的，都尽可能引入经过大家验证的高性能计算库，而不是选择自研。**

同时用户已明确两条具体选择：

1. **2D 三角化**：直接引入三方库（计算方向，无需自研）。
2. **OBJ 解析**：使用 tinyobjloader。

这条原则覆盖 SDK 的 **L3 primitives**（几何 / 数学 / 空间查询）与 L4 玩法层里的
**纯计算部分**。渲染管线本身（RHI、上屏、命令流）不属于"计算库"范畴，仍按前几份
设计文档自研。

## 1. 计算任务 → 选型对照表

| # | 计算任务 | 用在哪 | 选型 | 版本 | 状态 |
|---|---|---|---|---|---|
| C-1 | **2D 轮廓带孔三角化**（挤出的上下盖） | 魔法涂鸦挤出、印章底板 | **libtess2**（GLU tessellator 重写，处理自交/多孔稳健） | `v1.0.2` | ✅ 已入池并编过 |
| C-2 | **2D 多边形布尔运算**（并/差/交、偏移） | 涂鸦笔画合并、印章外形裁切、轮廓简化 | **Clipper2**（行业事实标准，float/int64 两套坐标） | `Clipper2_2.0.1` | ✅ 已入池并编过 |
| C-3 | **OBJ 解析** | 积木"导入 OBJ" | **tinyobjloader**（header-only，`v/vt/vn/f`、多边形三角化） | `v1.0.7` | ✅ 已入池并编过 |
| C-4 | **线性代数 / 相机 / 投影 / 求交数学** | 全 SDK | **glm**（池内已有） | `1.0.1` | ✅ 已在池 |
| C-5 | **网格简化 / 顶点缓存优化 / 轮廓简化** | 挤出网格、体素面网格、导入模型 | **meshoptimizer**（v1.3） | `v1.3` | ⏳ 待接入（L3 阶段） |
| C-6 | **GPU 内存分配** | 全 SDK | **VMA**（用户指定） | `v3.5.0` | ✅ 已入池 |
| C-7 | **体素网格 / 空间索引** | 叠叠乐体素、积木放置 | 待定（见 §3，倾向引库） | — | ❓ 待定 |
| C-8 | **射线拾取**（屏幕→世界射线 + 求交） | 积木点选、叠叠乐刷子、印章落印 | 直接用 glm 的求交原语 + 自写体素 DDA（见 §3） | — | ❓ 待定 |

## 2. 本次已落地的池接入（可复用）

三库全部接入全局池 `third_party/deps.yaml` 并完成 `fetch` + `build`（release/debug 双变体）：

| 库 | tag | 构建方式 | 池产物 |
|---|---|---|---|
| tinyobjloader | `v1.0.7` | CMake 导出 `tinyobjloader` CONFIG 包 | `_install/tinyobjloader-v1.0.7/<v>/include/tiny_obj_loader.h` + `lib/tinyobjloader/cmake/` |
| Clipper2 | `Clipper2_2.0.1`（CMakeLists 在 `CPP/` 子目录） | CMake 导出 `Clipper2` CONFIG 包 | `_install/clipper2-.../<v>/include/clipper2/*` + `lib/cmake/clipper2/` |
| libtess2 | `v1.0.2`（上游**无 CMake**） | 池驱动新增 `build: source-objects`：按源文件清单编静态库 | `_install/libtess2-v1.0.2/<v>/include/tesselator.h` + `lib/tess2.lib` + `lib/cmake/tess2/tess2Config.cmake` |

### 2.1 池驱动为此新增的两个能力

1. **`cmake_subdir` / `tag: "ref/subdir"`**：部分仓库 CMakeLists 在子目录（Clipper2 在 `CPP/`）。
   清单里写成 `tag: "Clipper2_2.0.1/CPP"`，由 `manifest.split_tag()` 拆成 `ref=Clipper2_2.0.1`、
   `subdir=CPP`，`cmake_driver.configure_command()` 按子目录配置。
2. **`build: source-objects`**：给"只有 premake/Makefile、没有 CMake"的库用。
   `cmake_driver.SOURCE_OBJECT_LIBS` 里登记 `{sources, headers, lib}`，驱动直接调用
   `cl /c` 编 `.obj` → `lib /OUT:` 打包静态库 → 落 include/lib/生成 CONFIG 包。
   （libtess2 7 个 `.c` 文件，MSVC + `/MD`（release）/`/MDd`（debug）。）

## 3. 仍未定 / 待评估的计算库

| 任务 | 候选 | 说明 | 建议 |
|---|---|---|---|
| C-5 网格优化 | **meshoptimizer** `v1.3`（⭐8,777 · MIT · 2026-10-14） | 顶点缓存重排、索引压缩、顶点量化、简化 | **建议引入**（L3 MeshFactory 阶段接入） |
| C-7 体素/空间索引 | **openvdb**（重）、**voxeloo**、或自写稠密 chunk | 本项目体素是"稠密 ≤32³ chunk + 单通道值"，不是稀疏大体积 | 稠密 chunk 结构简单，**建议自写数据布局**，但**空间索引/邻域查询**若需要可引 **meshoptimizer 的网格空间**或轻量 BVH 库 |
| C-7 空间索引（射线加速） | **embree**（CPU 光线追踪，重）、**bvh**（madmann91/bvh，header-only，⭐1k+） | 积木场景模型数量大时，射线拾取可用 BVH | **建议引入 `madmann91/bvh`**（header-only，零依赖），替代手写 AABB 树 |
| C-3 追加 | **stb_image**（单头） | 若导入 OBJ 带贴图 / 印章投影位图 | 建议按需引入 |

> 原则落地方式：**凡是"有公认最优实现"的计算**（三角化、布尔运算、OBJ 解析、网格优化、
> 空间索引、字体光栅、图像编解码），一律先找库，找不到再自研；**自研仅保留在**
> "没有可复用的通用实现"的场景（例如体素 chunk 的内存布局、命令流序列化格式）。

## 4. 对接下游（实现阶段注意事项）

1. **挤出的链路**：`IFillPath` 生成 mask → 轮廓提取 → **Clipper2**（合并/清理/偏移）→
   **libtess2** 三角化（带孔）→ 上下盖 + 侧壁 → **meshoptimizer** 优化 → `Mesh`。
2. **OBJ 导入**：`tinyobjloader` 解析 → 收集 `v/vt/vn` → 三角化多边形面 → 统一成
   SDK 的顶点格式（pos/normal[/uv]）→ **meshoptimizer** 生成索引顺序。
3. **印章底板轮廓**：圆形/方形用程序化（无需库）；"任意模型的二维投影"→ **Clipper2** 求轮廓 + **libtess2** 填充。
4. **许可合规**：libtess2（SGI-B-2.0，宽松）、Clipper2（BSL-1.0）、tinyobjloader（MIT）、
   meshoptimizer（MIT）—— 均可商用，LICENSE 已随池产物落盘（`libtess2` 已拷 `LICENSE.txt`）。
5. **Android 交叉编译**：上述库均为纯 C/C++、无运行时依赖，NDK 下可直接编；
   `build: source-objects` 分支的 `cl` 需在实现 Android 构建时切换为 NDK clang
   （Android 阶段补）。

## 5. 与之前"自研"口径的差异（记录以免反复）

| 项 | 旧口径 | 新口径（用户定） |
|---|---|---|
| 2D 三角化 | 自研 ear-clipping | **libtess2** |
| OBJ 解析 | 自研或 tinyobjloader 待定 | **tinyobjloader** |
| 计算类通用原则 | 未表态 | **尽可能引入已验证的高性能计算库** |
| 轮廓布尔 | 未定 | **Clipper2** |
| 渲染管线（RHI/上屏/命令流） | 自研 | **仍自研**（不属于计算库范畴） |
