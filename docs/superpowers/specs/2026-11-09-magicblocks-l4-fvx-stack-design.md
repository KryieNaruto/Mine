# MagicBlocks L4 玩法二 · F-VX 魔法叠叠乐 设计(定稿)

> 文档类型:实现前设计规格(design spec),供后续 plans 与实现直接照做。
> 对应总 spec:`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`(下文简称「总 spec」)
> 参照同类文档:`docs/superpowers/specs/2026-11-05-magicblocks-l4-fsk-sketch-design.md`(F-SK,已实现、已出图)、`docs/superpowers/specs/2026-11-09-magicblocks-l4-fbl-blocks-design.md`(F-BL,同类玩法)
> 本轮不做实现、不改任何源码;本文只定契约与验收判据。

---

## 0. 本轮用户指令与硬边界

### 0.1 用户原话要点

> 「L4 第二个玩法 F-VX 魔法叠叠乐:用户在屏幕上刷一笔,小方块就沿着笔迹堆成柱。」

由此派生出的六条**功能需求**(全部来自总 spec §4 F-VX 行,行 275):

| 编号 | 需求原文(总 spec §4 F-VX) | 一句话落点 |
| --- | --- | --- |
| F-VX-1 | 笔刷沿笔迹扫掠,每格吸附到网格,自动堆叠成柱 | 笔迹 → 网格整数格 → 实例化方块列 |
| F-VX-2 | 跨笔独立 `stroke_id`,整笔撤销 | 一次落笔 = 一个 `stroke_id`;撤销整笔只改实例列表 |
| F-VX-3 | **内面渲染**:积木堆内部的面也要始终可见 | 双 pass:表面 pass + 内面 pass |
| F-VX-4 | 颜色模式:整堆改色 / 按层改色 / 按笔改色 | 只改 `Block.color`,不重编任何几何 |
| F-VX-5 | `IShapeTemplate`:一次落出预设形状(方 / 圆 / 十字 / 球形) | 模板 = 一组相对格偏移 |
| F-VX-6 | 笔刷层锁:锁定在一层上刷 | 落笔时锁定层平面,不随相机漂移 |

### 0.2 需求 → 设计章节映射表

| 需求 | 设计章节 | 关键落点 |
| --- | --- | --- |
| F-VX-1 扫掠吸附堆叠 | §6 笔刷链路 | §6.1 射线→层平面 → §6.2 `floor((p-origin)/cell_size)` 吸附 → §6.3 3D supercover DDA → §6.4 去重判据 |
| F-VX-2 整笔撤销 | §5 数据模型 / §9 撤销 | `Block.stroke_id` + `IBlockSet::remove_stroke` |
| F-VX-3 内面渲染 | §10 渲染(双 pass) | §10.2 表面 pass、§10.3 内面 pass、§10.4 内面可见性的三项必要条件(注:`depthConstWrite` 已依 D-VX-4′ 禁用)、§10.6 合批 |
| F-VX-4 颜色模式 | §11 颜色模式 | 只改 `InstanceAttribs::base_color`,实例缓冲重写,几何零重建 |
| F-VX-5 形状模板 | §12 `IShapeTemplate` | 模板 = 整数格偏移集合;内置方/圆/十字/球形生成规则 |
| F-VX-6 层锁 | §6.5 层锁 | `lock_layer` 在 Down 事件定格,Move 期间不变 |
| **CLI + 离屏出图**(构建流水线硬约束) | **§13 CLI 契约** | 子命令 `stack`,离屏目标 + `write_png_rgba8`,三态确定性,逐字节可复跑 |
| 层归属 / 不污染 L0–L3 | §3 分层与文件布局 | `features/stack/*` + `src/features/stack/*`,L0–L3 **零改动** |
| 性能预算 | §14 | drawcall = 2,10k 方块 ≈ 480 KB,稳态 0 次 VMA |
| 验收判据 | §15 | A1–A14,含 CLI 出 PNG 与 unit test 清单 |
| 不回退 / 明确不做 | §2.4 / §16 | 无降级路径,所有非法输入显式报错 |

### 0.3 硬边界(本条优先于一切实现偏好)

- **B1 只写一个文件**:本轮只产出本文档,不改任何源码、不提交。
- **B2 CLI 必须能离屏出 PNG**。构建流水线的验收只看「命令行跑完 → 目录里出现真 PNG」。任何「只跑单测就算过」的设计视为不合格(总 spec §5.4 / §11,行 433-444)。
- **B3 无降级路径**。D8 原则:参数非法、形状未知、区域运算不支持、越界层锁,一律返回显式错误码 + 人话 message,退出码非 0;**不允许**「跳过这一格」「退化成单层」「悄悄关掉内面 pass」。总 spec §8 行 392 已把「改色/撤销只改实例列表」写成硬判据,同样不允许退化成重建几何。
- **B4 L0–L3 不许出现玩法语义**。玩法名(`stack`/`叠叠乐`/`积木`)只允许出现在 `features/` 与文档;L3 只提供通用图元(`mesh_factory`、实例化批、`IOverlay2D`、`ICamera`、`picker`、`DepthState`)。门禁 G3 / G7 见 §3.3。
- **B5 层归属固定**:公共头 `include/magicblocks/features/stack/*.h`,实现 `src/features/stack/*.cpp`;纯逻辑不碰 Vulkan,`src/features/` 内不得出现 `vkCmd*` / `CmdDraw`(G3)。

---

## 1. 现状基线(实测)

本节所有「实测」标记都来自本轮对**仓库现状**的直接读取/运行,不是推测。

### 1.1 L3 已经给了什么(实测)

| 能力 | 位置 | 对本玩法的意义 |
| --- | --- | --- |
| `Block` **不来自 L3** | L3 spec §6A 行 346-352 只给了 32B 草案,**未交付**;`include/magicblocks/primitives/` 下无 `Block` 类型(实测 grep 0 命中),L3 spec 行 939 明确划到 L4 | **本玩法自建 24B `Block`**(§4.1 勘误 E-VX-3),不复用、不依赖 |
| 落笔链路(射线 → 层平面 → `floor` → DDA) | 同上,行 362-384 | §6 的算法蓝本 |
| 内面渲染的 `DepthState` 组合 | 同上,行 385-399;`include/magicblocks/resources/pipeline.h` 行 144-151(⚠️ 头注释行 14-16 与实现不符,勿采信,见 R14) | 双 pass 的语义出处 |
| 渲染批次:drawcall = 2 | 同上,行 401-408 | 与 §10 的预算一致 |
| `cube()` / `box_faces(mask)` | `include/magicblocks/primitives/mesh_factory.h` | 方块网格来源 |
| `InstanceAttribs`(112 B,`base_color` @64) | `include/magicblocks/primitives/batch.h` 行 100-109 | 颜色只落在这里 |
| `IOverlay2D`(屏幕空间线宽) | `include/magicblocks/primitives/overlay2d.h` | 网格指示线 |
| `ICamera::screen_to_ray` | `include/magicblocks/primitives/camera.h` | 笔迹 → 世界射线 |
| `intersect_plane` | `include/magicblocks/primitives/picker.h` | 射线 → 层平面交点 |
| `kStandardUnlit = "std_unlit"` | `include/magicblocks/primitives/shader_library.h` 行 60 | 纯色/渐变着色(头注释已点名「叠叠乐」) |
| CLI 渲染 7 步模板 | `MagicBlocks/src/l3_cli/main.cpp` 行 1661-1990 | §13 直接照抄骨架 |
| 实例化管线描述 | 同文件 行 225-313 | binding 0 = 顶点 / binding 1 = 实例 |
| 软件设备描述 | 同文件 行 140-147(`require_software_rasterizer = true`) | golden 可复现 |

### 1.2 双 pass 深度的实现事实(实测,带行号)

`depthConstWrite` 的真实落地方式是 **depthBounds 测试**,不是着色器写深度:

- `MagicBlocks/src/backend/vulkan/vulkan_pipeline.cpp` 行 **614**:`ds.depthBoundsTestEnable = desc_.depth.depthConstWrite`。
- 同文件 行 **615-624**:开启时把 `minDepthBounds` / `maxDepthBounds` **双双钉在** `depth_const`(钳到 [0,1]);关闭时 min=0、max=1。
- 同文件 行 **668-671**:只有当 `depthTest || depthWrite || depthConstWrite` 三者之一为真,才把 `VkPipelineDepthStencilStateCreateInfo` 挂进管线;三者全假时 `pDepthStencilState = nullptr`。
- 同文件 行 **593**:`polygonMode` 恒为 `VK_POLYGON_MODE_FILL`(R2,无离线框)。
- 同文件 行 **435**:`effective_depth_ = desc_.depth`(可直接断言)。
- 同文件 行 **236-237**:管线哈希覆盖 `depthConstWrite` 与 `depth_const` —— 所以「只差深度状态」的两个 pass 必然得到**两个不同的 `pipeline_hash`**,这是 §10.6 合批判据成立的前提。

