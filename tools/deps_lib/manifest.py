"""deps.yaml 清单解析:全局定义 + 项目 use 引用合并。"""
from __future__ import annotations

import dataclasses
import os

import yaml


@dataclasses.dataclass(frozen=True)
class LibSpec:
    name: str
    repo: str
    tag: str
    build: str = "cmake"
    options: tuple = ()
    depends_on: tuple = ()
    windows_package: str = ""  # Windows 用 MSYS2 pacman 预编译包名;非空则 Windows 不源码编译

    def __post_init__(self):
        object.__setattr__(self, "options", tuple(self.options or ()))
        object.__setattr__(self, "depends_on", tuple(self.depends_on or ()))


def ver_dir(name: str, tag: str) -> str:
    """由 name + tag 生成池目录名,非 [A-Za-z0-9._-] 字符替换为 '-'。"""
    safe = "".join(c if (c.isalnum() or c in "._-") else "-" for c in str(tag))
    return f"{name}-{safe}"


def _load_yaml(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data or {}


def load_global_manifest(root: str) -> dict:
    path = os.path.join(root, "third_party", "deps.yaml")
    if not os.path.exists(path):
        raise FileNotFoundError(f"全局清单不存在: {path}")
    return _load_yaml(path)


def load_project_manifest(project_dir: str) -> list:
    path = os.path.join(project_dir, "deps.yaml")
    if not os.path.exists(path):
        raise FileNotFoundError(f"项目清单不存在: {path}")
    return _load_yaml(path).get("use", []) or []


def resolve_libs(global_manifest: dict, use: list) -> list:
    libs = global_manifest.get("libs", {}) or {}
    missing = [n for n in use if n not in libs]
    if missing:
        raise KeyError(f"全局清单未定义这些库: {', '.join(sorted(missing))}")
    out = []
    for name in use:
        d = libs[name]
        out.append(LibSpec(
            name=name,
            repo=d["repo"],
            tag=str(d["tag"]),
            build=d.get("build", "cmake"),
            options=d.get("options", []) or [],
            depends_on=d.get("depends_on", []) or [],
            windows_package=d.get("windows_package", "") or "",
        ))
    return out


def all_libs(global_manifest: dict) -> list:
    return resolve_libs(global_manifest, list(global_manifest.get("libs", {}).keys()))


def variants(global_manifest: dict) -> list:
    return global_manifest.get("variants", ["release", "debug"]) or ["release", "debug"]


def extract_windows_packages(global_manifest: dict) -> list:
    """所有声明了 windows_package 的库 → pacman 预编译包名列表(Windows 用)。"""
    return [
        spec.windows_package
        for spec in all_libs(global_manifest)
        if spec.windows_package
    ]
