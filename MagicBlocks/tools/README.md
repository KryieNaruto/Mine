# MagicBlocks\tools\ 说明

**门禁脚本在仓库根的 `tools/`，不在本目录。**

本目录只放 MagicBlocks 子项目自己的工具：

| 文件 | 用途 |
|---|---|
| `build.ps1` | 构建入口（会导入 `vcvars64`，**必须用它构建**，裸 `cmake --build` 会因 `C1083 cstdint` 失败） |
| `run_tests.ps1` | 测试入口 |
| `png_inspect.py` | PNG 像素分析（纯标准库） |
| `check_png_content.py` | PNG 内容检查 |
| `verify_model_renders.py` | 模型渲染像素断言 |
| `fetch-assets.ps1` | 资源获取 |
| `register_swiftshader_icd.ps1` | 注册 SwiftShader ICD |
| `vulkan_env.ps1` / `.sh` | Vulkan 环境变量 |

## 门禁脚本在哪

```powershell
E:\workspace\myspace\Mine\tools\check_*.py
```

| 门禁 | 脚本 |
|---|---|
| G1 无几何着色器 | `check_no_geometry_shader.py` |
| G3 分层纯度 | `check_layer_purity.py` |
| G4 零裸分配 | `check_no_raw_alloc.py` |
| G6 热路径零分配 | `check_l3_no_alloc_in_hotpath.py` |
| G7 无玩法名 | `check_l3_no_gameplay_names.py` |
| G8 命令对齐 | `check_l3_command_parity.py` |
| R1 离屏优先 | `check_offscreen_first.py` |
| UIG1 零回读 | `check_ui_no_readback.py` |
| UIG2 单次提交 | `check_ui_single_submit.py` |
| L2 无时钟 | `check_scene_no_clock.py` |
| deps-tests | `check_deps_tests.py` |

运行方式（`--root` 默认就是 `Mine\MagicBlocks\`，即主树）：

```powershell
cd E:\workspace\myspace\Mine
python tools\check_layer_purity.py
```

## 为什么写这个文件

2026-11-11 曾因 `MagicBlocks\tools\` 与 `Mine\tools\` **同名不同层**，
误判为「主树没有门禁脚本」，浪费了一轮排查。

**排查「某物不存在」时，必须用一个「应该存在」的正例做对照**
（例如：默认 root 扫描 206 个文件 vs worktree root 扫描 160 个文件），
不要只查一个目录就下结论。
