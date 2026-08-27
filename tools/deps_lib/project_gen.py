"""按项目 deps.yaml 的 type: 字段生成对应 IDE 工程 —— 扫描 + 类型注册表。"""
from __future__ import annotations

import os

from . import manifest

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
