# tools —— 工作空间环境工具

职责:新机器快速搭建环境 —— 检测系统工具、拉取三方库源码、统一预编译进共享池。
**构建/编译是项目自己的事**,tools 只负责把库备好。

## 脚本
| 脚本 | 作用 |
|---|---|
| `setup-env.sh` | 检测/安装系统工具链(cmake/ninja/g++/pkg-config/git/python3) |
| `fetch-deps.py` | 拉取三方库源码进 `third_party/_src/`(只拉不编) |
| `build-deps.py` | 预编译三方库进 `third_party/_install/<name>-<ver>/<variant>/` |
| `new-project.py` | 新建项目骨架(cpp / python / web) |

## 常用命令
    # 新机器还原
    tools/setup-env.sh --check
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
