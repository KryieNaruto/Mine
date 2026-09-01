"""三方库源码拉取:clone 到池 + 状态汇总。"""
from __future__ import annotations

import os
import shutil
import stat
import subprocess
import time
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


GLSLANG_URL = "https://github.com/KhronosGroup/glslang.git"


def _rm_rf(path: str) -> bool:
    """跨平台强制删除文件/目录:先清只读属性,失败重试数次再放弃。

    Windows 上删除可能因只读属性或瞬时文件锁失败(PermissionError);git 子模块
    中断残留的 .git gitfile 是本次痛点,必须删得掉(旧实现在此静默失败,导致 git
    内部重试撞上残留 gitfile 报 "BUG: submodule considered for cloning",本机复现)。
    """
    def _chmod_and_retry(func, p, exc_info):
        try:
            os.chmod(p, stat.S_IWRITE)
        except OSError:
            pass
        try:
            func(p)
        except OSError:
            pass  # 仍删不掉则放弃,由返回值的 lexists 判定
    if not os.path.lexists(path):
        return True
    if os.path.isdir(path) and not os.path.islink(path):
        for _ in range(4):
            shutil.rmtree(path, onerror=_chmod_and_retry)
            if not os.path.lexists(path):
                return True
            time.sleep(0.5)  # 锁通常是瞬时的,重试
        return not os.path.lexists(path)
    for _ in range(4):
        try:
            os.chmod(path, stat.S_IWRITE)
            os.remove(path)
            return True
        except OSError:
            time.sleep(0.5)
    return not os.path.lexists(path)


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


