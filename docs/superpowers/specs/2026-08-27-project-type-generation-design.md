# 按项目类型自动生成 IDE 工程(project-type-generation)设计

日期:2026-08-27

## 1. 背景与目标

Mine 工作区是"一项目一文件夹 + 三方库全局共享池"结构。目前每个项目是 CMake 工程(`deps.yaml` + `CMakeLists.txt` + `CMakePresets.json`),但工作区工具(`setup-env.sh` / `new-project.py`)完全不知道"项目类型"这回事:没有类型声明、不按类型产出 IDE 工程文件。

用户的诉求:

1. 每个项目声明自己的**类型**(如 `vs` = Visual Studio,`as` = Android Studio),工作区工具能**按类型自动生成对应的 IDE 工程**。
2. 现有两个项目(EasyPainter / StickyNotes)都是 `vs` 类型;跑完一键脚本后,**结果里要能出现 Visual Studio 解决方案(`.sln`)**。
3. 新建项目时,`new-project.py` 也要支持按类型生成(类型写进项目清单)。
4. 类型系统要可扩展:以后加新类型(Android Studio 等)只添一个生成器,不动框架。

**范围界定(与用户逐条确认)**:
- **现在只实现 `vs` 生成器**;`as` 作为已登记类型**占位**(生成器报"未实现"并跳过)。
- 工作区目前没有 Android 工程,`as` 具体生成什么(Gradle 骨架)留待有真实需求再做。

## 2. 核心决策(已确认)

| 决策点 | 结论 |
|---|---|
| 项目类型声明位置 | 每个项目自己的 `deps.yaml` 加 `type:` 字段,延续"一项目一清单、自包含"约定 |
| 类型取值 | `vs` / `as`;缺省按 `vs` 兜底(工作区现状) |
| `vs` 的 .sln 产出方式 | 新增 `tools/gen-projects.py`,Windows 上对每个 `type=vs` 项目用 **CMake VS generator 单独 configure 到 `build/vs/`**,只 configure 不编译 |
| 现有 release/debug 构建 | **不受影响**,仍走 Ninja 跑 CLI/golden 测试 |
| `as` 类型 | 已登记占位,生成器报"未实现"跳过 |
| `new-project.py` | 加 `--type vs\|as`(默认 `vs`,仅 cpp 生效),类型写进生成的 deps.yaml |
| 触发时机 | `setup-env.sh` 在池 build 之后自动跑 `gen-projects.py --all`(Linux 下 `vs` 类型自动跳过,不报错) |

## 3. 类型模型

### 3.1 项目清单

每个项目目录的 `deps.yaml` 增加可选字段:

```yaml
type: vs          # vs | as;缺省视为 vs
use: [abseil-cpp, glfw]
```

- 类型是**生成 IDE 工程**的依据,与项目语言/构建系统正交。
- 现有 EasyPainter、StickyNotes 各补一行 `type: vs`。
- 无 `CMakeLists.txt` 的项目(如 python/web)即使标了 `vs` 也会被生成器跳过并提示,不影响工作区其他项目。

### 3.2 类型注册表

新增 `tools/deps_lib/project_gen.py`,核心是一个**生成器注册表**:

```python
GENERATORS = {
    "vs": _gen_vs,   # Windows 上 CMake VS generator 出 .sln
    "as": _gen_as,   # 占位:未实现,调用即返回"未实现"错误
}

def generate(root: str, project: str, type_name: str, variant: str, generator: str | None) -> tuple:
    fn = GENERATORS.get(type_name)
    if fn is None:
        return False, f"未知项目类型: {type_name}"
    return fn(root, project, variant, generator)
```

- 加新类型 = 注册一个新函数,不动扫描/分发框架。
- 每个生成器返回 `(ok, err_or_ok_msg)`;失败只影响当前项目,不中断其它项目。

## 4. `tools/gen-projects.py` 新工具

### 4.1 CLI

```
python3 tools/gen-projects.py --all              # 扫描根目录下所有项目
python3 tools/gen-projects.py --project <dir>    # 只处理指定项目
--variant release|debug                          # 指向池的哪个变体,默认 release
--generator <名>                                 # 覆盖 VS generator 名(调试用)
```