`AttachmentShape` 在公共面不可用(实测):`include/magicblocks/resources/pipeline.h` 里**没有** `AttachmentShape`;只有后端头 `src/backend/vulkan/vulkan_pipeline.h` 行 48-53 定义它,行 62-63 提供三参构造,`src/backend/vulkan/vulkan_render_device.h` 行 58-59 提供 `create_pipeline_for_shape(desc, shape)`。

### 1.3 多 pass 帧的硬约束(实测)

`MagicBlocks/src/backend/vulkan/vulkan_device_frame.cpp`:

- 行 **234**:`IRenderTarget* target = list.last_pass_desc().target;` —— **一帧内所有 pass 必须共享同一个 target**。这正好匹配 L3 渲染器的形状(一个 target 多 pass),也是本玩法「双 pass」能成立的原因。
- 行 **219**:上一个 pass 未 `end_pass` 再 `begin_pass` 直接报错(不允许嵌套)。
- 行 **271-288**:render pass 兼容性**逐项**比较 `has_depth` / `samples` / `color_format`;不兼容则 `create_pass` 现建一个新的。
- 行 **340-359**:clear 编码 `f[0..3]=rgba`,`f[4]=clear_depth_value`,`f[5]=clear_color 标志`,`f[6]=clear_depth 标志`;`clearValueCount = shape.has_depth ? 2 : 1`。
- 行 **303-323**:深度图像由 `OffscreenTarget::with_depth` 提供,没有深度图像时**不挂深度附件**。

L1 侧 `MagicBlocks/src/backend/vulkan/vulkan_command.cpp` 行 **458-527**:`begin_pass` 在目标可提供 `VkImage` 时**自建一张只有颜色附件的 render pass**(`VkAttachmentReference ref{0, ...}`,`pDepthStencilAttachment` 未设置);行 **532-545** 把 clear 值写进 `cmd.f[]`。

### 1.4 本轮实测:双 pass 深度组合的像素事实

> 方法:临时探针程序(仅本机验证,未入库、已清理)用与 `magicblocks-l3-cli sketch` 相同的 7 步骨架,离屏 320×240 + `with_depth=true`,把 8 个立方体沿 X 排成一排,相机 `eye=(3.0,2.4,3.6)` 看原点、fovY=45°;每例跑一遍 `submit_command_list` → `readback_rgba8` → `write_png_rgba8`,再统计前景占比 / 平均亮度 / 不同颜色数 / 与基准的逐像素差异。**这一段的结论是 §10.4 与 §10.5 的事实依据。**

关键前提(实测,很要命):**若管线按「公共 `create_pipeline(desc)`」创建且该 desc 的 `DepthState` 三开关全 false,后端不会把 `VkPipelineDepthStencilStateCreateInfo` 挂进管线(行 668-671),此时 `depthConstWrite` 完全无效** —— 因为根本没挂深度状态,自然也没有 depthBounds。所以内面 pass 的 `DepthState` 必须**至少开一个写位**(`depthWrite=true` 即可),否则内面 pass 的语义是空的。

| 例 | pass 组成 | 前景占比 | 平均亮度 | 不同色数 | 结论 |
| --- | --- | --- | --- | --- | --- |
| A | 只有表面 pass,`cull=Back` | 39.45% | 34.36 | 5(基准) | 背光面全黑,形状塌 |
| S | 只有表面 pass,`cull=Back`,`DepthState{}` 全 false | 39.45% | 34.36 | 5 | 与 A **逐像素相同**(0 差异) |
| B | 只有表面 pass,`cull=None` | 39.45% | 47.46 | **10** | 内面被画出来,但**前后顺序错乱** |
| K | 内面 pass 先、表面 pass 后(`cull=None`) | 39.45% | 34.36 | 5 | 与 A **逐像素相同**(0 差异)→ 内面被表面完全覆盖 |
| O / N | 表面 pass 后接内面 pass(`cull=None` / `cull=Back`) | 39.45% | 44.08 | 7 | 内面生效,且**顺序正确** |
| T | 只有内面 pass,`depthConstWrite=true, depth_const=0.0` | **0.00%** | 17.94 | 1 | **整帧被清成背景色** ⚠️ 与 E/J 矛盾,见 §1.4 勘误 |
| E / J | 表面 pass 后接 `depthConstWrite=true, depth_const=1.0` 的内面 pass | 39.45% | 44.08 | 7 | 与「无 const」的内面 pass **逐像素相同** ⚠️ 与 T 矛盾 |

> **勘误 E-VX-2(2026-11-09 审阅门后发现,必须优先于上表)**:T 例(`depth_const=0.0` → 整帧全背景)与 E/J 例(`depth_const=1.0` → 与不开 const **逐像素相同**)在**同一个驱动、同一份代码**上**不可能同时成立**。
> - 若 `depthBounds` 真的生效,T 例成立意味着「上下界钉死后全部片元被拒」;那么 E/J 把上下界钉在 1.0 时,内面片元的深度显然不等于 1.0,**同样应被全部拒绝**,结果应与 T 一致(全背景),而不是与「不开 const」逐像素相同。
> - 反过来,若 E/J 成立(`depthBounds` 被**忽略**),则 T 例的全背景必须另有原因(例如那组参数同时动了别的东西),而不能归因于 depthBounds。
>
> **根因(已复核)**:`depthBounds` 特性**从未被请求** —— `vulkan_render_device.cpp:150,157` 是 `VkPhysicalDeviceFeatures feats{}`(全 0)。对未启用的特性设置 `depthBoundsTestEnable = VK_TRUE` 属**未定义行为**,驱动程序可以做任何事(忽略、部分生效、或按内部实现偶然表现)。**上表 T 与 E/J 两条都只能视为「不可复现的单次观测」,不得作为设计依据。**
>
> **由此得出的硬裁定 D-VX-4′(替代 D-VX-4)**:
> 1. **`depthConstWrite` 在 F-VX 中完全禁用**。不在 `PipelineDesc` 里置位、不暴露 CLI 参数、不作任何验收判据。
> 2. 内面 pass 的正确性**只**来自三件事:`depthTest = false`、`CullMode::None`、**排在表面 pass 之后**。
> 3. 不新增 `--inner-depth-const` 与 `--allow-inner-depth-near` 参数(把 UB 固化成 API 是设计错误)。
> 4. 若后续确实需要「常量深度写」语义,必须先由 L0 侧**显式请求 `depthBounds` 特性**并在后端实现真正的常量深度写,那是独立工作项,不在 F-VX 范围。

三条可直接写进判据的结论(仅保留在 T/E 矛盾之外仍成立的部分):

1. **内面 pass 必须在表面 pass 之后**。K 与 A 逐像素相同(0 差异像素),说明「内面先画、表面后画」等于没画 —— 表面把内面整片覆盖了。**这条不受 E-VX-2 影响,仍是契约。**
2. ~~`depth_const = 0.0` 的 const-write 会把整帧吃掉~~ —— **作废,见 E-VX-2**。T 例的成因未定,且该配置本身属 UB,不得复现、不得依赖。
3. ~~**`depth_const = 1.0`(far)与「不开 const」在本场景逐像素相同**~~ —— **不可作为依据(见 E-VX-2)**。该观测依赖未启用的 `depthBounds` 特性,属 UB;「逐像素相同」最可能是驱动忽略该测试的结果,不能据此推出「钉到 far 无害」。**唯一成立的推论**是:真实叠叠乐是互相遮挡的方块堆,`depthTest=OFF` 本身**不保证**内面前后序,故 §10.4 把**内面实例排序**写成显式契约(D-VX-5) —— 这一条保留且必需,且它的成立**不依赖** `depthConstWrite`。

### 1.5 工程基线(实测)

- 测试基线:**389/389** 全绿;`ctest` 必须以 `-j 1` 跑(并行会出伪失败 `0xC0000409`)。
- 构建必须走 `MagicBlocks\tools\build.ps1`(带 `vcvars64`);直接 `cmake --build` 会 LNK1181。
- `magicblocks-l3-cli.exe blocks --width 480 --height 320 --blocks 12 --output …` 实测 exit 0 并写出真 PNG(输出行:`[blocks] 12 个体素 | 实体 12 → 请求 12 → 合批 4 (合并 8) | 管线切换 0`)—— 证明 CLI + 离屏出图路径在**本机**是通的,§13 的契约不是空想。
- `submit_command_list` 是唯一真正写像素的生产路径;`backend_hooks.h` 不需要(F-K 依赖评估 §3 已修正)。

---

## 2. 玩法定义与边界

### 2.1 一句话

**用户按住鼠标/手指画一条笔迹,引擎把这条笔迹按网格离散成整数格,在每个格位上堆起一根高度固定的方块柱;整堆支持整笔撤销、整堆/按层/按笔改色、内面始终可见、笔刷锁定在某一层。**

### 2.2 六条可验收行为

