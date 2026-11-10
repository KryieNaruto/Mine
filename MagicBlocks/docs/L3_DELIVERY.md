# L3 primitives 层交付记录

> 对应 spec：`docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md`
> 交付日期：2026-11-05

## 总览

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | `IPainter` + CLI `paint` | ✅ |
| M2 | `IWorld`(ECS) + `IMeshFactory` + `IBatchBuilder` + CLI `blocks` | ✅ |
| M3 | `IOverlay2D` / `IPicker` / `IGizmoSystem` + CLI `gizmo` | ✅ |
| M4 | 扩展图元 / `FromOutline` / OBJ 加载 / BVH | ✅ |
| M5 | G6/G7/G8 门控 + L3 golden + 三态校验 | ✅ 门控与 golden 完成 |

测试基线：**361/361 ctest 全绿**（L3 相关 136 项：Camera/Math/Material/ShaderLibrary/Pool/Renderer/Mesh/Painter/Renderer2/PixelRegression/Overlay2D/Picker/Gizmo + M4 的 Loader 24 / BVH 7 / Outline 12 / Stroke 10）。

## 交付物

### 头文件（`include/magicblocks/primitives/`）

| 文件 | 内容 |
|---|---|
| `types.h` | `Vec2/Vec3/Vec4`、`Mat4`（列主序 `m[col*4+row]`）、`Box`、`Ray`、`Plane` |
| `camera.h` | `ICamera`（`world_size_per_pixel()` 是屏幕恒定尺寸的**唯一**计算点） |
| `material.h` | `StandardParams` / `MaterialDesc` / `MaterialKey` / `IMaterialTable` |
| `shader_library.h` | `ShaderKey` / `ShaderKeyFlags` / `RasterStateL3` / `IShaderLibrary` |
| `mesh.h` / `mesh_factory.h` | `Vertex`(48B) / `IMesh` / `IMeshFactory`（cube/box/quad/grid） |
| `alloc.h` | §8 对象池 + §4.1 帧预算（`IFrameArena` / `IObjectPool<T>` / `IBatchPool`） |
| `batch.h` | `IBatchBuilder`（`push`/`push_batch`/`build`，排序键 `[pipeline, material, mesh, transparent, entity]`） |
| `world.h` | archetype SOA ECS（`EntityId{index,gen}`、`ComponentKind`、`DirtySet`） |
| `world_render.h` | `build_draw_requests(world, builder, desc)` — 世界→合批的接线 |
| `painter.h` | `IPainter`（笔画 → 条带几何） |
| **`overlay2d.h`** | 屏幕空间线/虚线/矩形/环/三角（M3） |
| **`picker.h`** | 纯 CPU 射线拾取：AABB / 三角形 / 平面 / 球（M3） |
| **`gizmo.h`** | 事件总线 + 声明式轴 + 显式状态机（M3） |

### CLI（`magicblocks-l3-cli`）

```bash
magicblocks-l3-cli paint  --width 640 --height 400 --strokes 4 --output paint.png
magicblocks-l3-cli blocks --width 640 --height 400 --blocks 16 --output blocks.png
magicblocks-l3-cli gizmo  --width 640 --height 400 --output gizmo.png
```

三个子命令**都不读时钟/鼠标**，同样的参数产出逐字节相同的 PNG。

## 五条硬约束的落地情况

### 1. 性能第一（`alloc.h` / `batch.h`）

- 帧预算 `FrameBudget{kMaxHeapAllocs=0, kMaxVmaAllocs=0, kMaxDrawcalls=300, kMaxPipelineSwitches=16}`；
- `IFrameArena` 按 256 KiB 块分配，帧末整体重置（无逐对象释放）；
- 合批：16 个实体 → 4 次 draw（按材质分组），**Pipeline 切换 0 次**；
- G6 门控锁住热路径零扩容。

### 2. `Material` 与 `Shader` 的联动（`material.h` / `shader_library.h`）

