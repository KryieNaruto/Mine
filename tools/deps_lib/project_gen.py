"""按项目 deps.yaml 的 type: 字段生成对应 IDE 工程 —— 扫描 + 类型注册表。"""
from __future__ import annotations

import os
import re
import shutil
import subprocess

from . import android, cmake_driver, manifest, msvc_env, pool

# 根目录下不是"项目"的目录:工具/池/文档/CI/用户级依赖/超能力笔记/VCS
_EXCLUDE_DIRS = {"tools", "third_party", "docs", ".claude", ".github", ".user-deps", ".superpowers", ".git"}


def list_projects(root: str) -> list:
    """返回 [(项目目录名, deps.yaml 绝对路径)],排除工具/池/文档目录。"""
    out = []
    for name in sorted(os.listdir(root)):
        if name in _EXCLUDE_DIRS or name.startswith("."):
            continue
        project_dir = os.path.join(root, name)
        if not os.path.isdir(project_dir):
            continue
        deps_yaml = os.path.join(project_dir, "deps.yaml")
        if os.path.isfile(deps_yaml):
            out.append((name, deps_yaml))
    return out


def project_type(deps_yaml_path: str) -> str:
    """读 deps.yaml 的 type: 字段,缺省 'vs'。"""
    data = manifest._load_yaml(deps_yaml_path)
    return data.get("type") or "vs"


