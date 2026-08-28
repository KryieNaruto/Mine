# 一键环境搭建全链路统一(setup-unified)设计

日期:2026-08-27

## 1. 背景与目标

Mine 工作区是"一项目一文件夹 + 三方库全局共享池"结构。现有 `setup-env.sh` 已能完成"工具链 + 系统依赖 + 三方库池"的一键搭建,但存在多个缺口:

1. **入口不便**。Windows 上必须先开 Git Bash 再手动跑 `tools/setup-env.sh`,没有"双击即运行"的入口。
2. **逐项目构建不完整**。`gen-projects.py` 只对 `vs` 项目在 Windows 上生成 `.sln`(只 configure 不编译);Linux 上什么都不做、`as`(Android Studio)类型是占位。
3. **GitHub 拉取无镜像兜底**。`third_party/deps.yaml` 的库直接 clone `github.com`,国内/弱网下 502/超时频发(SwiftShader 的 glslang 子模块尤甚)。
4. **没有 Android 工具链支持**。即使以后有 `as` 项目,也没有 SDK 探测/下载/许可证接受/Gradle/Maven 镜像这一整套。

用户的诉求(新机器首次体验):

1. 我是一台**新机器**(Linux/Windows,自带 VS2026)。
2. 我只需要**双击**,脚本执行。
3. 先查找三方库池所需依赖;根据平台**下载包,优先预编译包,其次自行编译**;必须使用**国内镜像源**。
4. 查找完成、三方库完成时,**开始逐项目构建**:windows→生成 `.sln` 工程;linux→能编译成功即可;android 项目,windows 环境下→生成 `as`(Android Studio)工程,android 项目,linux 环境下→能编译出 apk 即可。
5. Windows 环境下缺少工具时**自动下载安装**;Linux 环境下**禁止使用 sudo**,需要下载包安装。
6. 最终效果:执行脚本后,Windows 上可直接打开某个项目用 IDE 开发;Linux 上可直接编译出结果。

## 2. 范围界定(与用户逐条确认)

| 决策点 | 结论 |
|---|---|
| Android 是否也是宿主机 | **否**,仅是一种项目类型(在 Linux/Windows 宿主机上被构建的目标),脚本本身只在 Linux/Windows 上跑 |
| Android 验证载体 | **新建一个最小可编译的 Android 示例项目**(`HelloAndroid/`),本次改动端到端可用 |
| GitHub 镜像方案 | **ghproxy 类加速代理**(clone URL 改写为镜像前缀,测速挑最快,失败退回官方源) |
| "优先预编译"含义 | **靠现有 `.built` 缓存标记**(首次编一次、以后跳过重建);**不引入**从网上下载二进制这个新机制(vcpkg/conan 均不做) |
| Windows 入口前提 | **假定已预装 Git for Windows**;`setup.bat` 检测不到 `bash.exe` 时报错提示,不自动装 |
| Linux 工具链原则 | 维持现有"装到 `.user-deps`、不碰 sudo"的套路;新加 Android SDK/Gradle/Maven 均走免 sudo 下载 |
| 明确不做 | 不自动装 Git/VS/Android Studio;不做 NDK/Android C++ 构建(示例项目纯 Kotlin) |

## 3. 整体架构与数据流

单一入口触发四步流水线,平台分叉只发生在"工具链探测"和"逐项目构建"两步:

```
setup.bat(Win) / tools/setup-env.sh(Linux)
  → ① 探测/装齐平台工具链(现有 win-deps.sh / install-user-deps.sh;新增 Android SDK cmdline-tools)
  → ② 拉取 + 预编译三方库池(build-deps.py --all;git clone 镜像优先、失败退官方源;已建产物靠 .built 跳过)
  → ③ 逐项目按 (项目类型, 宿主平台) 分派处理
  → ④ 汇总打印结果
```

第③步分派表(在 `project_gen.py` 现有 `GENERATORS` 上扩展平台维度):

| 项目类型 | Windows | Linux |
|---|---|---|
| `vs`(C++) | 生成 `.sln`,不自动编译(现有 `_gen_vs` 不变) | **新增**:配置 + 实际编译 —— 优先 `cmake --preset release`(有 CMakePresets.json 时);否则兜底 `cmake -S <dir> -B <dir>/build/release -DCMAKE_BUILD_TYPE=Release` |
| `as`(Android) | **新增**:探测到 Android SDK → 写 `local.properties`,让工程在 AS 里双击打开即可用 | **新增**:`./gradlew assembleDebug` 实际编译出 apk |

## 4. 平台工具链

### 4.1 Windows(现状 + 新增)

现有 `win-deps.sh`(MSYS2 引导 + Qt MSVC 预编译 + Vulkan SDK 探测/复用/兜底)保持不变。本次新增:

- `setup.bat` 入口:检测 `bash.exe`(Git for Windows),存在则调用 `tools/setup-env.sh`,不存在则报错提示并 `pause`。
- 边界修正:`win-deps.sh` 的 MSYS2 引导必须在"Git Bash 里运行"时也能工作(检测到无 pacman 就自引导 MSYS2 到 `.user-deps`)——覆盖"提示符是 MINGW64 但没有 pacman"的真实机器现状。