- `MaterialDesc::shader` 只是 `ShaderKey`，**不含管线**；管线由
  `(ShaderKey, PrimitiveType, blend, depth, raster)` 派生并缓存；
- 材质参数**不进 push constant**（128B 移动端上限装不下每实例材质），
  走实例属性 `InstanceAttribs::base_color/roughness/metallic`；
- 同 `ShaderKey` 的材质参数差异**不拆 drawcall**。

### 3. Gizmo 事件解耦（`gizmo.h`）

四条规则写死在头文件注释里：
- R1 gizmo 不拥有世界，只发 `GizmoEvent`；改世界是订阅者的事；
- R2 轴是数据（`GizmoAxis` 数组），增删轴不改代码；
- R3 屏幕恒定尺寸只调 `ICamera::world_size_per_pixel()`；
- R4 `pick_axis()` 单点返回 axis id，阈值是像素。

验收测试 `Gizmo.AxisScreenLengthIsConstantAcrossDistances` 断言不同相机距离下
轴的**屏幕像素长度**都 ≈ 80px（误差 2px 内）。

### 4. 命令录制回放

- gizmo 的时间戳只从 `InputEvent::timestamp_us` 抄，**绝不读时钟**；
- `GizmoSnapshot` 可快照/恢复（值类型，`operator==`）；
- 测试 `Gizmo.RecordedEventStreamReplaysIdentically`：同一串输入两次喂给两个
  独立 gizmo 实例，事件序列（kind + 时间戳）完全相同；
- `tools/check_png_deterministic.py` 在 ctest 里跑两遍验证逐字节一致。

### 5. ECS 组件拼装（`world.h`）

archetype SOA：`Transform`(0) / `LocalBounds`(1) / `MeshRef`(2) / `MaterialRef`(3)。
`build_draw_requests` 只遍历匹配 `Mesh|Material|Transform` 的 archetype。
`register_component` 支持**带默认值**注册 —— `Transform` 的 scale 默认
`(1,1,1)`，铺零会变成 `(0,0,0)`，实体缩成一个点且不崩不报错。

## 踩过的坑（都已在代码注释里留痕）

### A. `upload()` 不 `flush_pending()` = 静默全零缓冲 ⚠️

`IBuffer::upload()` 只把数据 memcpy 进 Staging 桶并压栈 `pending_`，
真正拷到 GPU 要 `flush_pending()`。漏掉它：
- **没有任何错误码**；
- 顶点缓冲在 GPU 侧全零 → 所有三角形退化到原点 → 零片元；
- 清屏色仍然正确，所以图看起来"是一片纯色背景"。

这个坑让 `l3_cli` 和 `cube_demo` 都产出过空图。
现在 `l3_cli` 里每个上传点后都有硬断言 `pending_upload_bytes() == 0`。

**单测抓不到它**（单测用 `FakeBuffer`，其 `flush_pending()` 是空实现）。
这就是为什么必须有 `PixelRegression` 测试与 CLI 级 content 检查。

### B. 屏幕 Y 翻转只翻一次

像素原点左上、+y 向下；Vulkan NDC +1 对应视口**底边**，
而引擎契约要求 NDC +1 = 图像第 0 行。所以像素→NDC 必须翻一次：
`ndc.y = 1.0 - py/height*2.0`。`stroke_ribbon.vert` 曾漏翻。

### C. 门控必须做变异验证

写了才通过的门控**极可能空转**。本次实测抓到三处：

| 门控 | 缺陷 | 后果 |
|---|---|---|
| G6 | 函数名单漏了 `build` | 合批器/覆盖层三角化完全没被检查 |
| G7 | 用 `\bword\b`，而 `_` 属于 `\w` | `is_voxel_scene` 这种最典型形态抓不到 |
| G8 | 先去字符串字面量再查 include | include 路径变成 `""`，规则静默失效 |