`--all` 与 `--project` 二选一(互斥),风格对齐 `build-deps.py`。

### 4.2 项目扫描

- 根目录(MINE_ROOT)下一层的目录,包含 `deps.yaml` 即视为项目。
- 排除:`tools/ third_party/ docs/ .claude/ .github/ .user-deps/`。
- 按 `deps.yaml` 的 `type:` 分派到对应生成器;缺省按 `vs`。
- 汇总输出:`[GENERATED] <项目> (vs) → build/vs/*.sln` / `[SKIPPED] <项目> (vs) 非 Windows` / `[FAILED] ...` / `[TODO] <项目> (as) 未实现`。

### 4.3 扫描识别逻辑(纯函数,可单测)

```python
def list_projects(root: str) -> list:
    """返回 [(项目目录名, deps.yaml 绝对路径)],排除工具/池/文档目录。"""
def project_type(deps_yaml_path: str) -> str:
    """读 deps.yaml 的 type: 字段,缺省 'vs'。"""
```

## 5. `vs` 生成器

### 5.1 平台门控

- 仅 Windows(MSYS/MINGW)执行;Linux 返回 `(True, "跳过: vs 类型仅 Windows")`。
- 平台判定复用 `pool.on_windows()`。

### 5.2 生成命令

对每个 `type=vs` 项目,先注入 MSVC 环境(复用 `deps_lib/msvc_env.py`),再执行:

```
cmake -S <项目> -B <项目>/build/vs
     -G "<VS generator>" -A x64
     -DCMAKE_CONFIGURATION_TYPES=Release
     [-DCMAKE_PREFIX_PATH=<池 release 前缀;env.sh 的 Qt/Vulkan 前缀>]
```

- **只 configure 不编译**:产出 `<项目>.sln` + `.vcxproj`,configure 失败即报错(带尾部日志,风格同 build-deps)。
- **`-DCMAKE_CONFIGURATION_TYPES=Release`**:生成只含 Release 配置的解决方案,指向池 `release` 变体,避免 Debug/Release 动态 CRT 混链(见 §9 取舍)。
- **`MINE_ROOT` 环境变量**:EasyPainter 靠它定位池(`set(MINE_ROOT "$ENV{MINE_ROOT}" ...)`),必须在 configure 进程环境里注入。
- **`CMAKE_PREFIX_PATH`**:含池 `release` 已建前缀(`cmake_driver._built_prefixes` 复用)+ 合并 `env.sh` 已导出的前缀(Qt/Vulkan)。StickyNotes 靠它 `find_package(Qt6)`。

### 5.3 VS generator 自动发现

运行时探测,不写死版本(用户 VS 是 18/Insiders):

1. `cmake --help` 列出可用的 "Visual Studio" 生成器;
2. 解析生成器名里的版本年份(如 `Visual Studio 17 2022` → 2022),取年份最大者;
3. 允许 `--generator` 覆盖;找不到任何 VS generator 时给出清晰报错(提示装 Build Tools)。

```python
def discover_vs_generator() -> str:
    """返回 cmake --help 里可用的最新 Visual Studio 生成器名;无则空串。"""
```

### 5.4 幂等与失败隔离

- `build/vs/` 已 gitignore;重复运行即重复 configure(CMake 幂等,增量更新 cache)。
- 单项目失败只记 `[FAILED]`,继续处理其它项目;最后非零退出码由 `--all` 汇总。

## 6. 项目侧改动

### 6.1 EasyPainter/CMakeLists.txt(必要的小修)

现状:池依赖目录由 `string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)` 推导。VS generator 是多配置,`CMAKE_BUILD_TYPE` 为空 → `_variant` 为空 → `file(GLOB .../third_party/_install/*/${_variant})` 匹配到 `_install/<name>-<ver>/` 而非 `<variant>/` 子目录 → `find_package(absl)` 找不到库,configure 必挂。

修复(一行):`CMAKE_BUILD_TYPE` 为空时 `_variant` 兜底为 `release`:

```cmake
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
if(NOT _variant)
  set(_variant "release")          # 多配置生成器(VS)下 CMAKE_BUILD_TYPE 恒空
endif()
```

这也让工程对任何多配置生成器(Xcode 等)稳健。