1. 画一笔 → 沿笔迹出现**列**:每列由 `column_height` 个方块垂直堆叠,列与列之间**无缝隙**(整数格相邻)。
2. 每列的位置**必然**落在整数格上:误差 0(整数即真值),不存在「半格」。
3. 撤销一笔 → 该笔贡献的所有方块**同时**消失,不重建、不抖动顺序。
4. 改色 → 像素变化**只**来自 `base_color`,实例数、实例顺序、深度缓冲行为**完全不变**。
5. 内面 pass 打开时,**被前面方块遮住的那些内侧面**仍然可见(实测 O/N 与 A 差 21870 像素,见 §1.4)。
6. 笔刷锁层后,相机俯仰变化时新落笔**仍然**在同一层。

### 2.3 与 F-BL(积木搭建)的边界

| 维度 | F-BL(单点放置) | F-VX(本玩法) | 谁复用谁 |
| --- | --- | --- | --- |
| 落笔 | 单点拾取 → 放一个方块/一个形状 | **扫掠**:一条笔迹离散成一串格 | F-VX **不复用** F-BL 的单点路径 |
| 撤销粒度 | 单个 `BlockId` | **整笔** `stroke_id` | 各自独立 |
| 内面渲染 | 不需要(`BlockShape` 单块 Q-BL-3 明文不做合并) | **必须有**,双 pass | F-VX 独有 |
| 层锁 | 无 | 有(`lock_layer`) | F-VX 独有 |
| 颜色 | 单块 | 整堆 / 按层 / 按笔 | F-VX 多两种模式 |

两份文档共享「只改实例列表、不重编几何」的原则,但**`Block` 定义与接口均不同名、不同头文件**,不可互换;两者的 `Block` 各自独立定义(F-VX 为 24B,见 §4.1 勘误 E-VX-3),避免后来者以为可以互换。

### 2.4 明确不做(且**不允许**以「降级」形式偷偷做)

- 不做自由浮点放置(`FreeFloat` / `IPlacementPolicy`):Q1 裁定(2026-11-04,总 spec §14 行 472-489)网格吸附**强制**。
- 不做运行期体素网格 / `IVoxelizer` 常驻:R-2 裁定,`IVoxelizer` 只在**一次性** OBJ → 方块预处理时出现。
- 不做方块之间的面合并(no-merge):合并会改变实例数据布局,破坏「改色只改 `base_color`」的判据。
- 不做几何着色器(G1)、不做线框(R2)。
- 不做跨帧增量阴影 / 屏幕空间内面;内面只靠双 pass。
- **不允许**把「内面 pass 关掉」当成性能选项偷偷默认开启关闭:`--inner-pass` 的取值是**契约**(§13.3)。

---

## 3. 分层与文件布局

### 3.1 新增文件(全部属于 L4)

```
include/magicblocks/features/stack/
  stack_types.h        // Cell / Block / StrokeId / BlockId / GridSpec / StackError
  block_set.h          // IBlockSet(真源,数据 + 查询 + 哈希)
  block_brush.h        // IBlockBrush(射线 → 格 → 列)
  shape_template.h     // IShapeTemplate(模板 = 偏移集合)
  stack_session.h      // IStackSession(命令 / 撤销 / 改色 / 层锁的状态机)
  stack_render.h       // IStackRenderer(双 pass 录制)
src/features/stack/
  block_set.cpp
  block_brush.cpp
  shape_template.cpp
  stack_session.cpp
  stack_render.cpp
```

CMake:在 `MagicBlocks/CMakeLists.txt` 核心库源文件表(实测 F-SK 在行 243-244 登记 `src/features/sketch/*.cpp`)的同一位置登记 `src/features/stack/*.cpp`;新测试文件 `tests/magicblocks_l4_stack_test.cpp` 加入 `magicblocks_test` 目标(即 F-SK 拿到 CTest 覆盖的方式)。

### 3.2 依赖方向(只向下)

```
features/stack/  ──►  primitives/(L3:mesh_factory, IOverlay2D, ICamera, picker, batch)
                 ──►  scene/(L2)、resources/(L1:IBuffer, IPipeline, ICommandList)、device/(L0)
```

**本玩法不向 L0–L3 增加任何能力**,§3.4 逐条说明为什么够用。

### 3.3 门禁

- **G3**(`tools/check_layer_purity.py`):`src`, `include`, `tests`, `tools`, `shaders` 里 `backend/` 之外不得出现 `Vk*`;`src/features/` 内不得出现 `vkCmd` / `CmdDraw`。
- **G7**(`tools/check_l3_no_gameplay_names.py`):扫描 `include/magicblocks/primitives` 与 `src/primitives`,不得出现玩法名。**命令行本轮的注意事项(实测)**:`stack` **不在** G7 的禁用名单里,但 `blocks` / `voxel` / `叠叠乐` / `积木` **在**;所以 L3 里连变量名都不能叫 `blocks`。放行规则实测(`:131-138`):先把行拆成**完整标识符词**,凡词命中 `ALLOWED_IDENTIFIERS`(即 `block` / `blocks`)的**无条件跳过**,其余词再比对禁用表 —— 即按**完整标识符**放行,`blocks_gameplay` 这类新名字仍会被抓。
- **G1**:无几何着色器。**G4**:零裸分配,一切资源来自 `IMemoryAllocator`。**G8**:无隐式降级。
- **UIG1/UIG2**:渲染器只**录制**命令,由调用方 `submit_command_list` 提交一次。

### 3.4 为什么 L0–L3 够用(逐条证伪「需要新增能力」)

| 担忧 | 实测反证 |
| --- | --- |
| 需要新的深度模式? | 不需要。`pipeline.h` 行 144-151 的 `DepthState` 已有 `depthTest/depthWrite/depthOp/depthConstWrite/depth_const`,仅用 `depthTest`/`depthWrite`/`depthOp` 三个字段即可表达双 pass。⚠️ **不再引用头注释行 14-16** 的「叠叠乐 = depthConst=near」表述 —— 该注释与实现不符(`depthConstWrite` 落成未启用的 `depthBounds`,属 UB),依 D-VX-4′ / 勘误 E-VX-2 不采信(见风险 R14)。 |
| 需要新的批处理接口? | 不需要。`batch.h` 的 `DrawRequest`/`BatchDraw`/`BatchStats`(行 54-93)已覆盖实例化绘制与统计;限制「drawcall ≤300、pipeline_switches ≤16」足够本玩法(§14 只用 2 与 1)。 |
| 需要新的网格求交? | 不需要。`picker.h` 的 `intersect_plane` + `ICamera::screen_to_ray` 足够;网格吸附是纯整数运算,属 L4。 |
| 需要新的 2D 叠加? | 不需要。`overlay2d.h` 的 `LineDesc`(屏幕像素线宽)正好画网格线。 |
| 需要新的网格生成? | 不需要。`mesh_factory.h` 的 `cube()` / `box_faces(mask)` / `quad()` 足够;**模板是「一组格偏移」而非「一张网格」**(§12),所以不必为球形/十字模板造新 mesh。 |

---

## 4. 数据模型

### 4.1 `Cell` 与 `Block`

```cpp
namespace magicblocks::features::stack {

// 整数格坐标。单位 = GridSpec::cell_size 的倍数。
struct Cell {
  std::int32_t x = 0;
  std::int32_t y = 0;   // 层号(向上为正)
  std::int32_t z = 0;
};

struct Block {
  Cell cell;              // 12 B
  std::uint32_t color;    // 8bit RGBA,直接喂 InstanceAttribs::base_color
  std::uint32_t stroke_id; // 哪一笔画的(整笔撤销的依据)
  std::uint32_t flags;     // 预留:bit0 = 内面高亮,其余必须为 0
};
static_assert(sizeof(Block) == 24, "Block 必须 24B(12B Cell + 4B color + 4B stroke_id + 4B flags)");
static_assert(alignof(Block) == 4, "Block 必须 4 字节对齐,保证逐字节哈希稳定");

}  // namespace
```

> **口径统一(勘误 E-VX-3)**:`Block` 的规格**只有 24 B 这一种**,以本节的 `static_assert` 为准。
> L3 设计文档 §6A 行 346-352 里提到的 32 B `Block` **并未随 L3 交付** —— L3 spec 行 939 明确把 `IBlockSet`/`IBlockBrush`/`IShapeTemplate` 划到 L4 之外,`include/magicblocks/primitives/` 下**不存在** `Block` 类型(实测 grep 0 命中)。因此「复用 L3 定义」是一条**不存在的退路,已删除**:`Block` 由本玩法在 `features/stack/` 内**完整定义**,24 B。

### 4.2 为什么是 `int32` 而不是 `float`

