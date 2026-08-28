"""Android SDK 定位与 Android Studio 工程辅助。

SDK 探测顺序:ANDROID_HOME / ANDROID_SDK_ROOT 环境变量 → 常见安装路径
(Windows %LOCALAPPDATA%\\Android\\Sdk、Linux ~/Android/Sdk、/opt/android-sdk)
→ .user-deps/android-sdk(tools/android-deps.sh 的落地目录)。
"""
from __future__ import annotations

import os

from . import MINE_ROOT


def find_android_sdk() -> str | None:
    """返回已安装的 Android SDK 根目录;找不到返回 None。"""
    for env in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        v = os.environ.get(env)
        if v and os.path.isdir(v):
            return v
    home = os.path.expanduser("~")
    local = os.environ.get("LOCALAPPDATA", "")
    # 默认落到 MINE_ROOT/.user-deps(即 tools/android-deps.sh 的落地目录),保证
    # 无 env.sh source 的独立 `gen-projects.py --all` 也能命中;USER_DEPS 可覆盖。
    user_deps = os.environ.get("USER_DEPS", os.path.join(MINE_ROOT, ".user-deps"))
    candidates = []
    if local:
        candidates.append(os.path.join(local, "Android", "Sdk"))
    candidates += [
        os.path.join(home, "Android", "Sdk"),
        "/opt/android-sdk",
        os.path.join(user_deps, "android-sdk"),
    ]
    for c in candidates:
        if os.path.isdir(c):
            return c
    return None


def _escape_properties(path: str) -> str:
    """Java .properties 文件转义:反斜杠(Windows 路径)需加倍。"""
    return path.replace("\\", "\\\\")


def write_local_properties(project_dir: str, sdk_path: str) -> str:
    """写 Android Studio 的 local.properties(sdk.dir),返回文件路径。"""
    p = os.path.join(project_dir, "local.properties")
    with open(p, "w", encoding="utf-8") as f:
        f.write(f"sdk.dir={_escape_properties(sdk_path)}\n")
    return p
