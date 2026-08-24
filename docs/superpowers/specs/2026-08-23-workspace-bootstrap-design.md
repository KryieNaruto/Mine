# 工作空间脚手架(worskpace-bootstrap)设计

日期:2026-08-23

## 1. 背景与目标

在 `/home/qiansenwei/workspace/Mine/` 下搭建一个可快速新开任意项目的工作空间,核心诉求:

1. **一项目一文件夹** —— 每个项目独立成目录,互不干扰。
2. **三方库全局共享,避免重复拉取与重复编译** —— 所有项目共用一个 `third_party/` 池。同版本库只拉一次源码、只预编译一套产物,多项目复用。
3. **可复现、仓库小** —— 三方库的源码与产物全部 gitignore,仓库只保留清单(`deps.yaml`)与脚本。新机器上靠脚本一键还原环境。

工具链分工(与用户明确对齐):

- **tools = 环境搭建**:负责新机器检测系统依赖、拉取三方库源码、统一预编译(debug/release)进池。
- **项目 = 消费产物**:项目 CMake 通过 `find_package` 链接池内预编译产物,自己不编译三方库。

## 2. 核心决策(已确认)

| 决策点 | 结论 |
|---|---|
| 根目录 | `/home/qiansenwei/workspace/Mine/`(工作区根,即本仓库) |
| tools 是否独立 git | **否**,tools 归属 Mine 工作区仓库 |
| 三方库共享 | 全局共享池 `Mine/third_party/`,跨项目复用 |
| 预编译 | **全部库池内预编译共享**,debug + release 双变体 |
| 清单格式 | YAML,Python 脚本解析(PyYAML) |
| 清单组织 | 全局一份 `third_party/deps.yaml`(单一事实来源)+ 项目 `deps.yaml` 只引用库名 |
| 构建系统 | CMake + Ninja 统一驱动 |
| 项目链接方式 | `find_package` + `CMAKE_PREFIX_PATH` 指向池 install 前缀 |
| 产物变体 | `_install/<name>-<ver>/<variant>/`,variant ∈ {release, debug} |
| gitignore 策略 | `_src/`、`_build/`、`_install/`、`.pool.lock.json` 全忽略,只提交清单+脚本+项目源码 |
| 新机器还原 | `setup-env.sh`(系统工具检测/安装)→ `fetch-deps.py`(拉源码)→ `build-deps.py`(预编译) |

## 3. 总体布局

```
/home/qiansenwei/workspace/Mine/
├── tools/                          # 环境工具中心(拉取 + 统一预编译)
│   ├── setup-env.sh                # 新机器:检测系统工具,缺则 apt 装
│   ├── fetch-deps.py               # 一键拉取:解析清单 → 拉齐池源码
│   ├── build-deps.py               # 一键预编译:每库编 release+debug 进池
│   ├── new-project.py              # 新建项目脚手架(生成项目骨架)
│   ├── deps_lib/                   # 共享 Python 模块
│   │   ├── __init__.py
│   │   ├── manifest.py             # deps.yaml 解析/合并(全局+项目)
│   │   ├── pool.py                 # 池状态查询 + lock 读写
│   │   └── cmake_driver.py         # CMake+Ninja 统一构建驱动
│   ├── templates/                  # 项目模板
│   │   ├── cpp/                    # C/C++ 模板
│   │   ├── python/                 # Python 模板
│   │   └── web/                    # 前端模板(占位)
│   └── README.md                   # 用法 + 约定规范
├── third_party/                    # 全局公用三方库池(共享,跨项目)
│   ├── deps.yaml                   # ★ 三方库清单(全局单一事实来源)
│   ├── _src/<name>-<ver>/          # 源码唯一副本(仅拉取)
│   ├── _build/<name>-<ver>/<variant>/  # cmake 中间构建目录
│   ├── _install/<name>-<ver>/<variant>/  # 预编译产物,debug/release 区分
│   └── .pool.lock.json             # 实际 commit hash + 构建状态标记
├── <项目A>/                        # 一项目一文件夹,由 new-project.py 生成
│   ├── deps.yaml                   # use: [fmt, spdlog, glm]
│   ├── CMakeLists.txt              # find_package + CMAKE_PREFIX_PATH→池 install
│   ├── CMakePresets.json           # Debug/Release 预设 → 对应池产物变体
│   ├── scripts/fetch-deps.py       # 薄封装 → 调 tools/fetch-deps.py
│   ├── scripts/build-deps.py       # 薄封装 → 调 tools/build-deps.py
│   ├── src/…
│   └── README.md
└── <项目B>/ …                      # 复用同一池产物
```

## 4. 目录职责

