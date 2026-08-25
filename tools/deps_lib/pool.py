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


def _windows_package(root: str, name: str) -> str:
    """读 deps.yaml 取该 lib 的 windows_package 字段(Windows 用 pacman 包名)。"""
    try:
        import yaml
        m = yaml.safe_load(open(os.path.join(_tp(root), "deps.yaml"), encoding="utf-8"))
        lib = (m.get("libs") or {}).get(name, {})
        return lib.get("windows_package", "") or ""
    except Exception:
        return ""


def is_pacman_provided(root: str, name: str) -> bool:
    """Windows 上该 lib 由 MSYS2 pacman 预编译包提供且已安装 → True。"""
    if not on_windows():
        return False
    pkg = _windows_package(root, name)
    if not pkg:
        return False
    # pacman -Q 检查是否已装
    try:
        r = subprocess.run(["pacman", "-Q", pkg], capture_output=True, timeout=15)
        return r.returncode == 0
    except Exception:
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


def is_built(root: str, name: str, tag: str, variant: str) -> bool:
    """已建且源码指纹一致才视为 built;源码被本地修改(补丁)时触发重编。
    Windows 上由 pacman 预编译包提供的 lib(pacman -Q 已装)直接视为 built。"""
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
        if "src=" not in content:
            # 旧格式 .built:源码 dirty(本地补丁)则重编,否则视为已建
            return "0" == fp.split("|", 1)[1]
        return ("src=" + fp) in content
    except Exception:
        return False