修法分别是：补 `build` 到名单、按非字母数字分词、include 用原始行判断。
三者的变异用例（注入违规 → 必须变红）都已手工跑过。

### D. Windows 的 `max` 宏

`<windows.h>` 把 `max` 定义成宏，破坏 `std::numeric_limits<T>::max()`。
`world.h` 与 `l3_cli/main.cpp` 顶部都加了 `#ifndef NOMINMAX` 防御。

## M4：扩展图元 / FromOutline / OBJ / BVH（已完成）

spec §13 把 M4 单列为难度 ★★★★：

| # | 条目 | 状态 | 落点 |
|---|------|------|------|
| 17 | `IMeshFactory` 其余图元（Cylinder/Cone/Sphere/Prism/Trapezoid） | ✅ | `src/primitives/mesh_factory.cpp` |
| 18 | `FromOutline`（libtess2 三角化 + 轮廓合并 + 优化） | ✅ | `mesh_factory.cpp` + libtess2 |
| 19 | `IMeshLoader::obj` | ✅ | `src/primitives/mesh_loader.cpp` |
| 20 | `IPicker` 的 BVH 加速 | ✅ | `src/primitives/picker.cpp` |

### 17. 扩展图元

`cube()/box_faces()/quad()/grid()` 之外补齐：

- `cylinder(radius, height, segments, cap)` — 侧壁 + 上下盖，两套法线
- `cone(radius, height, segments, cap)` — 锥面法线按母线斜率求，非单位圆法线
- `sphere(radius, rings, segments)` — UV 球，极点收成一个顶点（不重复）
- `prism(sides, radius, height)` — **平直着色**，每面独立四顶点（棱角必须硬）
- `trapezoid(bottom, top, height)` — **平滑着色**，上下底法线沿斜面插值

外加 `cylinder_unit()/cone_unit()/sphere_unit()/prism_unit()` 四个内联便捷语法糖
（单位尺寸 + 默认分段数），省掉调用点每次手填分段数。

**绕序约定**（全部图元统一）：**从 +Z 看逆时针**为正；索引不进 `MeshData` 的
几何阶段，由 `IMesh` 上传时按 `front_face` 决定。

### 18. FromOutline

`from_outline(const OutlineSet&, const ExtrudeParams&)`：
外环 + 可选洞 → 侧壁 + 上盖 + 下盖的实体网格。

- **三角化用 libtess2**（`tess2.lib`，池里 `libtess2-v1.0.2`）
- **轮廓合并用非零环绕规则**，没有接 Clipper2。原因：libtess2 的
  `TESS_WINDING_NONZERO` 本身就实现了"重叠笔画不出现内部边"，
  实测两个 2×2 方块沿 x 偏移 0.5 合并后面积恰为 5.0（= 2.5×2 矩形），
  与 Clipper2 的 union 结果一致。Clipper2 池里虽有 `.lib`，但它的
  CMake config 是坏的（见"踩坑"），权衡后不引入。
- **优化没接 meshoptimizer**：池里没有这个包。图元本身顶点数就小
  （最复杂单位球 16×24 = 386 顶点），优化收益不抵一个新依赖的构建成本。

**绕序（硬碰硬的坑）**：libtess2 对 CCW 输入输出 CCW 三角形（实测
`cross=+4.00`）。所以 **上盖（+Z 法线）直接按 `tri[0],tri[1],tri[2]` 用**，
**下盖（−Z 法线）必须换成 `tri[0],tri[2],tri[1]`**。
最初写反了（上盖翻转、下盖不翻），12 个用例里 4 个绕序 BAD。

`stroke_outline(centerline, width, round_cap, arc_steps)` 是配套的笔画加粗：
中心线 + 线宽 → 闭合轮廓，可直接喂给 `from_outline` 出实体。
修了三个真 bug：

1. 采样用 `t = k/steps`，弧只扫 −60°..+60° 而不是 −90°..+90°，圆头被削平
   （面积 0.80 vs 理论 0.9257）。改成 `t = k/(steps−1)` 后 0.9176。