def _glslang_commit(src: str) -> str:
    """读 SwiftShader 索引里固定的 glslang commit SHA(gitlink)。"""
    r = subprocess.run(
        ["git", "-C", src, "ls-tree", "HEAD", "third_party/glslang"],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        return ""
    parts = r.stdout.strip().split()
    return parts[2] if len(parts) >= 3 else ""


def _submodule_ready(src: str) -> bool:
    """glslang 是否已就位可用。现代 git 子模块(.git 为 gitfile 且 gitdir 有效)
    与独立克隆(.git 为目录)都算就位。"""
    sub = os.path.join(src, "third_party", "glslang")
    if not os.path.isdir(sub):
        return False
    # 独立克隆:自带 .git 目录(本次修复的产物),直接可用
    if os.path.isdir(os.path.join(sub, ".git")):
        r = subprocess.run(["git", "-C", sub, "rev-parse", "--git-dir"],
                           capture_output=True, text=True)
        return r.returncode == 0
    # 标准 git 子模块形态:.git 是 gitfile,以 git submodule status 为准。
    # 首字符即状态前缀:「 」(已初始化且与索引一致)、「+」(检出不同 commit)。
    # 注意绝不能 strip() —— 会把就位的空格前缀剥掉,误判未就位(本机已复现)。
    if os.path.isfile(os.path.join(sub, ".git")):
        r = subprocess.run(
            ["git", "-C", src, "submodule", "status", "third_party/glslang"],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            return False
        line = r.stdout or r.stderr
        return line[:1] in (" ", "+")
    return False


def _clone_glslang(src: str, sub: str, commit: str, prefix: str | None) -> tuple:
    """把 glslang 按固定 commit 克隆成独立仓库(自带 .git 目录)。

    完全绕开 git submodule 机制:`git submodule update` 在克隆失败中断后会留下
    半成品 .git gitfile,内部重试撞上它报 "BUG: submodule considered for cloning,
    doesn't need cloning any more?"(git 自身 BUG,本机复现);Windows 上清理该
    gitfile 还可能失败。独立克隆不产生 gitfile —— SwiftShader CMake 判据
    `if(NOT EXISTS <dir>/.git)` 命中目录 .git 即跳过其自身 submodule update。

    浅拉固定 commit:`git init + remote add + fetch --depth 1 origin <sha> +
    checkout FETCH_HEAD`(GitHub 与 ghproxy 镜像都支持按 SHA 浅拉)。镜像→官方兜底;
    全失败再退官方全量 clone + checkout commit(兼容不支持按 SHA 浅拉的源)。
    返回 (ok, err)。
    """
    urls = [mirror.mirror_url(GLSLANG_URL, prefix)] if prefix else []
    urls.append(GLSLANG_URL)
    last = ""
    for u in urls:
        if os.path.isdir(sub):
            _rm_rf(sub)  # 清上一路失败残留(含坏 gitfile),再 init
        # 兜底:整个目录可能因 Windows 文件锁删不掉,至少单独删掉坏 .git gitfile,
        # 让 git init 能在空目录上成功(否则 git init 撞坏 gitfile 直接失败)
        _rm_rf(os.path.join(sub, ".git"))
        r = subprocess.run(["git", "init", "-q", sub], capture_output=True, text=True)
        if r.returncode != 0:
            last = (r.stderr or r.stdout).strip()[-800:]
            continue
        # remote 幂等:已有 origin 则换 URL,否则新增
        gurl = subprocess.run(["git", "-C", sub, "remote", "get-url", "origin"],
                              capture_output=True, text=True)
        if gurl.returncode == 0:
            subprocess.run(["git", "-C", sub, "remote", "set-url", "origin", u],
                           capture_output=True, text=True)
        else:
            subprocess.run(["git", "-C", sub, "remote", "add", "origin", u],
                           capture_output=True, text=True)
        ok = True
        for cmd in (["git", "-C", sub, "fetch", "--depth", "1", "origin", commit],
                    ["git", "-C", sub, "checkout", "-q", "FETCH_HEAD"]):
            rc = subprocess.run(cmd, capture_output=True, text=True)
            if rc.returncode != 0:
                ok = False
                last = (rc.stderr or rc.stdout).strip()[-800:]
                break
        if ok:
            rc = subprocess.run(["git", "-C", sub, "rev-parse", "HEAD"],
                                capture_output=True, text=True)
            if rc.returncode == 0 and rc.stdout.strip() == commit:
                return True, ""
            ok = False
            last = (rc.stderr or "checkout 后 HEAD 与固定 commit 不一致").strip()[-800:]
    # 浅拉全失败(如源不支持按 SHA 拉取):官方全量 clone + checkout commit 兜底
    if os.path.isdir(sub):
        _rm_rf(sub)
    rc = subprocess.run(["git", "clone", GLSLANG_URL, sub], capture_output=True, text=True)
    if rc.returncode == 0:
        rc2 = subprocess.run(["git", "-C", sub, "checkout", commit],
                             capture_output=True, text=True)
        if rc2.returncode == 0:
            return True, ""
        last = (rc2.stderr or rc.stdout).strip()[-800:]
    else:
        last = (rc.stderr or rc.stdout).strip()[-800:]
    return False, last


def ensure_swiftshader_submodules(root: str) -> tuple:
    """确保 swiftshader 的 glslang 子模块就位(其 Vulkan 构建必需)。

    SwiftShader 的 InitSubmodule 在 CMake configure 时对缺 .git 的 glslang 跑
    `git submodule update --init`(全量,数百 MB,国内/弱网下常 502)。这里提前把
    glslang 按索引固定 commit 浅克隆就位;CMake 判据 `if(NOT EXISTS <dir>/.git)`
    命中后直接跳过,不再走网络。

    实现:直接克隆成独立仓库,完全绕开 `git submodule update`——它克隆失败中断
    会留半成品 .git gitfile,内部重试撞上即报 git BUG(本机复现);Windows 上删该
    gitfile 又可能失败。独立克隆不产生 gitfile,天然规避。镜像→官方兜底。
    返回 (ok, err)。
    """
    src = pool.src_dir(root, "swiftshader", "master")
    sub = os.path.join(src, "third_party", "glslang")
    if _submodule_ready(src):
        return True, ""  # 已就位
    if not os.path.isdir(src) or not os.path.isfile(os.path.join(src, ".gitmodules")):
        return True, ""  # swiftshader 未拉取或无需子模块,交给后续流程
    commit = _glslang_commit(src)
    if not commit:
        return False, "读不到 swiftshader 索引中 glslang 的固定 commit(git ls-tree 失败)"

    _clear_mirror_rewrites(src)  # 清陈旧 insteadOf(旧代码可能留 ghproxy.net 的坏改写)
    prefix = mirror.pick_mirror_prefix()
    ok, err = _clone_glslang(src, sub, commit, prefix)
    if not ok and prefix:
        ok, err = _clone_glslang(src, sub, commit, None)  # 镜像失败,退官方直连
    if not ok:
        return False, f"glslang 拉取失败(镜像→官方): {err}"
    # 清父仓残留子模块注册与坏 gitdir,保持仓库整洁(独立克隆后不再需要它们)
    _rm_rf(os.path.join(src, ".git", "modules", "third_party", "glslang"))
    subprocess.run(["git", "-C", src, "config", "--remove-section",
                    "submodule.third_party/glslang"],
                   capture_output=True, text=True)
    return True, ""


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
