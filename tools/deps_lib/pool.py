"""第三方库池的路径约定与状态判定。"""
from __future__ import annotations

import json
import os

from .manifest import ver_dir


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


def is_built(root: str, name: str, tag: str, variant: str) -> bool:
    return os.path.isfile(os.path.join(install_dir(root, name, tag, variant), ".built"))