2. 端头弧和内凹弧方向反了，胶囊右端被切平（`x` 最大值恰好 2.0）。
3. 输出是顺时针（面积 −0.424），违反头文件约定的 CCW。末尾 `std::reverse` 修正。

### 19. OBJ 加载

`create_mesh_loader()->load_obj_file/load_obj_text`，手写解析器
（池里的 `tinyobjloader-v1.0.7` 只有头文件，没有导出 target，接入要手工
`target_include_directories`，不如直接写——本工程只需要 OBJ）。

支持 `v/vt/vn/f`、负索引、`l`（忽略）、`o/g`（忽略）、`s` 平滑组。
`MeshLoadError` 七种错误码：`kNone/kFileNotFound/kParseFailed/kEmptyGeometry/
kNonPlanarFace/kConcaveFace/kTooLarge`。

**平滑组的顶点合并**：`s off`（默认）走**每面独立**的
`std::unordered_map`，硬边必须分开；`s 1` 走**全局** `vmap`，同位置同法线的
顶点共享。`synthesize_normals()` 只焊接 `vertex_smooth==true` 的顶点，
硬顶点 `of[i]=i` 保持自己的面法线。

**踩坑**：最初硬边面从不插入 `vmap`，却拿 `face_local` 的迭代器和
`vmap.end()` 比，比较结果无意义——立方体返回 `verts=0` 但 `tris=12`。
改成独立的 `hit` 布尔 + 各自 map 的 `find` 结果。

### 20. BVH

`pick_scene()` 在实体数 `<= 8` 时走原来的线性扫描，超过就
`build_bvh()`（`std::nth_element` 按最长轴分，叶子 4 个）
+ `pick_bvh()`（显式 64 深 `std::uint32_t` 栈，先压右再压左）。

**正确性验证**：同一个场景，8 个盒子走线性、加第 9 个埋在下方的盒子走 BVH，
两张 200×140 图逐像素比对完全一致。

**踩坑**（两个都是"静默全错"级别）：

1. `BvhNode` 最初想用 `right − 1` 推左孩子——左子树多节点时就错了。
   改成显式存 `left`。
2. `Box{Vec3 lo, Vec3 hi}` 聚合初始化把第三个成员 `empty` 留在默认值
   `true`，于是每个内部节点的包围盒都被判为"空"，`intersect_box` 永远失败。
   `Box` 有三个成员，必须显式传 `false` 或用 `Box::of()`。
3. `PickHit::distance` 默认值是 `0.0f`，而 `valid()` 判 `distance >= 0`，
   默认构造的 `PickHit` 竟然是"命中"的。默认值改成 `−1.0f`。

### M4 新增用例

| 文件 | 用例数 | 覆盖 |
|------|--------|------|
| `tests/magicblocks_l3_loader_test.cpp` | 24 | OBJ 解析 / 法线合成 / 平滑组 / 错误码 |
| `tests/magicblocks_l3_bvh_test.cpp` | 7 | BVH 与线性一致 / 剪枝计数 / 退化场景 |
| `tests/magicblocks_l3_outline_test.cpp` | 12 | 洞 / 重叠合并 / 厚度 / 绕序 / Z 偏移 |
| `tests/magicblocks_l3_stroke_test.cpp` | 10 | 胶囊形状 / CCW / 圆头面积 / 端到端出实体 |

全量回归：**361/361 通过**。

### M4 两个环境级踩坑（不是代码 bug，但卡了很久）

1. **`find_package(GTest)` 命中了错误的包**。`third_party/_install` 里
   `clipper2` 和 `ink-stroke-modeler` 都各自打包了一份 gtest 头文件
   和 `lib/cmake/GTest`。靠 `CMAKE_PREFIX_PATH` 顺序匹配时命中了 Clipper2 那份，
   它的 config 缺 MSVC 工具链配置，编译时报
   `gtest/gtest.h(52): fatal error C1083: 无法打开包括文件: "cstddef"` ——
   行号指向第三方头，看上去像 STL 坏了，实际是 `GTest_DIR` 指错。
   **修法**：`CMakeLists.txt` 里显式 `set(_gtest_dir ...)` 指向正版池并
   `PATHS ... NO_DEFAULT_PATH`，把选择权从搜索顺序里夺回来。
