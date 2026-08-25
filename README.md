# Mine 工作空间

快速新开任意项目的工作空间:一项目一文件夹,三方库全局共享、只拉一次、只编一次。

## 结构
    Mine/
    ├── tools/           环境工具(拉取/预编译/脚手架)
    ├── third_party/     全局共享三方库池(清单 deps.yaml + 源码/产物,后者 gitignore)
    └── <项目>/          一项目一文件夹(由 tools/new-project.py 生成)

## 快速开始
    # 1. 一键搭建环境(新机器):工具链 + 系统依赖 + 三方库池,一步到位
    tools/setup-env.sh          # 全链路:检测缺失→自动安装→拉取并预编译三方库→探针验证
    #   tools/setup-env.sh --check   # 只检测不安装(CI 用)

    # 2. 新建项目
    tools/new-project.py cpp myapp --libs fmt,glm
    cd myapp
    cmake --preset release && cmake --build --preset release

## 详见
- 设计:`docs/superpowers/specs/2026-08-23-workspace-bootstrap-design.md`
- 工具用法:`tools/README.md`
