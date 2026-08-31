# tools —— 工作空间环境工具

职责:新机器快速搭建环境 —— 检测系统工具、拉取三方库源码、统一预编译进共享池。
**构建/编译是项目自己的事**,tools 只负责把库备好。

## 脚本
| 脚本 | 作用 |
|---|---|
| `setup-env.sh` | 一键搭建:检测缺失→自动安装→拉取并预编译三方库→探针验证 |
| `install-user-deps.sh` | 无 sudo 用户级系统依赖部署(Vulkan/X11/lavapipe/Xvfb/Qt + 工具链) |
| `win-deps.sh` | Windows(Git Bash + MSVC)依赖部署(独立/VS 工具链 + Vulkan + Qt6,SwiftShader 走池) |
| `fetch-deps.py` | 拉取三方库源码进 `third_party/_src/`(只拉不编) |
| `build-deps.py` | 预编译三方库进 `third_party/_install/<name>-<ver>/<variant>/` |
| `new-project.py` | 新建项目骨架(cpp / python / web) |

## 常用命令
    # 新机器一键还原(全链路,幂等)
    tools/setup-env.sh
    # 只检测不安装(CI 用)
    tools/setup-env.sh --check

    # 手动分步(等价于 setup-env.sh 内部流程)
    tools/install-user-deps.sh        # 工具链 + 用户级系统依赖(Windows 自动转交 win-deps.sh)
    tools/fetch-deps.py --all
    tools/build-deps.py --all          # release + debug 双变体

    # 只处理某项目
    tools/fetch-deps.py --project <项目>
    tools/build-deps.py --project <项目>

    # 新建项目
    tools/new-project.py cpp myapp --libs fmt,glm

## 约定
- 全局清单 `third_party/deps.yaml` 是三方库唯一定义处;项目 `deps.yaml` 只 `use` 引用库名。
- 池目录:`_src/<name>-<ver>` 源码、`_install/<name>-<ver>/<variant>` 产物;`.built` 文件标记已编译。
- 源码/产物全部 gitignore,仓库只留清单 + 脚本。
- golden 渲染测试单一基线:双平台逐像素一致校验;不一致 → 修渲染路径,不建分平台基线。
