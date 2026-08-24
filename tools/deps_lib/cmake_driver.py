"""CMake + Ninja 统一预编译驱动。"""
from __future__ import annotations

import os
import subprocess

from . import pool
from .manifest import LibSpec


def _built_prefixes(root: str, variant: str) -> list:
    """扫描 _install/*/<variant>/,返回含 .built 标记的安装前缀(字典序稳定)。"""
    install_root = os.path.join(root, "third_party", "_install")
    if not os.path.isdir(install_root):
        return []
    out = []
    for name in sorted(os.listdir(install_root)):
        vdir = os.path.join(install_root, name, variant)
        if os.path.isfile(os.path.join(vdir, ".built")):
            out.append(vdir)
    return out


def configure_command(root: str, lib: LibSpec, variant: str) -> list:
    src = pool.src_dir(root, lib.name, lib.tag)
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    cmd = [
        "cmake", "-S", src, "-B", bdir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=" + variant,
        "-DCMAKE_INSTALL_PREFIX=" + idir,
    ]
    for opt in lib.options:
        cmd.append("-D" + opt)
    # 注入池内已建前缀,使 find_package(absl) 等能命中池产物
    prefixes = _built_prefixes(root, variant)
    if prefixes:
        cmd.append("-DCMAKE_PREFIX_PATH=" + ";".join(prefixes))
    return cmd


def build_lib(root: str, lib: LibSpec, variant: str, jobs: int) -> tuple:
    """configure + build + install,成功后写 .built。返回 (ok, err_log)。"""
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    os.makedirs(bdir, exist_ok=True)
    os.makedirs(idir, exist_ok=True)

    cmds = [
        configure_command(root, lib, variant),
        ["cmake", "--build", bdir, "-j", str(jobs)],
        ["cmake", "--install", bdir],
    ]
    for cmd in cmds:
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return False, "\n".join(x for x in (r.stdout, r.stderr) if x).strip()

    with open(os.path.join(idir, ".built"), "w", encoding="utf-8") as f:
        f.write(f"variant={variant}\n")
    return True, ""