- **去重的唯一判据必须是精确相等**。浮点 `cell` 需要 ε 比较,而 ε 比较在「边界恰好落在格线上」时会两边都不等,产生**重叠方块**;整数比较没有这个问题。
- **哈希可复现**。`Block` 逐字节哈希 → 文档哈希 → golden 文件名;`float` 的 NaN/`-0.0` 会破坏「同样输入同样字节」。
- **世界坐标是派生量**:`world = origin + cell * cell_size`,由 `GridSpec` 单向计算,永不回写。

### 4.3 为什么是线性数组 + SOA,而不是 ECS/空间树

- 真源必须可**逐字节序列化**以做哈希(`hash()`),线性 `std::vector<Block>` 是最直接的形态。
- **不为查询造索引**:本玩法的查询只有「这个格有没有方块」与「这一笔有哪些方块」。前者用 `std::unordered_map<Cell, BlockId, CellHash>` 做**派生缓存**(不进哈希、可随时从真源重建);后者靠 `stroke_id` 线性扫描即可(10k 方块 < 0.1 ms)。
- **绝不引入 `unordered_map` 遍历序做输出顺序**(确定性铁律):所有输出遍历一律走真源数组的**下标序**。
- 选用 SOA(真源数组按 `Block` 连续)而非 AOS 拆分数组:实例缓冲要按顺序 `memcpy` 每个 `Block` 的派生量,连续数组一次线性遍历最快;真正的 AOS/实例属性重排只发生在**派生**的实例缓冲里(§10.6)。

### 4.4 不变量

- **I1**:`(cell, stroke_id)` 唯一 —— 同一格里不可能有两块 `Block`。由 §6.4 的去重判据保证。
- **I2**:`stroke_id` **单调递增且永不复用**(与 F-SK 的 D-SK-3 同规矩)。撤销一笔不回收它的 `stroke_id`;重做时用**新的** `stroke_id`,所以在日志里「撤销→重做」与「从未画过」可区分。
- **I3**:`Block.flags` 的未定义位必须为 0;`cell.y ∈ [0, layers)`。

---

## 5. `GridSpec` 与 `IBlockSet`

### 5.1 `GridSpec`

```cpp
struct GridSpec {
  float origin[3] = {0.0f, 0.0f, 0.0f};  // 网格原点(世界坐标)
  float cell_size = 0.5f;                // 一格边长(米)
  std::int32_t layers = 8;               // 层数 y ∈ [0, layers)
  std::int32_t half_extent = 16;         // x/z ∈ [-half_extent, half_extent)
};
```

约束(**非法即报错**,不裁剪):`cell_size > 0`;`layers >= 1`;`half_extent >= 1`;所有字段必须是常量(CLI 参数 → 构造 → 常量传参),本玩法**不允许**运行期改 `cell_size`(改了会让已存 `Block` 的物理位置整体漂移,而 `Block` 里不该存浮点)。

### 5.2 `IBlockSet`(真源)

```cpp
class IBlockSet {
 public:
  virtual ~IBlockSet() = default;

  // ---- 写入(全部返回新块的稳定下标;失败返回错误而非"尽力而为")
  virtual Result<std::size_t> add(const Block& b) = 0;
  virtual Status remove_stroke(std::uint32_t stroke_id) = 0;   // 整笔撤销
  virtual Status undo_stroke(std::uint32_t stroke_id, std::vector<Block>* out) = 0;

  // ---- 查询
  virtual std::size_t size() const = 0;
  virtual const Block* data() const = 0;
  virtual const Block* find(Cell c) const = 0;       // 存在返回指针,否则 nullptr
  virtual std::uint32_t max_stroke_id() const = 0;
  virtual std::size_t stroke_count() const = 0;

  // ---- 改色(只改 color,不许动 cell/stroke_id)
  virtual Status set_color(std::size_t index, std::uint32_t rgba) = 0;

  // ---- 确定性哈希
  virtual std::uint64_t hash() const = 0;
};
```

**哈希口径(必须写死)**:逐字节喂 `(cell.x, cell.y, cell.z, color, stroke_id, flags)`,按真源数组下标序;**不**包含 `unordered_map` 内容、不包含未初始化填充位(`static_assert` 保证 24 B 无 padding)。这样「撤销一笔」与「从未画过」哈希不同(I2 的 `stroke_id` 不复用),而「改色」必然改哈希。

### 5.3 撤销/重做的语义

- 撤销:按 `stroke_id` 把 `Block` 从真源移除,顺带从派生索引里删键。**不重建**任何几何、**不**重新编号其余方块(下标会前移,所以外层持有的 `BlockId` 在撤销后失效 —— 这是刻意的:UI 只持有 `stroke_id`)。
- 重做:调用方持有撤销时返回的 `std::vector<Block>`,`stroke_id` **改为新的单调值**后重放(I2)。
- 撤销不存在的 `stroke_id` → `StackError::UnknownStroke`,**不是**空操作。

---

## 6. 笔刷链路:射线 → 层平面 → 吸附 → 扫掠

### 6.1 射线 → 层平面

```
ray    = camera->screen_to_ray(px, py)              // L3 ICamera
plane  = { normal = (0,1,0), d = origin.y + layer*cell_size }
t      = dot(plane.point - ray.origin, n) / dot(ray.dir, n)
```

判据:

- `|dot(ray.dir, n)| < 1e-6`(射线平行于层平面)→ `StackError::RayParallelToLayer`,**不**退化成「用上一次的交点」。
- `t <= 0`(交点在相机背后)→ `StackError::RayMissesLayer`。

### 6.2 吸附(精确算法)

**一次落笔只算一次**(Down 事件),Move 期间不再重算平面:

```cpp
// p = 世界坐标交点,origin/cell_size 来自 GridSpec
std::int32_t snap(float p, float origin, float cell) {
  return static_cast<std::int32_t>(std::floor((p - origin) / cell));
}
Cell c = { snap(p.x, origin[0], cell_size),
           layer,                            // §6.5 锁定
           snap(p.z, origin[2], cell_size) };
if (c.x < -half_extent || c.x >= half_extent ||
    c.z < -half_extent || c.z >= half_extent ||
    c.y < 0 || c.y >= layers)
  return StackError::OutOfGrid;              // 显式报错,不 clamp
```

- **不做 ε 微调**。理由:整数格是唯一真值,任何 ε 都会让「格线上的一笔」在两次运行里落到不同的相邻格,破坏 golden。为了消除浮点抖动,吸附前把交点按 **1e-4 · cell_size** 量化到网格(`p = round(p / q) * q`,q 为量化步长),使「恰好落线」的点稳定落在同一侧;这是**确定性**要求,不是容差。
- 世界坐标由 `world = origin + cell * cell_size` 单向派生。

### 6.3 扫掠:3D supercover DDA

一笔 = 一串相邻格。**必须在整数空间做**,且**必须包含线段碰到的每一个格**(supercover,而不是只取端点或只穿格心)。

算法(Amanatides & Woo 的三维版本,全程整数化):

1. 把笔迹采样点逐个吸附为 `Cell`,得到折线 `c0 → c1 → … → cn`(去相邻重复)。
2. 对每段 `a → b`,在整数空间走 supercover:
   - `dir = sign(b - a)`,`delta = a - b` 的逐轴绝对值;
   - 从 `a` 出发,每次挑「下一个轴线平面在参数轴上最早被穿过」的那一轴前进一格,并把该轴的剩余量 += `delta`(全整数,无除法误差);
   - 直到到达 `b`;
   - 每个进入的格都 emit —— 包括**只被线段擦过一角**的格。
3. 采样点之间若跳过多个格(鼠标事件稀疏),第 2 步天然补齐,不需要插值。

判据:`a == b` 的段直接 emit `a` 一次;**段为空不 emit** 由调用方保证(见 §6.6 的空笔)。

### 6.4 去重判据(唯一、可复算)

- 每 `emit` 一个 `Cell`,先查派生索引 `occupied`:命中则**跳过并计数**(`skipped_occupied++`,进统计,不进哈希);未命中则 `add` 并登记索引。
- 判据是**整数格相等**,不是「距离小于某阈值」。因此:I1 唯一性由构造保证,不依赖顺序。
- 同一笔内部也可能自我交叉(画了个圈):同样被 `occupied` 拦下,所以「一笔内外各画一次只出现一根柱」。
- 去重**不**跨层:`occupied` 的键是三元素 `Cell`,同 (x,z) 不同 y 是两块。

### 6.5 层锁(`lock_layer`)

- 状态机:`unlocked` → (`lock(layer)`) → `locked` → `unlock()` → `unlocked`。
- `locked` 时:落笔的 `cell.y` **恒等于** `locked_layer`;相机俯仰变化**不影响**它(这正是需求 F-VX-6)。
- 越界:调用 `lock(layer)` 且 `layer ∉ [0, layers)` → `StackError::LayerOutOfRange`,**不**钳到合法范围。
- 锁定时必须显式给出 `layer`;不给 → `StackError::LockLayerRequired`(无默认值,避免「忘了设就悄悄锁在第 0 层」)。

### 6.6 错误码(`StackError::Code`)