| 路径 | 职责 | 是否进 git |
|---|---|---|
| `tools/` | 全部环境脚本与模板,唯一的逻辑所在 | ✅ |
| `third_party/deps.yaml` | 全局三方库清单,单一事实来源 | ✅ |
| `third_party/_src/<name>-<ver>/` | 三方库源码唯一副本(git clone 落点) | ❌ |
| `third_party/_build/<name>-<ver>/<variant>/` | cmake 中间构建目录 | ❌ |
| `third_party/_install/<name>-<ver>/<variant>/` | 预编译产物安装前缀 | ❌ |
| `third_party/.pool.lock.json` | 实际 commit hash + 已构建状态 | ❌ |
| `<项目>/` | 项目骨架(源码 + 声明依赖) | ✅ |
| `<项目>/deps.yaml` | 项目依赖声明(仅 `use` 列表) | ✅ |

**版本目录命名规则**:`<name>-<ver>` 中 `<ver>` 取清单 `tag` 字段。若 `tag` 含 `/` 或其它非法路径字符,替换为 `-`。示例:`fmt-10.2.1`、`spdlog-v1.14.1`。

## 5. deps.yaml 清单格式

### 5.1 全局清单 `third_party/deps.yaml`

每库唯一一次定义,字段:

| 字段 | 必填 | 说明 |
|---|---|---|
| `libs.<name>.repo` | ✅ | git 仓库 URL |
| `libs.<name>.tag` | ✅ | 需拉取的 tag / 分支 / commit |
| `libs.<name>.build` | ❌ | 构建方式,默认 `cmake`(初版仅支持 `cmake`) |
| `libs.<name>.options` | ❌ | 传给 cmake 的 `-D<key>=<value>` 列表,如 `[FMT_TEST=OFF]` |
| `variants` | ❌ | 顶层:预编译变体列表,默认 `[release, debug]` |
| `default_variant` | ❌ | 顶层:默认变体,默认 `release` |

```yaml
# third_party/deps.yaml
default_variant: release
variants: [release, debug]

libs:
  fmt:
    repo: https://github.com/fmtlib/fmt.git
    tag: "10.2.1"
    build: cmake
    options: [FMT_TEST=OFF]
  spdlog:
    repo: https://github.com/gabime/spdlog.git
    tag: "v1.14.1"
    build: cmake
    options: [SPDLOG_BUILD_EXAMPLE=OFF]
  glm:
    repo: https://github.com/g-truc/glm.git
    tag: "1.0.1"
    build: cmake
```

### 5.2 项目清单 `<项目>/deps.yaml`

只声明本项目用到哪些库,引用全局清单的定义:

```yaml
# <项目A>/deps.yaml
use: [fmt, spdlog, glm]
```

**合并规则**:脚本以项目 `use` 列表为「需要集」,从全局清单查完整定义。`use` 中出现但全局清单未定义的库名 → 报错并列出,不静默跳过。

## 6. 池状态 `.pool.lock.json`

记录每库实际拉取的 commit 与构建状态,供脚本判断「已拉/已编」以实现去重。

```json
{
  "fmt-10.2.1": {
    "repo": "https://github.com/fmtlib/fmt.git",
    "requested_tag": "10.2.1",
    "commit": "1f6f2e2f07b1b0b4e2d8e6d20d9a4f4a31e0e0f1",
    "fetched": true,
    "built": { "release": true, "debug": false }
  }
}
```

- `fetched: true` 且 `_src/<name>-<ver>/` 存在 → 视为已拉取。
- `built.<variant>: true` 且 `_install/<name>-<ver>/<variant>/.built` 标记文件存在 → 视为已编译该变体。
- 标记文件(`.built`)是权威判定;lock 仅作信息记录,二者不一致时以 `.built` 文件为准。

## 7. 脚本接口与行为

### 7.1 `tools/setup-env.sh`

职责:检测/安装**系统级**工具链(cmake、ninja、g++、pkg-config、git),不涉及三方库。

```
setup-env.sh [--check] [--help]
  --check   只探测不安装,输出缺项清单;硬依赖缺失时非零退出
  -h,--help 打印用法
  默认      探测;硬依赖缺失且在 Debian/Ubuntu(有 apt-get)时 sudo apt 安装
```

探测项与分级:

| 探测项 | 级别 | 最低版本 | apt 包名 |
|---|---|---|---|
| cmake | 硬 | 3.22 | cmake |
| ninja | 硬 | — | ninja-build |
| g++ | 硬 | 11 | build-essential |
| pkg-config | 硬 | — | pkg-config |
| git | 硬 | — | git |
| python3 | 硬 | 3.8 | python3 |

硬依赖缺失 → 尝试 `sudo apt-get install`;失败或非 apt 系统 → 非零退出并打印手动指引。

### 7.2 `tools/fetch-deps.py`

职责:解析清单,把**缺的**三方库源码拉进池。只拉取,不编译。