### 4.2 Linux(现状 + 新增)

维持"装到 `.user-deps`、免 sudo、PATH 前置"原则,不改现有 C++ 工具链逻辑。新增 Android SDK 下载(见 §5),本身就是免 sudo 的。

## 5. Android 工具链(两条宿主机共用)

新写独立文件 `tools/android-deps.sh`(与 win-deps.sh / install-user-deps.sh 并列,便于单独测试与复用):

- **探测顺序**:`$ANDROID_HOME` / `$ANDROID_SDK_ROOT` → 常见路径(Windows `%LOCALAPPDATA%\Android\Sdk`,Linux `~/Android/Sdk` / `/opt/android-sdk`) → 都没有才下载到 `.user-deps/android-sdk`。
- **下载**:官方 `dl.google.com/android/repository/commandlinetools-<linux|windows>-latest.zip`;不可达时走国内 Android SDK 镜像兜底(与 §6 同一测速套路)。
- **许可证**:`sdkmanager --licenses` 自动接受(`yes |` / `--sdk_root`)。
- **不装 gradle**:用 Gradle Wrapper 自引;`distributionUrl` 指国内镜像(腾讯 `mirrors.cloud.tencent.com/gradle/`)。
- **Maven 依赖仓**:`google()`/`mavenCentral()` 后追加国内镜像兜底:阿里云 `https://maven.aliyun.com/repository/google` 与 `/repository/public`,腾讯 `https://mirrors.cloud.tencent.com/nexus/repository/maven-public/`——Android 构建在国内的三个真实卡点(GitHub 已由 §6 覆盖)即 Google dl / Gradle / Maven。
- **不强制 AS**:Windows 上探测到 SDK 就写 `local.properties`,AS 未装也能先落好工程文件。

## 6. GitHub 源码拉取镜像层

新增 `tools/deps_lib/mirror.py`,复用 `win-deps.sh` 已验证的"候选镜像测速→挑最快"模式:

- 候选:ghproxy 类加速前缀,具体写在 `mirror.py` 顶部常量列表里便于日后更换失效服务(实现时取当前实测可用的 2-3 个,如 `https://ghproxy.net/`、`https://gh-proxy.com/`),拼接规则 `<前缀> + https://github.com/<repo>.git`。
- `fetch.py::clone_lib()`:先测速选一个可达镜像前缀,clone 用镜像 URL;失败(非零)退回官方 `github.com` 直连。镜像不可用绝不卡死。
- SwiftShader 的 glslang 子模块拉取(`ensure_swiftshader_submodules`)复用同一层。
- 测速结果不做全局缓存/复用(每次运行时现测,几个 HEAD 请求),避免陈旧结果(本次话题 CMakeCache 陈旧教训)。

## 7. 示例 Android 项目 + new-project.py 支持

- `new-project.py` 加 `--type as`:生成最小可编译 Android 骨架 —— `settings.gradle`、根 `build.gradle`、`app/` 模块、`app/build.gradle`、`AndroidManifest.xml`、Kotlin `MainActivity`、`gradle/wrapper/`(distributionUrl 指国内镜像)、`.gitignore`。纯 Kotlin,不引入 NDK。
- 版本锁定:AGP / Kotlin / Gradle 用一套互相兼容的稳定版本(写在模板)。
- 仓库新增真实项目 `HelloAndroid/`(`deps.yaml` 声明 `type: as`)作端到端验证载体。
- `project_gen.py::_gen_as()` 从占位改为真实实现:
  - Windows:探测到 SDK → 写 `local.properties`(`sdk.dir=...`);没探测到 → 报错但不挡其他项目。
  - Linux:检测到 `./gradlew` → `./gradlew assembleDebug` 编 apk;SDK 缺失时跳过并提示先跑 tools 那一步。

## 8. 汇总与退出码

沿用现有 `gen-projects.py` 分级汇总,扩展到 4 类:`[GENERATED]`(sln / apk)、`[SKIPPED]`(平台不适用,如 as 在 Windows 只写 local.properties)、`[TODO]`(占位未实现)、`[FAILED]`(报错)。仅 `FAILED` 使脚本非零退出;`setup-env.sh` 末尾探针保持"失败一眼可见、不静默"。

## 9. 测试策略

- 纯逻辑进 `tools/tests/*.py` 单测:分派表、镜像 URL 拼接、local.properties 内容、as 模板生成。
- 端到端可测性:Android 全链路(Linux 编 apk)依赖下载 SDK + Gradle + Maven,重且慢——**不做成 CI 每轮必跑**,标记为手动/可选验证(与"VS 的 MSVC 分支只能靠用户实测"同一原则)。
- Linux C++ 链路维持现有 13/13 ctest 全绿作为回归基线。

## 10. 明确不做(本次 YAGNI)

- 不从网上下载二进制预编译库(靠 `.built` 缓存)。
- 不引入 vcpkg / conan。
- 不做 NDK / Android C++ 构建(示例项目纯 Kotlin)。
- 不自动装 Git for Windows / Android Studio / VS(只探测、提示、或装 SDK 这种命令行可静默装的东西)。
