"""第三方库池的路径约定与状态判定。"""
from __future__ import annotations

import json
import os
import subprocess
import sys

from .manifest import ver_dir


def on_windows() -> bool:
    """是否为 Windows(MSYS2/Git Bash 下 uname -s 报 MSYS*/MINGW*)。"""
    try:
        uname = subprocess.run(["uname", "-s"], capture_output=True, text=True, timeout=5).stdout
        return uname.startswith(("MSYS", "MINGW"))
    except Exception:
        return os.name == "nt"


def is_pacman_provided(root: str, name: str) -> bool:
    """MSVC 工具链下 pacman 的 MinGW 预编译库不兼容 → 恒 False(不再用 pacman 预编译)。"""
    return False


def _tp(root: str) -> str:
    return os.path.join(root, "third_party")


def src_dir(root: str, name: str, tag: str) -> str:
    return os.path.join(_tp(root), "_src", ver_dir(name, tag))


def build_dir(root: str, name: str, tag: str, variant: str) -> str:
    return os.path.join(_tp(root), "_build", ver_dir(name, tag), variant)


def install_dir(root: str, name: str, tag: str, variant: str) -> str:
    return os.path.join(_tp(root), "_install", ver_dir(name, tag), variant)


def lock_path(root: str) -> str:
    return os.path.join(_tp(root), ".pool.lock.json")


def load_lock(root: str) -> dict:
    p = lock_path(root)
    if not os.path.exists(p):
        return {}
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def save_lock(root: str, lock: dict) -> None:
    p = lock_path(root)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(lock, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, p)


def is_fetched(root: str, name: str, tag: str) -> bool:
    return os.path.isdir(src_dir(root, name, tag))


def _src_fingerprint(root: str, name: str, tag: str) -> str:
    """源码 git 指纹:HEAD 短哈希 + dirty 补丁指纹(本地改源码后触发重编)。"""
    src = src_dir(root, name, tag)
    try:
        import subprocess
        head = subprocess.run(
            ["git", "-C", src, "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=10,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", src, "diff", "--stat"], capture_output=True, text=True, timeout=10,
        ).stdout.strip()
        return f"{head}|{len(dirty)}"
    except Exception:
        return ""


def options_sig(options) -> str:
    """构建选项签名:排序后按 '|' 连接,写入 .built 的 opts= 行。

    用于判断 deps.yaml 里某库的 options 是否变过 —— 变了则 is_built/setup-env 探针判定
    需要重编(如 googletest 加 gtest_force_shared_crt 后,旧静态 CRT 池库要自动重建)。
    旧 .built(改版前建的)没有 opts= 行、无法回溯当时选项,按"已建"放行,避免误伤全池
    重编;迁移期对确实变过 options 的库(googletest)需手动删一次 install/build 目录,
    重建后即带 opts 签名,以后再改 options 就能自动失效。
    """
    return "|".join(sorted(options or []))


def is_built(root: str, name: str, tag: str, variant: str, options=None) -> bool:
    """已建且源码指纹一致才算 built;源码被本地修改(dirty)或构建选项(options)变化时重编。"""
    if is_pacman_provided(root, name):
        return True
    bfile = os.path.join(install_dir(root, name, tag, variant), ".built")
    if not os.path.isfile(bfile):
        return False
    fp = _src_fingerprint(root, name, tag)
    if not fp:
        return True  # 无 git 元数据(如手工放置)时退化为只看 .built
    try:
        with open(bfile, "r", encoding="utf-8") as f:
            content = f.read()
        if "src=" in content:
            src_ok = ("src=" + fp) in content
        else:
            # 旧格式 .built:源码 dirty(本地补丁)则重编,否则视为已建
            src_ok = "0" == fp.split("|", 1)[1]
        if options is not None:
            if "opts=" in content:
                return src_ok and (("opts=" + options_sig(options)) in content)
            return src_ok  # 旧 .built 无 opts 记录:无法回溯,按已建放行
        return src_ok
    except Exception:
        return False