### 6.2 两项目 deps.yaml

EasyPainter、StickyNotes 各加一行 `type: vs`。

## 7. `new-project.py` 扩展

- 新增 `--type vs|as`,默认 `vs`;**仅 cpp 类型生效**;python/web 忽略该参数。
- cpp 模板生成的 `deps.yaml.tmpl` 增加 `type: {{TYPE}}`,`ctx` 注入类型。
- `--type as` 照常生成 cpp 骨架但标 `as`:类型系统闭环,gen-projects 遇 `as` 报"未实现"跳过。
- 校验 `--type` 取值 ∈ {vs, as},否则报错退出。

## 8. `setup-env.sh` 接线

在"池 build(或已就绪跳过)"之后、最终探针之前插入。注意:**必须在 `if ! pool_built; then ... else ... fi` 块之后无条件执行**——否则池早已就绪时(跳过 build 分支)就不会生成 `.sln`:

```bash
# 池就绪判断 + fetch/build 之后
info "=== 生成 IDE 工程(vs → .sln)==="
python3 -u "$MINE_ROOT/tools/gen-projects.py" --all
```

- 依赖池已就绪(此时无论新建还是已存在,池必已 build)。
- Linux:所有 `vs` 项目跳过并打印,脚本继续,不报错。
- 若某项目类型生成失败,`--all` 返回非零 → 脚本失败退出(与 build-deps 的失败语义一致)。

## 9. 复用重构:`tools/deps_lib/msvc_env.py`

`build-deps.py` 现有的 `_vcvars_bat()` / `_msys_linked()` / `_ensure_msvc_env()` 提到新模块:

- `find_vcvars_bat(root) -> str`  —— 原 `_vcvars_bat`
- `is_msys_linked_python() -> bool` —— 原 `_msys_linked`
- `ensure_msvc_env(root) -> bool`  —— 原 `_ensure_msvc_env`(向 os.environ 注入 vcvars 环境)

`build-deps.py` 改为调用 `msvc_env.ensure_msvc_env(MINE_ROOT)`;`gen-projects.py` 在 Windows configure 前同样调用。**行为不变,纯搬移**,原单测改指向新模块。

## 10. 测试

### 单元测试(新增)

- `test_project_gen.py`:
  - 扫描:`list_projects` 识别根目录项目、排除工具/池/文档目录。
  - 类型解析:`project_type` 读 `type:`、缺省 `vs`。
  - vs 生成器:命令构造含 `-G <generator>`、`-A x64`、`-DCMAKE_CONFIGURATION_TYPES=Release`、池 release 前缀、`MINE_ROOT`;Linux 下返回"跳过"。
  - as 占位:返回"未实现"。
  - 非 CMake 目录:跳过并提示。
  - `discover_vs_generator`:mock `cmake --help` 解析出最新 VS generator。
- `test_new_project.py`:`--type vs` 写进生成的 deps.yaml;非法 `--type` 报错。
- `msvc_env` 搬移:原 test_build_deps.py 中 MSVC 相关用例改指向 `msvc_env` 模块,断言不变。

### 集成验证

- 本地 Linux:`python3 -m unittest discover -s tools/tests` 全绿;`gen-projects.py --all` 在 Linux 上各 `vs` 项目报"跳过"。
- Windows(用户机):`setup-env.sh` 跑完后,`EasyPainter/build/vs/EasyPainter.sln` 与 `StickyNotes/build/vs/StickyNotes.sln` 存在,VS 能打开、Release 配置能编译链接池内库。

## 11. 已知取舍 / 后续扩展

- **`.sln` 仅 Release 配置**:v1 指向池 `release` 变体,避免 Debug/Release CRT 混链。要在 VS 里编 Debug,需后续把 EasyPainter 的池目录解析改成 `$<CONFIG>` 感知并连到池 `debug` 变体——v1 不做。
- **`as` 生成器未实现**:已登记占位。真实 Android 工程出现时,在 `GENERATORS` 注册一个 Gradle 骨架生成器即可,框架不动。
- **`vs` 类型 Linux 行为**:Linux 上不生成(没有 VS);同一工程在 Linux 仍按 Ninja 构建,不受影响。