2. **Python 标准库的 `.pyc` 损坏**。`tools/tests` 的 9 个模块
   `ImportError: bad marshal data`，删项目 `__pycache__` 无效——
   坏的是 `Python313\Lib\asyncio\__pycache__` 这类**标准库自身**的字节码。
   清掉 `Python313\Lib` 下 308 个 `__pycache__` 后恢复。

## 明确不做（用户裁定）

`IBlockSet` / `IBlockBrush` / `IShapeTemplate` / `IVoxelizer` / `IPlacementPolicy` / `FreeFloat`
**不在 L3 范围**。它们属总 spec §4 P7a/P7b/P7c 的玩法数据模型，由 L4
`src/features/**` 实现。L3 只提供它们要用的通用能力
（`IMesh` / `ICamera` / `IMaterial` / `IPicker` / `IOverlay2D` / `IBatchBuilder`）。

落笔走**格点吸附**（spec 的 A 方案），不做 FreeFloat。

---

# L4 F-VX 魔法叠叠乐 交付记录

> 对应设计：`docs/superpowers/specs/2026-11-09-magicblocks-l4-fvx-stack-design.md`
> 对应计划：`docs/superpowers/plans/2026-11-09-magicblocks-l4-fvx.md`
> 交付日期：2026-11-10　　分支：`l4-fvx` → 合入 `main`

## 总览

| 阶段 | 内容 | 新增用例 |
|---|---|---|
| 0 | 基线校准（主分支当时不可构建，先补齐才能开工） | 0 |
| 1 | `Cell` / `Block`(24B) / `GridSpec` / `IBlockSet` 纯 CPU 数据模型 | 15 |
| 2 | `IBlockBrush` 笔刷链（射线→吸附→DDA→去重）+ 层锁 | 19 |
| 3 | 双 pass 渲染（表面 pass + 内面 pass），**全仓首个 `with_depth=true` 公共 API 像素回归** | 6 |
| 4 | `IOverlay2D` 网格绘制 | 8 |
| 5 | 颜色模式 + `IShapeTemplate` | 14 |
| 6 | CLI `stack` 子命令 + 离屏 PNG 出图 | 10 |
| 7 | 命令模式三态校验（哈希 + 像素）+ G3/G7 门控 | 11 |
| — | D-VX-5 内面排序契约补齐 | 4 |

**测试基线：489/489 ctest 全绿，0 失败 0 跳过**（`ctest --test-dir build -j 1`）。
其中 L4 F-VX 新增 **87** 条。L0–L3 与 UI 消费层**零改动**（`git diff` 对
`include/magicblocks/{device,resources,scene,primitives}/` 与
`src/{backend,resources,scene,primitives}/` 全空）。

## 交付物

### 头文件（`include/magicblocks/features/stack/`）

| 文件 | 内容 |
|---|---|
| `stack_types.h` | `Cell`(int32 x,y,z) / `Block`(24B, `static_assert`) / `GridSpec` / `Stroke` / `CellOrder{Index,YZX}` / `CellOrderLess` |
| `block_set.h` | `IBlockSet`：SOA 线性数组真源 + `unordered_map<Cell>` 派生索引（**只做查重，不参与输出序**） |
| `block_brush.h` | `IBlockBrush`：`ray→snap→DDA→dedupe`，层锁 |
| `color_mode.h` | 颜色模式（by-layer / 黄金分割默认色板 / 单色） |
| `shape_template.h` | `IShapeTemplate`（柱 / 台阶 / 空心框等） |
| `grid_overlay.h` | 网格叠层，走 `IOverlay2D` 通用能力 |
| `stack_render.h` | `StackRenderDesc` / `StackFrameDesc`（持 `float view_proj[16]`）/ 双 pass 描述 |