def discover_vs_generator() -> str:
    """返回 cmake --help 里可用的最新 Visual Studio 生成器名;无则空串。"""
    try:
        out = subprocess.run(["cmake", "--help"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""
    best_year = -1
    best_name = ""
    for line in (out or "").splitlines():
        m = re.search(r"(Visual Studio \d+ (\d{4}))", line)
        if m:
            year = int(m.group(2))
            if year > best_year:
                best_year = year
                best_name = m.group(1)
    return best_name


def _gen_vs(root: str, project: str, variant: str, generator: str | None) -> tuple:
    """vs 生成器:Windows 出 .sln(只 configure);Linux/其他直接 configure + build。"""
    project_dir = os.path.join(root, project)
    if not os.path.isfile(os.path.join(project_dir, "CMakeLists.txt")):
        return True, "跳过: 无 CMakeLists.txt"
    if not pool.on_windows():
        # Linux(或 macOS):直接编译出结果,满足"linux->能编译成功即可"。
        build_dir = os.path.join(project_dir, "build", "release")
        os.environ["MINE_ROOT"] = root  # 池根,CMakeLists 里靠 $ENV{MINE_ROOT} 定位池
        bt = "Debug" if variant == "debug" else "Release"
        if variant == "release" and os.path.isfile(os.path.join(project_dir, "CMakePresets.json")):
            cfg = ["cmake", "--preset", "release", "-S", project_dir]
        else:
            cfg = ["cmake", "-S", project_dir, "-B", build_dir, f"-DCMAKE_BUILD_TYPE={bt}"]
        print(f"---- configure {project} (linux): {' '.join(cfg)}", flush=True)
        ok, tail = cmake_driver._stream(cfg)
        if not ok:
            return False, f"configure 失败:\n{tail}"
        print(f"---- build {project} (linux): cmake --build {build_dir}", flush=True)
        ok, tail = cmake_driver._stream(
            ["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 4)])
        if not ok:
            return False, f"build 失败:\n{tail}"
        return True, f"构建完成: {build_dir}"
    # === 以下为原有 Windows .sln 逻辑,保持不动(gen_name 探测、MSVC 注入、缓存清理、
    #    build/vs 目录、-DCMAKE_CONFIGURATION_TYPES、_stream 调用) ===
    gen_name = generator or discover_vs_generator()
    if not gen_name:
        return False, "未探测到可用的 Visual Studio 生成器(cmake --help 无输出);请安装 VS Build Tools"
    if not msvc_env.ensure_msvc_env(root):
        return False, "MSVC 环境注入失败(vcvars),无法 configure"
    # release/debug 各自独立目录:一次 CMake configure 只能绑定池的一个变体前缀
    # (find_package 一次性解析,多配置生成器下 CMAKE_BUILD_TYPE 恒空,没法像单配置
    # 生成器那样靠它切前缀)。同名复用会导致两个变体互相覆盖 build 目录。
    build_dir = os.path.join(project_dir, "build", "vs" if variant != "debug" else "vs-debug")
    config_type = "Debug" if variant == "debug" else "Release"
    # build_dir 跨多次 regenerate 复用:find_package 的结果(如 glfw3_DIR)一旦被
    # CMakeCache.txt 缓存住,哪怕 CMakeLists.txt 后来改了搜索范围,重新 configure
    # 也不会重新搜——CMake 直接复用缓存里已定住的路径(哪怕是误链到 MinGW glfw3
    # 的那份),新收窄的 find_package 形同虚设(本机复现:上一版收窄 find_package
    # 搜索范围后 libglfw3.a 的 LNK2019 原样重现,根因在此,不在收窄逻辑本身)。每次
    # configure 前清掉配置期缓存强制全新搜索;已编译产物(Debug/Release 下的
    # obj/lib/exe)不在这两处,不受影响——这里只 configure 不编译,不该连带强制
    # 整包重编。
    cache_file = os.path.join(build_dir, "CMakeCache.txt")
    cache_dir = os.path.join(build_dir, "CMakeFiles")
    if os.path.isfile(cache_file):
        os.remove(cache_file)
    if os.path.isdir(cache_dir):
        shutil.rmtree(cache_dir)
    # EasyPainter 等靠 $ENV{MINE_ROOT} 定位池,必须在 configure 进程环境里注入
    os.environ["MINE_ROOT"] = root
    prefixes = cmake_driver._built_prefixes(root, variant)
    cmd = [
        "cmake", "-S", project_dir, "-B", build_dir,
        "-G", gen_name, "-A", "x64",
        "-DCMAKE_CONFIGURATION_TYPES=" + config_type,
    ]
    if prefixes:
        cmd.append("-DCMAKE_PREFIX_PATH=" + ";".join(prefixes))
    print(f"---- configure {project} (vs): {' '.join(cmd)}", flush=True)
    ok, tail = cmake_driver._stream(cmd)
    if not ok:
        return False, f"configure 失败:\n{tail}"
    return True, os.path.join(build_dir, f"{project}.sln")


def _gen_as(root: str, project: str, variant: str, generator: str | None) -> tuple:
    """as(Android Studio)生成器:Windows 探测 SDK 写 local.properties;
    Linux 直接 ./gradlew assembleDebug 编 apk。"""
    project_dir = os.path.join(root, project)
    sdk = android.find_android_sdk()
    if pool.on_windows():
        if not sdk:
            return True, "跳过: 未探测到 Android SDK(运行 tools/android-deps.sh 或设置 ANDROID_HOME 后重试)"
        p = android.write_local_properties(project_dir, sdk)
        return True, f"已写 local.properties(sdk.dir={sdk})→ {p}"
    # Linux:直接构建 apk
    if not sdk:
        return True, "跳过: 未探测到 Android SDK(先运行 tools/android-deps.sh 或设置 ANDROID_HOME)"
    gradlew = os.path.join(project_dir, "gradlew")
    if not os.path.isfile(gradlew):
        return True, "跳过: 无 gradlew(先用 tools/new-project.py as 生成 Android 工程)"
    cmd = ["./gradlew", "assembleDebug"]
    print(f"---- build {project} (as): {' '.join(cmd)} in {project_dir}", flush=True)
    ok, tail = cmake_driver._stream(cmd, cwd=project_dir)
    if not ok:
        return False, f"gradlew assembleDebug 失败:\n{tail}"
    apk = os.path.join(project_dir, "app", "build", "outputs", "apk", "debug", "app-debug.apk")
    return True, f"apk: {apk}"


GENERATORS = {
    "vs": _gen_vs,
    "as": _gen_as,
}


def generate(root: str, project: str, type_name: str, variant: str, generator: str | None) -> tuple:
    """按 type_name 分派到对应生成器;未知类型直接失败。"""
    fn = GENERATORS.get(type_name)
    if fn is None:
        return False, f"未知项目类型: {type_name}"
    return fn(root, project, variant, generator)
