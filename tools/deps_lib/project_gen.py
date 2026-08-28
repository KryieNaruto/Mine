"""按项目 deps.yaml 的 type: 字段生成对应 IDE 工程 —— 扫描 + 类型注册表。"""
from __future__ import annotations

import os
import re
import subprocess

from . import cmake_driver, manifest, msvc_env, pool

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
    """Windows 上用 CMake VS generator 为 project 生成 .sln;只 configure 不编译。"""
    if not pool.on_windows():
        return True, "跳过: vs 类型仅 Windows"
    project_dir = os.path.join(root, project)
    if not os.path.isfile(os.path.join(project_dir, "CMakeLists.txt")):
        return True, "跳过: 无 CMakeLists.txt"
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
    """as(Android Studio)生成器占位:已登记类型,真实 Android 工程出现前不实现。"""
    return False, "未实现: as(Android Studio)生成器待有真实 Android 工程后实现"


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