### 源文件（`src/features/stack/`）

`block_set.cpp` / `block_brush.cpp` / `color_mode.cpp` / `shape_template.cpp` /
`grid_overlay.cpp` / `stack_render.cpp`

### 测试

- `tests/magicblocks_l4_stack_test.cpp` — 数据模型 / 笔刷 / 渲染 / 叠层 / 颜色 / 模板
- `tests/magicblocks_l4_stack_command_test.cpp` — 命令模式三态 + 门控

### CLI

`magicblocks-l3-cli stack`，参数：`--width --height --layers --strokes --column-height
--color-mode --grid --out`。
例：`stack --width 480 --height 360 --layers 6 --strokes 4 --column-height 2 --color-mode by-layer`
→ `[stack] 笔画 4 | 方块 210 → 合批 2 (pass 2) | drawcall 2 管线切换 2 | doc_hash=0d98ebb1723ef103 inst=210`

## 关键设计裁定（落地时必须守着）

| 编号 | 裁定 | 依据 |
|---|---|---|
| D-VX-4′ | `depthConstWrite` **全面禁用** —— 不设、不开 CLI 参数、不作为验收项。内面正确性靠 `depthTest=false` + `cull=None` + **排在表面 pass 之后** + `depthWrite=true` | 实测该字段语义与设计假设不符（`depth_const=0.0` 与 `1.0` 两个用例互相矛盾），不能作为正确性依赖 |
| D-VX-5 | 内面实例按 `(y,z,x)` 升序 = `CellOrder::YZX`；表面按 `Index` | 内面 pass 关深度测试，绘制顺序即遮挡顺序；不排序则内面像素取决于落笔历史 |
| D-VX-8′ | L4 **禁止**调用 `create_pipeline_for_shape`（backend 私有）；两条 pass 都走公共 `create_pipeline(desc)` | 避免为玩法开专用通道 |
| E-VX-3 | `Block` 独立定义 24B（`Cell`4×3 + `color`4 + `stroke_id`4 + `flags`4），**不复用** L3 类型 | L3 根本没有 `Block` 类型 |

## 踩过的坑（本轮新增，均已留痕）

### E. 同一秒、同一 exit=0、文件存在，但图是空的 ⚠️

三张 320×200 的 PNG **字节数完全相同**（256283）、`exit=0`、文件都在，
但几何被压成 251×6 的扁条 —— 相机 `(4.2,3.6,5.0)→(0,0.9,0)` 把物体推出了视锥底部。
**教训：同字节数 ≠ 同内容。** 验收必须比内容（包围盒 / 前景像素占比 / 颜色数），
不能只看文件大小和退出码。改相机到 `(9,7,9)→(0,1.2,0)` 后 bbox 变为 102×58。

### F. 单实例 per-instance buffer 绑定会静默丢数据

`primitives/primitive_renderer.cpp:102` 只在 `instance_count > 1` 时绑 binding 1。
一个只有 1 个方块的柱体 → 实例数据根本不进管线，**无报错、出空图**。
L4 处理：`kMinInstanceCount` pad 到 ≥2，并二次校验；`instance_count()` 内两份长度
不一致时直接 `assert`（`stack_render.cpp:332-340`）。

### G. `commit()` 是整笔原子，批量堆叠不能用它

`IBlockBrush::commit` 遇已占格**整笔失败**（给交互落笔设计的语义）。CLI 批量堆叠
跨笔重叠是正常形态，所以 CLI 自己过滤已占格后走 `add_stroke`，**没有改 `commit` 语义**。

### H. 序列化 `%u` 写 / `strtol` 读会把高位吃掉

