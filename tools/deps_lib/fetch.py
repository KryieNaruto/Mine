"""三方库源码拉取:clone 到池 + 状态汇总。"""
from __future__ import annotations

import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

from . import MINE_ROOT, manifest, pool
from .manifest import LibSpec, ver_dir


def clone_lib(root: str, lib: LibSpec):
    """把 lib 源码 clone 到池 _src/<ver_dir>。返回 (ok, commit_or_err)。"""
    src = pool.src_dir(root, lib.name, lib.tag)
    if os.path.isdir(src):
        return False, "already exists"
    os.makedirs(os.path.dirname(src), exist_ok=True)

    # 主路径:tag/branch 浅克隆
    r = subprocess.run(
        ["git", "clone", "--depth", "1", "--branch", lib.tag, lib.repo, src],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        # 回退:全量克隆后 checkout 任意 ref(含 commit sha)
        r2 = subprocess.run(["git", "clone", lib.repo, src], capture_output=True, text=True)
        if r2.returncode != 0:
            shutil.rmtree(src, ignore_errors=True)
            return False, (r.stderr + "\n" + r2.stderr).strip()
        r3 = subprocess.run(["git", "-C", src, "checkout", lib.tag], capture_output=True, text=True)
        if r3.returncode != 0:
            shutil.rmtree(src, ignore_errors=True)
            return False, r3.stderr.strip()

    rc = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True)
    commit = rc.stdout.strip() if rc.returncode == 0 else ""
    return True, commit


def ensure_swiftshader_submodules(root: str) -> tuple:
    """确保 swiftshader 的 glslang 子模块就位(其 Vulkan 构建必需)。

    SwiftShader 的 InitSubmodule 在 CMake configure 时跑 `git submodule update --init`
    (全量,glslang 数百 MB,国内网络下 HTTP 502 常现,本机已复现两次失败)。这里提前
    用 --depth 1 浅克隆 + 重试,并利用其 CMake `if(NOT EXISTS <dir>/.git)` 逻辑:
    glslang 就位(带 .git)后 configure 直接跳过 submodule update,不再走网络。
    返回 (ok, err)。
    """
    src = pool.src_dir(root, "swiftshader", "master")
    sub = os.path.join(src, "third_party", "glslang")
    if os.path.isdir(os.path.join(sub, ".git")):
        return True, ""  # 已就位
    if not os.path.isdir(src) or not os.path.isfile(os.path.join(src, ".gitmodules")):
        return True, ""  # swiftshader 未拉取或无需子模块,交给后续流程
    last = ""
    for _ in (1, 2, 3):
        r = subprocess.run(
            ["git", "-C", src, "submodule", "update", "--init", "--depth", "1",
             "third_party/glslang"],
            capture_output=True, text=True,
        )
        if r.returncode == 0 and os.path.isdir(os.path.join(sub, ".git")):
            return True, ""
        last = (r.stderr or r.stdout)[-800:]
    return False, f"glslang 子模块拉取失败(3 次尝试): {last}"


def run(root: str, libs: list, jobs: int) -> dict:
    summary = {"fetched": [], "skipped": [], "failed": []}
    lock = pool.load_lock(root)

    def _work(lib):
        key = ver_dir(lib.name, lib.tag)
        if pool.is_fetched(root, lib.name, lib.tag):
            return ("skipped", key)
        ok, msg = clone_lib(root, lib)
        if ok:
            lock[key] = {
                "repo": lib.repo,
                "requested_tag": lib.tag,
                "commit": msg,
                "fetched": True,
            }
            return ("fetched", key)
        return ("failed", f"{key}: {msg}")

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for kind, item in ex.map(_work, libs):
            summary[kind].append(item)

    # SwiftShader 的 glslang 子模块:预先浅克隆就位,避免 CMake configure 时全量拉取 502
    for lib in libs:
        if lib.name == "swiftshader":
            ok, err = ensure_swiftshader_submodules(root)
            if not ok:
                summary["failed"].append(f"swiftshader-master [submodule] {err}")
    pool.save_lock(root, lock)
    return summary


def collect_libs(args) -> list:
    gm = manifest.load_global_manifest(MINE_ROOT)
    if args.project:
        use = manifest.load_project_manifest(args.project)
        return manifest.resolve_libs(gm, use)
    if args.all:
        return manifest.all_libs(gm)
    raise SystemExit("必须指定 --project <dir> 或 --all")