```cpp
enum class Code {
  Ok = 0,
  RayParallelToLayer,   // 射线平行层平面
  RayMissesLayer,       // 交点在相机背后
  OutOfGrid,            // 吸附后越出 half_extent / layers
  LayerOutOfRange,      // lock(layer) 越界
  LockLayerRequired,    // 锁层时未给 layer
  UnknownStroke,        // 撤销不存在的 stroke
  UnknownTemplate,      // 模板名未知
  TemplateOutOfGrid,    // 模板偏移把方块推到网格外
  EmptyStroke,          // 笔迹没有任何有效格
  ColorOutOfRange,      // 颜色模式参数越界
};
```

所有错误都带 `detail` 字符串(人话),CLI 逐条打印后 exit 1(§13.4)。

---

## 7. 落笔 → 出图整链(按帧序)

```
[1] Down(px,py)         → screen_to_ray → §6.1 层平面 → §6.2 吸附 → 锁层(cell.y)
[2] Move/Up             → §6.3 supercover DDA → §6.4 去重 → IBlockSet::add(每格 column_height 块)
[3] 每帧               → IBlockSet 不变时零重建;变更时只重写实例缓冲(§10.6)
[4] render(doc, grid)   → pass A(表面) → pass B(内面) → 一次 submit
[5] readback + write_png (CLI 路径)
```

关键判据:**步骤 [3] 的「变更」只发生一次**。一次落笔产生的 N 个方块在 [2] 内一次性插入,期间不录制、不出图。

---

## 8. 列与堆叠规则

- 一次笔迹的每个格 → **一根柱**:`column_height` 个方块,`y = layer, layer+1, … layer+column_height-1`。
- 若某格已有柱(来自别的笔),**新落笔在该格不生效**(被 §6.4 拦下并计数),**不**叠加、**不**替换、**不**长高 —— 这是「去重」的同一条判据,不设例外。要「往上堆」必须先把层锁抬到那根柱顶上。
- `column_height` 的越界(柱超出 `layers`)→ `StackError::OutOfGrid` 整笔失败(**要么整笔成、要么整笔不成**,不留半根柱)。

---

## 9. 撤销

- 数据面:§5.3。
- 渲染面:**零几何重建**。撤销只让实例数据少 N 条;实例缓冲重写一遍(§10.6),顶点/索引缓冲、管线、网格全部不动。
- 若实现里持有「每笔的实例区间」,撤销后必须**压紧**实例缓冲(把后面的实例前移),不允许留空洞(空洞会让 `instance_count` 与实际不符,统计随之失真)。
- 重做:I2,`stroke_id` 换新值。

---

## 10. 渲染:双 pass(内面始终可见)

### 10.1 两个 pass 的职责

| | pass A(表面) | pass B(内面) |
| --- | --- | --- |
| 目的 | 画正常朝向的面,建立正确的前后关系 | 让被遮住的内侧面也可见 |
| `depthTest` | `true` | **`false`** |
| `depthWrite` | `true` | `true` |
| `depthOp` | `LessEqual` | `Always`(depthTest 关时无效,但保持显式) |
| `depthConstWrite` | `false` | **`false`**(依 D-VX-4′ 禁用) |
| `depth_const` | 不适用 | 不适用(`depthConstWrite=false`,该值不参与) |
| `CullMode` | `Back` | **`None`** |
| 顺序 | **先** | **后**(§1.4 结论 1) |

pass B 的**必须**组合是:**`depthTest=false` + `depthWrite=true` + `CullMode::None`**。`pipeline.h` 头注释行 14-16 提到的 `depthConst=near` **不采用** —— 依 D-VX-4′(§1.4 勘误 E-VX-2),`depthConstWrite` 依赖从未启用的 `depthBounds` 特性,属未定义行为,F-VX 全面禁用。内面可见性**不依赖**它。

### 10.2 pass A 的实现要点

- 管线:`std_unlit`(或 `l3_scene` 的自由光照,见 §10.7),`CullMode::Back`,`depthTest/Write = true`,`depthOp = LessEqual`。
- 建管线走**公共** `device->create_pipeline(desc)`(勘误 E-VX-1 / D-VX-8′)。**不要**调用 `create_pipeline_for_shape`(仅后端私有头可见),也**不要**为此给 L1 加字段。深度行为由 `DepthState` 决定,目标侧的深度附件由 `TargetDesc::with_depth = true` 提供,二者经后端 `pass_compatible` 判定自建 pass,功能正确。

### 10.3 pass B 的实现要点

- 独立 `PipelineDesc`(`depth` / `raster.cull` 与 A 不同)→ **必然**不同的 `pipeline_hash`(实测:`vulkan_pipeline.cpp` 行 236-237 把 `depthTest`/`depthWrite`/`depthConstWrite`/`depth_const` 都混进哈希;**即使不用 `depthConstWrite`,`depthTest` 与 `cull` 的差异也足以产生不同 hash**)。
- 两个 pass 之间**不重建** framebuffer、render pass、命令缓冲;只是 `end_pass` → `begin_pass` → `bind_pipeline` → `draw` → `end_pass`。
- 不设 `depth_const`(`depthConstWrite=false`,该字段不参与;D-VX-4′)。

### 10.4 为什么「始终显示内面」= 这一组合(逐项论证)

1. **`CullMode::None`**:要显示内面,首先得让背面**不被剔除**。pass B 若用 `Back`(实测例 O 与 N 的对比虽在本场景相同,但那是因为内面 pass 的深度判定不依赖剔除法向),语义上就是「只画外表面」,与需求矛盾 —— 所以契约直接写 `None`。
2. **`depthTest = false`**:内面被表面挡住时,若开深度测试就会**失败**从而不写颜色。关掉它,内面才「无论挡不挡都写」。
3. **`depthWrite = true`**:必须至少开一个深度写位,否则后端**不会挂深度状态**(实测:`vulkan_pipeline.cpp` 行 668-671),内面 pass 的深度语义就是空的。
4. ~~`depthConstWrite = true` + `depth_const` 取 far~~ —— **作废(D-VX-4′ / E-VX-2)**。原论证依赖 `depthBounds`,而该特性在 `vulkan_render_device.cpp:150,157` 从未被请求(全 0 feats),属未定义行为。**F-VX 不置位 `depthConstWrite`**;内面可见性由第 1、2、3、6 条即可完整保证。
5. ~~`depth_const = 0.0` 是陷阱~~ —— **作废(E-VX-2)**。T 例成因未定且属 UB,不得复现、不得依赖;CLI 不提供 `--inner-depth-const` 参数。
6. **顺序必须在后**:实测 K 例(内面先、表面后)与 A 逐像素相同(0 差异),说明内面前面画完就被表面覆盖 = 白画。
7. **诚实的局限**(实测 §1.4 结论 3):在**互相遮挡**的多方块场景下,`depthTest=OFF` 的内面 pass 自身**不保证前后顺序**。(注意:E-VX-2 已判定 `depthConstWrite`/`depth_const` 相关观测不可靠,故这里**不再**用「把深度写成 far」立论 —— 排序契约的来源正是「深度机制不可依赖,只能靠确定性绘制顺序」。)因此本设计**额外要求**:内面 pass 的实例顺序 = **按 y 升序、再按 z 升序、再按 x 升序**(记作 `CellOrder::YZX`)稳定排序后写入实例缓冲(`StackRenderDesc::inner_order = CellOrder::YZX`)。这一步是 O(n log n)、一次性、可复算,不改任何几何。这是把「不确定性」压成「确定性契约」,而不是降级。

### 10.5 深度缓冲的建立与复制(多 pass 必须注意)

- 目标:`create_offscreen_target({Kind::Offscreen, extent, with_depth = true})`。实测 `vulkan_device_frame.cpp` 行 303-323:深度图像由 `OffscreenTarget::with_depth` 提供,**没有深度图像时不挂深度附件**。
- 一帧内两个 pass **共享同一 target**(实测 行 234);`end_pass` 后再 `begin_pass` 合法(行 219 只禁止嵌套)。
- 兼容性:两个 pass 的管线形状必须**逐项**匹配 begin_pass 的附件形状(行 271-288 比较 `has_depth` / `samples` / `color_format`);不匹配时后端会现建一个 pass 重来,虽然能出图,但会**多一次 render pass 创建**,并且若管线自带的 pass 与自建的 pass 依赖不一致会得到两种不同像素(见 `vulkan_pipeline.cpp` 行 485-502 与 `vulkan_device_frame.cpp` 行 84-119 的一致性要求)。本设计**要求两者一致**,并在验收里加断言(§15 A7)。
- clear:两个 pass 都按调用方给的值 clear(实测 行 340-359 的编码);本设计**要求 pass A clear、pass B 不 clear**(`clearColor=false, clearDepth=false`),否则 pass B 会把 A 的成果擦掉。实测补充:即使把第二个 pass 的 clear 关掉,本探针的 8 立方体场景结果不变,说明「先清后清」在本场景不是唯一失败因素 —— 但**在真实叠叠乐里 clear 语义必须明确**,所以仍写死为「A 清、B 不清」。