`0xAABBCCDD = 2864434397 > INT32_MAX`，`strtol` 置 `ERANGE` 截断 → 往返后流哈希变化。
改用 `num_u32()`（`strtoull`）。

### I. 门禁用例自己会“假通过”

三种假通过：GBK 中文输出被吞（加 `PYTHONIOENCODING=utf-8`）、`cmd /c` 内联引号导致
脚本没跑（改临时 `.py` 文件）、**扫描根本没跑起来也算 exit 0**（加 `N > 0` 反向断言）。

### J. G7 门禁分词漏检（L0–L3 既有门禁缺陷，本轮修复）

`tools/check_l3_no_gameplay_names.py:68` 原 `ALNUM_RE = r"[A-Za-z0-9]+"` 只按字母数字切词，
`VoxelCamera` 是一个整词，`voxel` 永远匹配不到 → 玩法名从驼峰标识符泄漏出去不被发现。
已补 `CAMEL_RE = r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+"` 二次切分（commit `ddc483c`），
selftest 43/43 通过，真实 L3 代码 32 文件无误报。

## 遗留技术债（L0–L3 既有问题，非本轮引入）

| 编号 | 问题 | 影响 |
|---|---|---|
| **T1** | `src/primitives/mesh.cpp:96-97` `create_mesh()` 用 `write()` 而非 `upload()+flush_pending()`，设备页缓冲上必然返回 `nullptr` | **真实功能缺陷**。目前全仓零调用者（仅定义在 `mesh.cpp:84` / `mesh.h:119`），尚未暴露；一旦有人用就必崩 |
| T2 | `Vertex/Index/Uniform/Storage` 在 device 页、CPU 不可映射，`write()` 返回 `Unsupported`，必须 `upload()`+`flush_pending()` | 漏 flush → GPU 读全 0 → 静默空白图，无报错 |
| T3 | `memory_allocator` 不自动扩页 | 须先 `reserve_pages(kind, N)`，否则上传失败 |
| T4 | `CameraSnapshot` 取不出 view-proj 矩阵 | L4 `StackFrameDesc` 只能直接携带 `float view_proj[16]` |
| T5 | 命令流只记 buffer handle，buffer 必须活到 `submit_command_list` 之后 | 提前析构 → Access violation。L4 改为出参 `OverlayBuffers*` 持有 |
| T6 | `IBlockBrush::commit` 原子语义（见坑 G） | 交互落笔可用，批量场景需绕开 |
| T7 | `pick_layer_for_ray` 恒给顶层（已修选层判据方向，但“恒顶层”的取交逻辑仍在） | CLI 改为逐笔显式 `layer_lock().lock()` |
| T8 | `IBlockSet` 无 `grid()` 访问器 | 需显式传递：`attach(set, grid)` / `replay(stream, set, grid)` |
| T9 | `magicblocks-l3-cli` 无 CTest 注册条目 | CLI 行为只靠独立用例覆盖，不进 ctest 矩阵 |

## 环境级事故记录

- **`3b06df0` 分支本身不可构建**：该提交在 `CMakeLists.txt:431` 注册了
  `tests/magicblocks_l4_stack_command_test.cpp`，但文件要到 `f71d54d` 才存在 →
  `CMake Error: Cannot find source file`。`af8b526` 补入后 main 才恢复可配置。
- **陈旧目标文件造成 66 条 SEH 假失败**：`MagicBlocks/build/` 被 C++（Ninja/CMake）与
  并发会话的 Android Gradle **共用**，混入陈旧目标文件后 L0 出现大批
  `0xc0000005` / `Access violation - no RTTI data!`。只删 C++ 子项
  （`build/CMakeFiles`、`CMakeCache.txt`、`build.ninja`、`.ninja_*`、
  `cmake_install.cmake`、`CTestTestfile.cmake`，**不能删 `build/tmp/`**）后全量重建，
  489/489 恢复全绿。教训：`build/` 共用目录时不要 `rm -rf build`，也不要相信增量重建的测试结果。
