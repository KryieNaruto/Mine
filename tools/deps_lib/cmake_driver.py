"""CMake + Ninja 统一预编译驱动。"""
from __future__ import annotations

import os
import shutil
import subprocess
from collections import deque

from . import pool
from .manifest import LibSpec, ver_dir


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
    # Windows 上额外注入 MSYS2 包根(/mingw64),使 find_package 命中 pacman 预编译包
    # (如 mingw-w64-x86_64-abseil-cpp 的 abslConfig.cmake)
    if pool.on_windows() and os.path.isdir("/mingw64/lib/cmake"):
        prefixes.append("/mingw64")
    if prefixes:
        cmd.append("-DCMAKE_PREFIX_PATH=" + ";".join(prefixes))
    return cmd


def _stream(cmd: list, tail_lines: int = 60) -> tuple:
    """运行子进程并逐行实时透传输出(flush),仅保留尾部 tail_lines 行作失败日志。

    背景:build-deps 在管道下 stdout 是块缓冲,capture_output 会把
    cmake/ninja 输出整段吞掉 —— SwiftShader 这类大库编译几十分钟屏幕零输出,
    形似卡死。此处把 stdout/stderr 合并逐行打印,慢则可见进度,真卡则能定位
    卡在哪个命令的最后一行。Windows 下子进程输出可能非 UTF-8,errors='replace'
    防止解码崩。
    """
    tail = deque(maxlen=tail_lines)
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            bufsize=1,
        )
    except FileNotFoundError as e:
        return False, f"命令不存在: {cmd[0]} ({e})"
    assert proc.stdout is not None
    with proc.stdout:
        for line in proc.stdout:
            line = line.rstrip("\n")
            tail.append(line)
            print(line, flush=True)
    rc = proc.wait()
    return rc == 0, "\n".join(tail).strip()


def build_lib(root: str, lib: LibSpec, variant: str, jobs: int) -> tuple:
    """configure + build + install,成功后写 .built。返回 (ok, err_log)。"""
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    os.makedirs(bdir, exist_ok=True)
    os.makedirs(idir, exist_ok=True)

    key = f"{ver_dir(lib.name, lib.tag)} [{variant}]"
    cmds = [
        (f"configure {key}", configure_command(root, lib, variant)),
        (f"build {key}", ["cmake", "--build", bdir, "-j", str(jobs)]),
        (f"install {key}", ["cmake", "--install", bdir]),
    ]
    for phase, cmd in cmds:
        print(f"---- {phase}: {' '.join(cmd)}", flush=True)
        ok, tail = _stream(cmd)
        if not ok:
            return False, f"{phase} 失败:\n{tail}"

    ok, err = _post_install_copy(lib, bdir, idir)
    if not ok:
        return False, err

    with open(os.path.join(idir, ".built"), "w", encoding="utf-8") as f:
        f.write(f"variant={variant}\n")
        f.write(f"src={pool._src_fingerprint(root, lib.name, lib.tag)}\n")
    return True, ""


def _post_install_copy(lib: LibSpec, bdir: str, idir: str) -> tuple:
    """cmake --install 后的库定制落盘。

    SwiftShader 顶层 CMakeLists 没有任何 install() 规则 —— 其 Vulkan ICD
    (`vk_swiftshader_icd.json`) 与动态库产出在 `${CMAKE_BINARY_DIR}/${CMAKE_SYSTEM_NAME}/`
    构建树目录里,`cmake --install` 只装了 SPIRV-Tools。这里把 ICD 及同目录动态库
    拷进池安装前缀,使 `_install/<ver_dir>/<variant>/vk_swiftshader_icd.json` 可达
    (供 win-deps.sh / VK_ICD_FILENAMES 引用)。
    """
    if lib.name != "swiftshader":
        return True, ""
    icd_dir = None
    for sysname in ("Linux", "Windows", "Darwin"):
        cand = os.path.join(bdir, sysname)
        if os.path.isfile(os.path.join(cand, "vk_swiftshader_icd.json")):
            icd_dir = cand
            break
    if icd_dir is None:
        return False, "SwiftShader ICD 未生成: 构建树中未找到 vk_swiftshader_icd.json"
    for fn in os.listdir(icd_dir):
        src = os.path.join(icd_dir, fn)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(idir, fn))
    return True, ""