```
fetch-deps.py [--project <dir> | --all] [--jobs N] [--help]
  --project <dir>   只拉该项目 deps.yaml 声明需集(默认:对 --all 以外需指定)
  --all             拉全局清单全部库
  --jobs N          并行 clone 数,默认 4
```

行为(以 `--project` 为例):
1. 读 `<dir>/deps.yaml` 得 `use` 集;与全局清单合并出完整定义列表。
2. 对每库:若 `_src/<name>-<ver>/` 存在且 lock 中 `fetched=true` → 打印 `SKIP <name>-<ver>`,跳过。
3. 缺失 → `git clone --depth 1 --branch <tag> <repo> _src/<name>-<ver>`;tag 非 branch(如 commit sha)→ `git clone --depth 1` 后 `git fetch --depth 1 origin <tag>` + `git checkout <tag>`。
4. 记录实际 commit 到 `.pool.lock.json`,置 `fetched=true`。
5. 输出汇总表:拉取 N / 跳过 M / 失败 K(失败非零退出)。

### 7.3 `tools/build-deps.py`

职责:对**缺的**变体执行 CMake+Ninja 预编译,装进 `_install/<name>-<ver>/<variant>/`。

```
build-deps.py [--project <dir> | --all] [--variant release|debug|all] [--jobs N] [--help]
  --project <dir>   只编译该项目需集(默认:与 fetch 相同需集规则)
  --all             编译全局清单全部库
  --variant         默认 all(即 release+debug,受清单 variants 字段约束)
  --jobs N          ninja -j N,默认 = CPU 核数
```

行为(单库单变体):
1. 确保源码已拉(fetch 未跑则先拉)。
2. 若 `_install/<name>-<ver>/<variant>/.built` 存在 → `SKIP <name>-<ver> [<variant>]`,跳过。
3. 缺失 → 依次执行:
   - `cmake -S <src> -B <build_dir> -DCMAKE_BUILD_TYPE=<variant> -DCMAKE_INSTALL_PREFIX=<install_dir> [-D<option>...]`
   - `cmake --build <build_dir> -j N`
   - `cmake --install <build_dir>`
   - 写标记文件 `_install/<name>-<ver>/<variant>/.built`
4. 更新 `.pool.lock.json` 对应 `built.<variant>=true`。
5. 输出汇总:已编 / 跳过 / 失败;失败非零退出。

**失败处理**:任一变体失败 → 打印错误与日志位置,继续下一库,最后非零退出并汇总失败列表。

### 7.4 `tools/new-project.py`

职责:生成项目骨架 + 写项目 `deps.yaml` + 薄封装脚本。

```
new-project.py <语言> <项目名> [--libs fmt,spdlog] [--help]
  <语言>   cpp | python | web
  <项目名> 项目目录名,创建于 Mine/ 根
  --libs   逗号分隔的库名,写入项目 deps.yaml 的 use;缺省交互式勾选
```

行为:
1. 校验项目名不重名、合法(仅 `[A-Za-z0-9_-]`)。
2. 从 `tools/templates/<语言>/` 复制骨架到 `Mine/<项目名>/`,替换模板占位符。
3. 生成 `<项目>/deps.yaml`(`use: [...]`)。
4. 生成 `<项目>/scripts/fetch-deps.py`、`build-deps.py` 薄封装(见 7.5)。
5. 输出下一步指引(先跑 `tools/fetch-deps.py --project <项目>`,再 `tools/build-deps.py --project <项目>`)。

### 7.5 薄封装脚本 `<项目>/scripts/fetch-deps.py`

不复制 tools 逻辑,只转发,保证 `MINE_ROOT` 解析一致:

```python
#!/usr/bin/env python3
# 薄封装:转发到 tools/fetch-deps.py,项目内固定 --project 指向本项目
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # Mine/
sys.exit(os.system(f'"{sys.executable}" "{ROOT}/tools/fetch-deps.py" --project "{os.path.dirname(os.path.dirname(os.path.abspath(__file__)))}" {" ".join(sys.argv[1:])}'))
```

`build-deps.py` 薄封装同理。

## 8. 项目侧 CMake 链接方案

生成的项目 `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22)
project(<项目名> CXX)

# 池 install 前缀:优先取缓存变量,否则推导 MINE_ROOT
set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "Mine workspace root")
set(CMAKE_PREFIX_PATH "${MINE_ROOT}/third_party/_install")

# 按需 find_package(由 new-project.py 按 use 集生成)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

add_executable(<项目名> src/main.cpp)
target_link_libraries(<项目名> PRIVATE fmt::fmt spdlog::spdlog glm::glm)
```

