"""三方库源码拉取:clone 到池 + 状态汇总。"""
from __future__ import annotations

import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

from . import MINE_ROOT, manifest, mirror, pool
from .manifest import LibSpec, ver_dir


def clone_lib(root: str, lib: LibSpec):
    """把 lib 源码 clone 到池 _src/<ver_dir>。镜像优先,官方直连兜底。
    返回 (ok, commit_or_err)。"""
    src = pool.src_dir(root, lib.name, lib.tag)
    if os.path.isdir(src):
        return False, "already exists"
    os.makedirs(os.path.dirname(src), exist_ok=True)

    prefix = mirror.pick_mirror_prefix()
    attempts = ([mirror.mirror_url(lib.repo, prefix)] if prefix else []) + [lib.repo]

    ok = False
    last_err = ""
    for url in attempts:
        r = subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", lib.tag, url, src],
            capture_output=True, text=True,
        )
        if r.returncode == 0:
            ok = True
            break
        shutil.rmtree(src, ignore_errors=True)  # 失败残留(镜像断流/半成品),清理后重试下一路
        last_err = (r.stderr or r.stdout).strip()
    if not ok:
        # 全量克隆后 checkout 任意 ref(含 commit sha);只走官方源
        r2 = subprocess.run(["git", "clone", lib.repo, src], capture_output=True, text=True)
        if r2.returncode != 0:
            shutil.rmtree(src, ignore_errors=True)
            return False, (last_err + "\n" + r2.stderr).strip()
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
    """清除 glslang 子模块半成品(反注册 + 删残留 gitdir/工作树/.git gitfile)。

    网络中断(如 502)会让 update 留下指向不完整 .git/modules 的 .git gitfile;
    git 内部重试遇此状态会误判("BUG: submodule considered for cloning, doesn't need
    cloning any more?",本机已复现),不清理直接重试会反复失败。deinit 反注册并删
    工作树,再手动删 .git/modules 下的模块仓库与工作树里的 .git gitfile,让下一次
    update 从零重克隆。
    """
    subprocess.run(
        ["git", "-C", src, "submodule", "deinit", "-f", "third_party/glslang"],
        capture_output=True, text=True,
    )
    # deinit 依赖 gitdir 可解析;残缺 gitfile 会挡路,先删 gitfile → 工作树 → gitdir。
    # Windows 下 rmtree 可能因文件锁静默失败,gitfile 单独 os.remove 兜底。
    gitdir = os.path.join(src, ".git", "modules", "third_party", "glslang")
    for p in (os.path.join(sub, ".git"), sub, gitdir):
        if os.path.isfile(p):
            try:
                os.remove(p)
            except OSError:
                pass
        elif os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
    # 清掉 .git/config 里的子模块注册,让下一次 update 干净地重新 init/clone
    subprocess.run(
        ["git", "-C", src, "config", "--remove-section", "submodule.third_party/glslang"],
        capture_output=True, text=True,
    )


def _set_mirror_rewrite(src: str, prefix: str) -> None:
    """仓库级 url.<prefix>https://github.com/.insteadOf,让本仓库内所有 github
    拉取(含子模块 clone)走镜像。git submodule 继承父仓库的 insteadOf 规则。"""
    subprocess.run(
        ["git", "-C", src, "config", f"url.{prefix}https://github.com/.insteadOf",
         "https://github.com/"],
        capture_output=True, text=True,
    )


def _unset_mirror_rewrite(src: str, prefix: str) -> None:
    subprocess.run(
        ["git", "-C", src, "config", "--unset-all", f"url.{prefix}https://github.com/.insteadOf"],
        capture_output=True, text=True,
    )


def _clear_mirror_rewrites(src: str) -> None:
    """清掉仓库内全部 url.*.insteadOf 改写(任一镜像前缀残留),保证拉取走当前选定源。"""
    r = subprocess.run(
        ["git", "-C", src, "config", "--name-only", "--get-regexp", r"url\..*\.insteadOf"],
        capture_output=True, text=True,
    )
    for key in r.stdout.splitlines():
        subprocess.run(
            ["git", "-C", src, "config", "--unset-all", key],
            capture_output=True, text=True,
        )


def ensure_swiftshader_submodules(root: str) -> tuple:
    """确保 swiftshader 的 glslang 子模块就位(其 Vulkan 构建必需)。

    SwiftShader 的 InitSubmodule 在 CMake configure 时跑 `git submodule update --init`
    (全量,glslang 数百 MB,国内/弱网下 GitHub HTTP 502 常现,本机已复现两次失败)。
    这里提前用 --depth 1 浅克隆 + 重试预取,并利用其 CMake `if(NOT EXISTS <dir>/.git)`
    逻辑:glslang 就位(带 .git 文件/目录)后 configure 直接跳过 submodule update,
    不再走网络。就位判定以 `git submodule status` 为准(现代 git 子模块 .git 是文件
    而非目录,os.path.isdir 会误判);重试前清理半成品,避免中断残留让重试沿用坏状态。

    镜像优先、官方兜底:子模块 clone 走 `git submodule update --init`(借助 insteadOf
    镜像改写),镜像全失败后清掉改写再用官方直连重试 —— 否则只有镜像一路,镜像对
    glslang 挂了(如 ghproxy.net 证书错)就永远失败。每次 update 前 _reset_submodule
    清掉上一轮失败残留的 .git gitfile,规避 git 内部重试的 BUG。
    返回 (ok, err)。
    """
    src = pool.src_dir(root, "swiftshader", "master")
    sub = os.path.join(src, "third_party", "glslang")
    if _submodule_ready(src):
        return True, ""  # 已就位
    if not os.path.isdir(src) or not os.path.isfile(os.path.join(src, ".gitmodules")):
        return True, ""  # swiftshader 未拉取或无需子模块,交给后续流程
    last = ""
    prefix = mirror.pick_mirror_prefix()
    if prefix:
        _clear_mirror_rewrites(src)   # 清陈旧残留,再设当前选定镜像
        _set_mirror_rewrite(src, prefix)
    rounds = [prefix, None] if prefix else [None]  # 镜像 → 官方
    for rnd in rounds:
        for _ in (1, 2, 3):
            _reset_submodule(src, sub)  # 清半成品(含坏 .git gitfile),否则重试沿用坏状态
            r = subprocess.run(
                ["git", "-C", src, "submodule", "update", "--init", "--depth", "1",
                 "third_party/glslang"],
                capture_output=True, text=True,
            )
            if r.returncode == 0 and _submodule_ready(src):
                # 成功路径也要清 insteadOf 改写,否则陈旧的镜像前缀会残留,让本仓库
                # 之后(如 CMake configure 时的 submodule update)仍走镜像而非官方直连。
                if prefix:
                    _clear_mirror_rewrites(src)
                return True, ""
            last = (r.stderr or r.stdout)[-800:]
        if prefix:
            _clear_mirror_rewrites(src)  # 镜像全失败,退官方直连
            prefix = None
    return False, f"glslang 子模块拉取失败(镜像→官方各最多 3 次): {last}"


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
