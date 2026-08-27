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


def _submodule_ready(src: str) -> bool:
    """glslang 子模块是否已就位可用。

    现代 git 把子模块仓库放进父仓 .git/modules/,子模块工作树里只有指向它的 .git
    「文件」——「存在 .git 目录」不是可靠判据(本机复现:浅拉成功后 .git 是 47 字节
    文件,os.path.isdir 误判失败,已拉取仍报错中断 setup)。以 git 自身状态为准:
    `git submodule status` 首字符为空格(已初始化且与索引一致)或 +(检出不同 commit,
    仍可用)即视为就位;`-`(未初始化)或命令失败则未就位。
    """
    sub = os.path.join(src, "third_party", "glslang")
    if not os.path.isdir(sub):
        return False
    r = subprocess.run(
        ["git", "-C", src, "submodule", "status", "third_party/glslang"],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        return False
    # 首字符即状态前缀:「 」(已初始化且与索引一致)、「+」(检出不同 commit)。
    # 注意绝不能 strip() —— 会把就位的空格前缀剥掉,误判未就位(本机已复现)。
    line = r.stdout or r.stderr
    return line[:1] in (" ", "+")


def _reset_submodule(src: str, sub: str) -> None:
    """清除 glslang 子模块半成品(反注册 + 删残留 gitdir/工作树)。

    网络中断(如 502)会让 update 留下指向不完整 .git/modules 的 .git 文件;
    不清理直接重试,git 会沿用坏状态反复失败。deinit 反注册并删工作树,再手动删
    .git/modules 下的模块仓库,让下一次 update 从零重克隆。
    """
    subprocess.run(
        ["git", "-C", src, "submodule", "deinit", "-f", "third_party/glslang"],
        capture_output=True, text=True,
    )
    shutil.rmtree(os.path.join(src, ".git", "modules", "third_party", "glslang"),
                  ignore_errors=True)
    shutil.rmtree(sub, ignore_errors=True)


def ensure_swiftshader_submodules(root: str) -> tuple:
    """确保 swiftshader 的 glslang 子模块就位(其 Vulkan 构建必需)。

    SwiftShader 的 InitSubmodule 在 CMake configure 时跑 `git submodule update --init`
    (全量,glslang 数百 MB,国内/弱网下 GitHub HTTP 502 常现,本机已复现两次失败)。
    这里提前用 --depth 1 浅克隆 + 重试预取,并利用其 CMake `if(NOT EXISTS <dir>/.git)`
    逻辑:glslang 就位(带 .git 文件/目录)后 configure 直接跳过 submodule update,
    不再走网络。就位判定以 `git submodule status` 为准(现代 git 子模块 .git 是文件
    而非目录,os.path.isdir 会误判);重试前清理半成品,避免中断残留让重试沿用坏状态。
    返回 (ok, err)。
    """
    src = pool.src_dir(root, "swiftshader", "master")
    sub = os.path.join(src, "third_party", "glslang")
    if _submodule_ready(src):
        return True, ""  # 已就位
    if not os.path.isdir(src) or not os.path.isfile(os.path.join(src, ".gitmodules")):
        return True, ""  # swiftshader 未拉取或无需子模块,交给后续流程
    last = ""
    for _ in (1, 2, 3):
        _reset_submodule(src, sub)  # 清半成品,否则中断残留会让重试沿用坏状态
        r = subprocess.run(
            ["git", "-C", src, "submodule", "update", "--init", "--depth", "1",
             "third_party/glslang"],
            capture_output=True, text=True,
        )
        if r.returncode == 0 and _submodule_ready(src):
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