**变体对应关系**:项目 `CMAKE_BUILD_TYPE` 必须与池产物变体一致 —— `Debug` preset 链接 `_install/<name>-<ver>/debug`, `Release` preset 链接 `_install/<name>-<ver>/release`。通过 `CMakePresets.json` 固定:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "debug",   "binaryDir": "${sourceDir}/build/debug",   "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug",   "CMAKE_PREFIX_PATH": "${sourceDir}/../third_party/_install" } },
    { "name": "release", "binaryDir": "${sourceDir}/build/release", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "CMAKE_PREFIX_PATH": "${sourceDir}/../third_party/_install" } }
  ],
  "buildPresets": [
    { "name": "debug",   "configurePreset": "debug" },
    { "name": "release", "configurePreset": "release" }
  ]
}
```

**约束与风险**:库的 `find_package` config 在 debug/release 下可能不同(如 MSVC 需要 `-DCMAKE_DEBUG_POSTFIX`);初版在 Linux/GCC 单一 toolchain 下工作,`find_package` 具体 target 名(lib::lib)由各库决定。若某库无 CMake config 或 target 名不同,在项目 CMakeLists 注释说明,或走 `add_library(... INTERFACE)` 兜底。

## 9. gitignore 策略

根 `.gitignore`:

```
# 第三方池:源码 / 中间构建 / 预编译产物 / 状态锁
third_party/_src/
third_party/_build/
third_party/_install/
third_party/.pool.lock.json

# 构建产物
build/
*.o
*.a
*.so
*.dylib

# IDE
.idea/
.vscode/
__pycache__/
*.pyc
```

**提交进 git 的只有**:`tools/` 全部、`third_party/deps.yaml`、各项目骨架(含项目 `deps.yaml`)。新机器 `git clone` 后,通过脚本完全还原 `_src/`、`_build/`、`_install/`。

## 10. 项目模板

### 10.1 `templates/cpp/`

```
cpp/
├── CMakeLists.txt            # 见 §8(占位符 {{PROJECT_NAME}}、{{DEPS_FIND}}、{{DEPS_LINK}})
├── CMakePresets.json         # 见 §8
├── deps.yaml.tmpl            # use: [{{DEPS}}] 由 new-project.py 填充
├── .gitignore
├── README.md                 # 项目说明占位
├── src/
│   └── main.cpp              # 最小可编译入口,打印 "hello from {{PROJECT_NAME}}"
└── scripts/
    ├── fetch-deps.py         # 薄封装
    └── build-deps.py         # 薄封装
```

模板中占位符:以 `{{PLACEHOLDER}}` 形式,new-project.py 用 `str.replace` 替换。

### 10.2 `templates/python/`

```
python/
├── pyproject.toml            # 最小项目(无三方依赖默认)
├── deps.yaml.tmpl            # use: []
├── .gitignore
├── README.md
└── src/
    └── {{PROJECT_NAME}}/
        └── __init__.py       # 空模块
```

初版 python 模板不做第三方池集成(池主要服务 C/C++),pyproject 依赖按项目自定。

### 10.3 `templates/web/`

```
web/
├── package.json              # 最小项目
├── deps.yaml.tmpl            # use: []
├── .gitignore
├── README.md
└── src/
    └── index.html            # 最小页面
```

初版 web 模板为占位,三方依赖走各自生态(npm),不接入池。

## 11. 新机器还原流程

```
git clone <Mine仓库> && cd Mine
tools/setup-env.sh                          # 1. 装系统工具链(缺才装)
tools/fetch-deps.py --all                   # 2. 拉齐全部三方库源码
tools/build-deps.py --all                   # 3. 预编译全部库(release+debug)
# 然后项目侧
cd <项目A> && cmake --preset release && cmake --build --preset release   # 直接链接池产物
```

## 12. 扩展位(初版不实现,仅预留)

| 扩展 | 预留方式 |
|---|---|
| 非 CMake 构建的库(autotools 等) | 清单 `libs.<name>.build` 字段;后续加 `build_cmd`/`install_cmd` |
| 项目级特有构建选项 | 项目 `deps.yaml` 预留可选字段,初版忽略 |
| 池内产物多 toolchain(交叉编译) | `_install/<name>-<ver>/<variant>/` 可扩为 `<variant>-<toolchain>/`,初版不实现 |

## 13. 验收标准

1. `git clone` 空仓库 + 三脚本后,`third_party/_install/<name>-<ver>/release/` 与 `debug/` 均含 `.built`。
2. 同一库二次运行 `fetch-deps.py --all`、`build-deps.py --all` → 全部 `SKIP`,无重复拉取/编译。
3. 新项目 `new-project.py cpp <项目>` 生成骨架,`cmake --preset release` 能链接池产物并编译运行。
4. 仓库 `git status` 干净,`_src/`、`_install/`、`_build/`、`.pool.lock.json` 均被忽略。