### 10.6 两个实例化 draw 的布局与合批

- **同一份实例缓冲绑定两次**:pass A 与 pass B 的 `bind_vertex_buffer(inst.id(), 1, 0)` 指向同一个 buffer;差别只在 `bind_pipeline`。这样改色/撤销只要重写一份实例数据,两个 pass 自动一致。
- 实例数据布局(`InstanceAttribs`,112 B,`batch.h` 行 100-109):
  - `model[16]` @0:方块的世界矩阵。因为 `cell_size` 是世界尺寸,4 个 `instances-per-quad` 不做缩放,只做平移;但 `--instances-scale` 打开时按 `cell_size` 缩放(契约:缩放必须是 `cell_size` 的整数倍,否则会破坏「无缝隙」)。
  - `base_color[4]` @64:`Block.color` 直译(8bit → float/255),**改色只改这里**。
  - `emissive[3]` @80 / `roughness` @92 / `metallic` @96:固定值(0,0,0 / 0.55 / 0),来自 `StackRenderDesc`,不参与哈希。
- **批次键 = (`pass_index`, `pipeline_hash`, `mesh_id`)**。两个 pass 的 `pipeline_hash` 不同(§10.3)→ **2 个 `DrawRequest`** → **2 个 drawcall**。若把两个 pass 合成一个 draw,必然只能是同一 `pipeline_hash`,那就不可能有两种深度状态 —— 这是「2 drawcall」的结构性来源,不是巧合。
- 2 ≤ `BatchStats::kMaxDrawCalls(300)`、1 ≤ `kMaxPipelineSwitches(16)`,预算见 §14。

#### 10.6.1 `CellOrder` 与 `StackRenderDesc` 的定义(补齐悬空符号)

```cpp
// 内面 pass 实例缓冲的排序规则(D-VX-5)。
enum class CellOrder {
  Index,   // 真源下标序(表面 pass 用这个)
  YZX,     // 先 y 升序,再 z 升序,再 x 升序(内面 pass 用这个)
};

struct StackRenderDesc {
  float cell_size = 0.5f;          // 与 GridSpec::cell_size 必须相等
  CellOrder surface_order = CellOrder::Index;
  CellOrder inner_order   = CellOrder::YZX;   // D-VX-5 契约
  bool  inner_pass        = true;              // false 时只画表面 pass(--inner-pass off)
  float roughness         = 0.55f;             // 固定值,不参与哈希
  float metallic          = 0.0f;              // 固定值,不参与哈希
  std::uint32_t clear_color_rgba = 0xFF0F121A; // 与既有 CLI 清屏色一致
};
```
> `depthConstWrite` / `depth_const` **不在** `StackRenderDesc` 里 —— 依 D-VX-4′ 全面禁用,不给它任何入口。

#### 10.6.2 单实例(`instance_count == 1`)必须显式处理

`primitive_renderer.cpp:102` 只在 **`instance_count > 1`** 时绑定 binding 1。因此 `column_height = 1` 的单格笔迹会走到 `instance_count == 1`,此时实例属性**不绑定**,着色器读到的是未定义数据。

契约(**实现时必须二选一并写明**):

- **旁路**:`instance_count == 1` 的批次不走实例化路径,改用非实例化绘制;或
- **pad**:实例缓冲尾部补一条「退化实例」(全零 `model` → 投影到零面积),使 `instance_count` 恒 ≥ 2。

**断言**:任何路径下都**不得**出现「`instance_buffer_ != nullptr && instance_count == 1`」的组合。若实现后该组合仍出现且画面错位,**停工报告**,不得当作可忽略的视觉瑕疵。

### 10.7 着色与光照的确定性

- `l3_scene.frag` 的光照方向是**常量**(`kLightDir` / `kFillDir`),且注释明文「刻意不随时间变 —— 命令回放要求同样的输入产出同样的像素」;Blinn-Phong 的视线近似也是常量。→ 同样的实例数据必然产出同样的像素,改色必然确定性改变像素。
- 纯色/渐变的颜色模式也用同一套着色(只换 `base_color`),**不**新增着色器,避免 golden 因着色器切换而整体重录。

### 10.8 ~~必须的 L1 依赖补齐~~ → **已撤销:不需要补 L1(勘误 E-VX-1)**

> **本节原始结论(P0-1)经复核为误判,已在 2026-11-09 审阅门后撤销。**以下为勘误后的正确结论。

**原误判**:带深度的离屏管线无法通过公共面创建,故须给 `resources::PipelineDesc` 补 `uses_depth_attachment`。

**事实(实测复核,三条互相独立)**:

1. **既有三个带深度的子命令,走的全是公共 `create_pipeline`,且都出对了图**:`l3_cli/main.cpp` 中 `td.with_depth = true` 出现在 `:639`(blocks)、`:1130`(gizmo)、`:1733`(sketch);这三处建管线只调用 `device->create_pipeline(...)`(`:901`、`:1446`、`:1864`),**从未**调用 `create_pipeline_for_shape`。
2. **`create_pipeline_for_shape` 全仓唯一调用点**是 `src/android/scene_renderer.cpp:274`,且传的是 `shape.has_depth = false`(`:271`)。
3. **公共面管线的 hash 与附件形状无关**:`vulkan_pipeline.cpp:251-256`,`hash_pipeline_desc_with_shape` 在默认形状下**退化为** `hash_pipeline_desc`。
4. **后端在形状不匹配时会自建 render pass**:`vulkan_device_frame.cpp:271-283` 的 `pass_compatible` 失败即 `create_pass(...)`;而 `create_pass` 与 `create_render_pass` 逐项一致,故「两套 pass 依赖不一致 → 像素不同」的担忧不成立。

**裁定 D-VX-8′(替代 D-VX-8)**:

- F-VX **不新增**任何 L1 字段,**不改** `resources/pipeline.h`、**不改**后端。
- 双 pass 的两个 `PipelineDesc` 均走公共 `create_pipeline(desc)`;深度行为完全由 `DepthState` + `RasterState` 决定,与 `has_depth` 形状字段无关。
- **L4 禁止调用 `create_pipeline_for_shape`**(它只声明在后端私有头 `vulkan_render_device.h:58`;调它就等于让 L4 依赖后端实现细节,破坏单向依赖,且撞 G3)。
- **§10.2 原文「必须通过带附件形状的入口建管线」同理作废**,正确做法是公共 `create_pipeline`。

**回归护栏(必做)**:`tests/` 中 `with_depth = true` 当前 **0 处**(全部 `with_depth = false`:l1_test:550/631、l2_test:68、l3_pixel_regression:71、magicblocks_test:70/269、ui_test:994),即「公共面 + 带深度」这条路径**没有任何 CTest 像素断言**。因此本设计的阶段 3 必须新增首批公共面带深度像素回归用例(见计划阶段 3),否则「不动 L1」这个裁定本身没有护栏。

---

## 11. 颜色模式

| 模式 | 语义 | 写入路径 | 重建次数 |
| --- | --- | --- | --- |
| `Flat` | 整堆同色 | 全部 `Block.color = c` | 0(只重写实例缓冲) |
| `ByLayer` | 按 `cell.y` 取色 | 每块 `color = palette[cell.y % palette.size()]` | 0 |
| `ByStroke` | 按 `stroke_id` 取色 | 每块 `color = palette[stroke_id % palette.size()]` | 0 |

判据(全部可断言):

- **几何零重建**:顶点缓冲、索引缓冲、网格工厂调用次数 = 0;`IBlockSet::hash` 之外的数据结构不变。
- **只改 `base_color`**:改色前后实例缓冲里除偏移 64-79 之外的字节**逐个相同**(§15 A8)。
- `palette` 为空 → `StackError::ColorOutOfRange`;`palette` 项必须是 32bit RGBA。
- 色板本身取色是**确定性函数**(`palette[i]`,不做随机、不读时钟);CLI 的默认色板用与 F-SK 相同的黄金分割 hue 序列(`l3_cli/main.cpp` 行 1606-1657 的做法),保证 golden 可复算。

---

## 12. `IShapeTemplate`

### 12.1 定义

```cpp
// 模板 = 一组相对格偏移;不是一张网格。
struct ShapeTemplate {
  const char* name;                 // "square" / "circle" / "cross" / "sphere" / 自定义
  const Cell* offsets;              // 相对锚点的整数格偏移
  std::size_t count;
};

class IShapeTemplate {
 public:
  virtual ~IShapeTemplate() = default;
  virtual Result<ShapeTemplate> find(const std::string& name) const = 0;  // 未知 → UnknownTemplate
  virtual std::vector<const ShapeTemplate*> list() const = 0;             // 稳定顺序
};
```

### 12.2 内置模板的生成规则(全部整数、可复算)

设半径 `r`(`--template-radius`,整数 ≥ 1),平面内 `(dx, dz)`,`anchored at (0,0)`:

