# {{PROJECT_NAME}}

C/C++ 项目。

## 依赖
- 三方库通过 `deps.yaml` 的 `use` 列表声明(引用全局清单 `third_party/deps.yaml`)。
- 拉取/预编译:`python3 ../tools/fetch-deps.py --project .`、`python3 ../tools/build-deps.py --project .`

## 构建
    cmake --preset release
    cmake --build --preset release