| 模板 | 格集合 | 说明 |
| --- | --- | --- |
| `square` | `max(|dx|,|dz|) <= r` | 正方形实心块 |
| `circle` | `dx*dx + dz*dz <= r*r` | 圆盘(整数判据,无浮点) |
| `cross` | `(dx == 0 && |dz| <= r) \|\| (dz == 0 && |dx| <= r)` | 十字 |
| `sphere` | `dx*dx + dy*dy + dz*dz <= r*r`,`dy ∈ [-r, r]`(y 再平移到锚点层) | 球;这是唯一跨多层的模板 |

- **偏移集合按 `(dy, dz, dx)` 升序输出**(确定性):模板 → 方块的插入顺序固定,`IBlockSet::hash` 因此稳定。
- 应用模板时锚点 = 笔迹吸附格;若任一偏移使其越出 `GridSpec` 边界 → `StackError::TemplateOutOfGrid`(`detail` 里给出第一个越界偏移),**不**裁剪、不跳过。
- 模板与列高互斥:`--column-height` 与 `--template` 同给 → 参数错误(两种堆叠规则不能混)。

### 12.3 扩展点

- 外部模板注册:实现 `IShapeTemplate::find` 即可;`list()` 必须返回稳定顺序(按注册序,不回退到 `unordered_map` 遍历序)。
- **一次性预处理**才用 `IVoxelizer`(R-2):OBJ → 方块集合 → 包装成 `ShapeTemplate` 常量。运行期**不**持有 `IVoxelizer`。
- 模板**不是** L3 概念:它只出现在 `features/stack/shape_template.h`,不进 `primitives/`(G7)。

---

## 13. CLI 契约(构建流水线的硬约束)

### 13.1 子命令

```
magicblocks-l3-cli stack  --width 640 --height 400
                          --layers 8
                          --strokes 3
                          --cell-size 0.5
                          --column-height 1
                          [--template square|circle|cross|sphere]
                          [--template-radius 2]
                          [--color-mode flat|by-layer|by-stroke]
                          [--lock-layer 2]
                          [--inner-pass on|off]
                          [--stroke-pattern 0|1|2]
                          [--dry-run]
                          --output stack.png
```

- 子命令名 `stack`,与既有 `paint` / `blocks` / `gizmo` / `sketch` 并列(`l3_cli/main.cpp` 行 1994-2014 的分派点)。
- **`--output` 必填**;缺失 → usage + exit 2(与 F-SK 的 `CliRequiresOutput` 同规矩)。
- `--inner-pass` 的取值必须显式:缺省按 `on`(契约默认),但**不接受** `auto` / `default` / 空值这类「让程序自己决定」的写法(`--inner-pass 1` 也接受,按真值解析)。
- ~~`--inner-depth-const`~~ **不提供**(D-VX-4′:`depthConstWrite` 依赖未启用的 `depthBounds` 特性,属 UB,不得固化成 API)。出现该参数 → usage 错误 + exit 2(`未知参数`)。

### 13.2 出图 7 步(照抄 `l3_cli/main.cpp` 行 1661-1990 的骨架)

```
1) 构造文档:make_stack_strokes(...) → IStackSession —— 纯逻辑,不碰设备
2) 构建网格列表:逐笔吸附 → DDA → 去重 → IBlockSet;错误则逐条打印 exit 1
3) 创建设备:software_desc()(require_software_rasterizer = true)+ reserve_pages(Vertex/Index/Staging, 4)
4) 创建离屏目标:TargetDesc{Offscreen, {width,height}, with_depth = true}
5) 上传:顶点/索引/实例缓冲 → flush_pending() → 断言 pending_upload_bytes() 归零
6) 录帧:pass A(begin → bind → draw → end)→ pass B(begin → bind → draw → end)→ 一次 submit_command_list
7) readback_rgba8 → write_png_rgba8(--output) → 打印统计一行
```

实测该骨架在本机可跑通(§1.5:`blocks` 子命令已出 PNG)。

### 13.3 确定性(三态可复跑)

- **无时钟、无随机、无 `unordered_map` 遍历序**:所有顺序来自真源数组下标 / 模板偏移的排序 / 稳定 batch key。
- 同一命令行跑两次 → PNG **逐字节相同**(§15 A10)。像素结果不依赖 `--stroke-pattern` 之外的任何时序。
- `--dry-run`:只走到步骤 2 并打印统计(`blocks / strokes / skipped_occupied / doc_hash / instance_bytes / drawcalls`),**不创建设备、不出图**;退出码 0。这条是给 CI 快速校验「吸附与去重逻辑对了」用的,不是降级路径。
- 颜色模式切换必须**只**改变像素里与 `base_color` 相关的部分;`--dry-run` 的 `doc_hash` 在只改颜色时**会变**(哈希含 `color`,见 §5.2),这是预期的。

### 13.4 退出码与输出格式

| 情况 | 退出码 | stderr |
| --- | --- | --- |
| 成功 | 0 | — (stdout 打印一行统计) |
| 参数非法(缺 `--output`、`--cell-size <= 0`、未知模板、出现已禁用参数如 `--inner-depth-const`) | 2 | `[stack] 参数错误: <detail>` |
| 逻辑错误(OutOfGrid / UnknownStroke / TemplateOutOfGrid / EmptyStroke) | 1 | 逐条 `[code=N] <detail>` |
| 设备/上传/出图失败 | 1 | `[stack] <what>: <message>` |

成功时的统计行(格式固定,便于 golden 与脚本比对):

```
[stack] 笔画 3 | 方块 96 → 请求 96 → 合批 2 (pass 2) | drawcall 2 管线切换 1 | doc_hash=… inst_hash=… | 已写出 stack.png (640x400)
```

### 13.5 需要的 CTest 覆盖(通过 gtest,而非 CTest 外部进程)

与 F-SK 的做法一致:CLI 端到端用 gtest 断言(`SketchEndToEnd.*` 的做法),因为 `CMakeLists.txt` 里没有给 l3-cli 单独注册 CTest 条目。本玩法对应:

```
StackEndToEnd.CliProducesRealPng
StackEndToEnd.CliIsByteIdenticalAcrossRuns
StackEndToEnd.CliRejectsZeroStrokes
StackEndToEnd.CliRequiresOutput
StackEndToEnd.CliDryRunMatchesFullRun
```

---

## 14. 性能预算

| 项 | 预算 | 依据 |
| --- | --- | --- |
| drawcall / 帧 | **2**(pass A + pass B) | §10.6;L3 上限 300 |
| pipeline 切换 / 帧 | **1** | 两次 `bind_pipeline` 之间不换几何 |
| 实例数据 | 10k 方块 × 112 B = **1.12 MB**(实例缓冲) | `InstanceAttribs` 112 B,`batch.h` 行 100-109 |
| 真源数据 | 10k × 24 B = **240 KB** | §4.1 |
| 总显存(方块部分) | ≈ **1.4 MB** | 上文两项 + 顶点/索引(单立方体共用) |
| 稳态 VMA 调用 | **0 次** | M5:`reserve_pages` 一次预取,热路径只动空闲链表 |
| 撤销 / 改色 | 0 次几何重建,只重写实例缓冲(O(n)) | §9 / §11 |
| 落笔 | O(格数),无排序 | §6.3 |

> 总 spec §8 行 392 的「10k instance ≈ 480 KB」是**旧口径**(按 48 B/实例估的);本设计按实测的 112 B/实例重新给出 **1.12 MB**,并在此明确:**以本节的实测口径为准**,不改总 spec 的历史陈述,但实现与 golden 预算按 1.12 MB 走。

---

## 15. 验收标准 A1–A14

| 编号 | 判据 | 形式 |
| --- | --- | --- |
| **A1** | 网格吸附:给定射线与 `GridSpec`,吸附出的 `Cell` 与手算整数完全一致(含格线上的点、负坐标、`cell_size≠1`) | 单测 `StackSnap.*` |
| **A2** | supercover:一段斜线经过的每一格都被 emit;擦角格不漏;`a==b` 只 emit 一次 | 单测 `StackSweep.*` |
| **A3** | 去重:同格二次落笔被拦、计数进 `skipped_occupied`、真源大小不变 | 单测 `StackDedup.*` |
| **A4** | 整笔撤销:撤一笔后真源恰好少该笔数量;其余方块逐字节不变;`hash` 变化 | 单测 `BlockSet.UndoStroke*` |
| **A5** | 撤销不存在笔 → `UnknownStroke`;锁层越界 → `LayerOutOfRange`;模板未知 → `UnknownTemplate`;空笔 → `EmptyStroke` | 单测 `StackFailures.*` |
| **A6** | 改色:三种模式各自只改变 `base_color` 字节区间(64-79),`cell/stroke_id` 不变 | 单测 `StackColor.*` |
| **A7** | 双 pass:① 两个 pass 的 `pipeline_hash` **不同**;② 两个 pass 在 `with_depth=true` 的离屏目标上**都真的画出像素** | 单测 `StackRender.PassPairIsDistinct`(hash) + `StackRender.BothPassesRenderOnDepthTarget`(像素) |

> **A7 修订说明（M6/N4）**：原 A7 要求「断言管线附件形状 `has_depth == true`」，但 `attachment_shape()` **只暴露在后端私有头** `vulkan_pipeline.h:98`，公共面 `IPipeline` 无此接口；且公共面 hash 与形状无关（`vulkan_pipeline.cpp:251-256`，`hash_pipeline_desc_with_shape` 在默认形状下退化为 `hash_pipeline_desc`）。→ **原判据在当前公共面下不可实现**，故拆为上面两条均可由公共面断言的用例。为断言形状而 include 后端私有头会撞 G3（`check_layer_purity.py:97` 连 `tests/` 也扫），**禁止**。
| **A8** | 内面像素事实:`--inner-pass on` 与 `off` 两图**不同**,且 `on` 图的不同颜色数 **>** `off` 图(本机实测 `on` 前景内至少多出 2 档色;见 §1.4 的 7 vs 5 / 21870 差别像素) | 单测 `StackRender.InnerPassChangesPixels` |
| **A9** | `--dry-run` 的统计(方块数、`doc_hash`、drawcall 预算)与完整运行一致;不创建设备 | 单测 `StackEndToEnd.CliDryRunMatchesFullRun` |
| **A10** | CLI 出真 PNG:文件存在、尺寸正确、前景像素占比 > 0、非纯背景;两次运行**逐字节相同**(**仅限同平台同驱动**;跨平台 golden 比对走 ≤1%/每通道容差,见 R5) | 单测 `StackEndToEnd.CliProducesRealPng` / `…ByteIdenticalAcrossRuns` |
| **A11** | 缺 `--output` → exit 2;`--strokes 0` → exit 1 且不写文件 | 单测 `StackEndToEnd.CliRequiresOutput` / `RejectsZeroStrokes` |
| **A12** | 层锁:相机俯仰变 3 次,新落笔的 `cell.y` 恒为锁定值 | 单测 `StackLayerLock.*` |
| **A13** | 门禁:G1/G3/G4/G7/G8/UIG1/UIG2 全绿;`src/features/stack/` 内零 `vkCmd`/`CmdDraw`;`primitives` 内零玩法名 | 既有 CTest 门禁 |
| **A14** | 回归:全仓 `ctest -j 1` 保持 **389 起、全绿**(新增用例只增不减) | `ctest -j 1` |

---

## 16. 裁定记录(D-VX-*)

| 编号 | 裁定 | 理由 / 反证 |
| --- | --- | --- |
| **D-VX-1** | 网格吸附强制,`Cell` 用 `int32`,`Block` 24 B | Q1 裁定;浮点不可哈希、ε 去重会重叠(§4.2) |
| **D-VX-2** | 撤销粒度是**整笔**(`stroke_id`),不提供单块撤销 | 需求 F-VX-2 原文;D-SK-3 同规矩(`stroke_id` 单调不复用) |
| **D-VX-3** | 内面渲染 = 表面 pass + 内面 pass 两个 drawcall,内面 pass 在后 | 实测 K 例:内面先画 = 与只有表面逐像素相同(0 差异) |
| **D-VX-4** | ~~内面 pass 的 `depth_const` 契约默认 1.0(far);`0.0` 必须显式豁免~~ **已被 D-VX-4′ 替代(E-VX-2)**:`depthConstWrite` 全面禁用(依赖从未启用的 `depthBounds` 特性 = UB);内面 pass 只用 `depthTest=false + depthWrite=true + cull=None` + 排在表面 pass 之后 | T 例(0.0 全背景)与 E/J 例(1.0 逐像素等同不开)在同一驱动上互斥,二者均不可作为依据;`vulkan_render_device.cpp:150,157` feats 全 0 |
| **D-VX-5** | 内面 pass 实例按 `(y, z, x)` 升序写入实例缓冲 | 实测:互相遮挡时 `depthTest=OFF` 自身不保证内外顺序;排序把不确定性压成契约(§10.4 第 7 条) |
| **D-VX-6** | 改色只改 `Block.color → InstanceAttribs::base_color`,实例缓冲重写,几何零重建 | 总 spec §8 行 392 硬判据 |
| **D-VX-7** | 模板 = 整数格偏移集合,不是网格 | 避免为一堆形状造 mesh;G7 也不允许形状名下沉 |
| **D-VX-8** | ~~需要补 L1 的 `uses_depth_attachment`(P0-1)~~ **已被 D-VX-8′ 替代(勘误 E-VX-1)**:P0-1 前提不成立,公共 `create_pipeline` 建带深度管线可用;不改 L1、不改后端、L4 禁止调 `create_pipeline_for_shape` | 三条独立实测证据见 §10.8;另 `tests/` 中 `with_depth=true` 为 0 处,故阶段 3 必须补公共面带深度像素回归 |
| **D-VX-9** | 所有非法输入显式报错,无 clamp / 无跳过 / 无「近似」 | D8 / B3 |
| **D-VX-10** | 实例数据口径按 112 B/实例(1.12 MB @10k),覆盖总 spec §8 行 392 的 480 KB 旧估算 | 实测 `InstanceAttribs` = 112 B(`batch.h` 行 100-109) |

---

## 17. 与总 spec 的对应

| 总 spec 位置 | 内容 | 本文对应 |
| --- | --- | --- |
| §2.1 行 84-89 | 分层单向依赖 | §3.2 |
| §2.2 行 100 | F-VX 玩法一行定义 | §0.1 / §2.1 |
| §4 行 238 / 239 / 240 | `IBlockSet` / `IBlockBrush` / `IShapeTemplate` | §5 / §6 / §12 |
| §4 行 245 | `DepthState`(含 `depthConstWrite`) | §10.1 |
| §4.1 行 258-262 | 玩法能力清单 | §2.2 |
| §4 行 275 | F-VX 六条需求原文 | §0.1 表 |
| §5.4 / §11 行 433-444 | CLI + 每玩法至少 1 张离屏 PNG;命令三态;单测含「方块扫掠求交含撤销整笔」「`IShapeTemplate` 实例化」 | §13 / §15 |
| §8 行 392 | 改色/撤销只改实例列表,不重编几何 | §9 / §11 / A6 |
| §14 行 472-489 | Q1 裁定(网格吸附强制) | §4.2 / D-VX-1 |
| L3 §6A 行 336-428 | Block / 落笔链路 / 内面渲染 / 渲染批次 | §4 / §6 / §10 |
| L3 §12 行 815 + R-2 行 818-833 | `IVoxelizer` 仅一次性预处理 | §12.3 |
| L3 §15 行 939 | `IBlockSet`/`IBlockBrush`/`IShapeTemplate` 不做在 L3 | §3.1 / §3.4 |

---

## 18. 遗留与依赖(必须在本轮计划里排上)

1. ~~**P0-1 L1 补齐**:`resources::PipelineDesc::uses_depth_attachment`~~ —— **已撤销(勘误 E-VX-1 / D-VX-8′)**。前提不成立,公共 `create_pipeline` 建带深度管线可用,**本轮不改 L1、不改后端**。取而代之的必做项是:阶段 3 新增**首批「公共面 + `with_depth=true`」像素回归用例**(当前 `tests/` 中该类为 0 处,无护栏)。
2. **CMake**:登记 `src/features/stack/*.cpp` 到核心库源表(`CMakeLists.txt:243-245` F-SK 区块旁),`tests/magicblocks_l4_stack_test.cpp` 加到 `add_executable(magicblocks_test ...)`(`:423-441`,注意 `:441` 行尾带右括号,新增行须插在其前)。
3. **ctest 必须 `-j 1`**;基线 389。
4. §10.4 第 7 条的「内面排序」如与实现者的实测结论不符,请**改本文档**而不是改实现 —— 这条是契约,不是建议。
5. **门禁口令建议带 `--root MagicBlocks`**:`check_l3_no_gameplay_names.py:158` 的 `--root` **默认值即 `MagicBlocks`**,故不带 `--root` 从仓库根跑**同样正确**(实测两种跑法均「扫描 32 个文件」exit 0)。显式写 `--root` 只为无歧义;仍建议断言输出扫描文件数 **N > 0** 作为低成本自检。(此前「不带 `--root` 会扫 0 文件」的说法经第二轮实测**已证伪并撤销**。)
6. **`depthBounds` 特性全 0 是设备级风险**:`vulkan_render_device.cpp:150,157` 的 `VkPhysicalDeviceFeatures feats{}` 把 `depthBounds`/`depthClamp`/`fillModeNonSolid`/`wideLines`/`logicOp` 等**全部默认特性一并关掉**。现行代码恰好没用到它们,属运气。登记为风险,任何功能**不得**依赖其中任意一项。
