"""CMake + Ninja 统一预编译驱动。"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
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
    if prefixes:
        cmd.append("-DCMAKE_PREFIX_PATH=" + ";".join(prefixes))
    # MSVC 工具链:统一动态 CRT,避免静态/动态 ABI 冲突。恒加、不受 on_windows 影响:
    # Linux 下 CMake 只把它当未消费缓存变量,无副作用;Windows/MSVC 下生效。按 variant
    # 选 /MD 还是 /MDd —— 消费方(如 EasyPainter)在 CMP0091=NEW 下 Debug 配置默认走
    # /MDd,池的 debug 变体若仍编 /MD 会导致 CRT 不一致(MSVC 链接期 LNK2038 或更隐蔽
    # 的堆/ABI 错乱)。
    _runtime = "MultiThreadedDebugDLL" if variant == "debug" else "MultiThreadedDLL"
    cmd.append("-DCMAKE_MSVC_RUNTIME_LIBRARY=" + _runtime)
    # Windows 强制用 MSVC cl,避免 PATH 里残留 g++(MinGW)时 CMake 选错编译器。
    # MSVC 预编译/Qt6 均要求 cl;SwiftShader 等含 __nop() 等 MSVC-only 代码,GCC 编译必崩。
    if pool.on_windows():
        cmd.append("-DCMAKE_C_COMPILER=cl")
        cmd.append("-DCMAKE_CXX_COMPILER=cl")
        # 统一 C++20 + REQUIRED:abseil 的 AbseilDll.cmake 要求 CMAKE_CXX_STANDARD 与
        # CMAKE_CXX_STANDARD_REQUIRED 同时设才走快速路径;只设 STANDARD 会落到
        # check_cxx_source_compiles,而 MSVC 下 try_compile 不传 /std:c++20 →
        # _MSVC_LANG=201402(默认 C++14)探测失败报 "compiler defaults to C++ < 17"
        # (本机已复现)。全池同标准也保证 abseil 消费者(ink-stroke-modeler)ABI 一致。
        cmd.append("-DCMAKE_CXX_STANDARD=20")
        cmd.append("-DCMAKE_CXX_STANDARD_REQUIRED=ON")
    return cmd


def _make_output_safe() -> None:
    """让 stdout/stderr 编码错误处理降级为 replace,print 永不因编码抛异常。

    Windows 控制台是 GBK(cp936)。子进程输出经 text=True + errors="replace" 解码后,
    非法字节变成 U+FFFD;print 回 GBK 时 U+FFFD 无法编码 → UnicodeEncodeError,
    长编译直接中断(本机已崩在 _stream 的 print)。Linux/UTF-8 下无副作用。
    """
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(errors="replace")
        except (AttributeError, ValueError, OSError):
            pass


def _stream(cmd: list, tail_lines: int = 60) -> tuple:
    """运行子进程并逐行实时透传输出(flush),仅保留尾部 tail_lines 行作失败日志。

    背景:build-deps 在管道下 stdout 是块缓冲,capture_output 会把
    cmake/ninja 输出整段吞掉 —— SwiftShader 这类大库编译几十分钟屏幕零输出,
    形似卡死。此处把 stdout/stderr 合并逐行打印,慢则可见进度,真卡则能定位
    卡在哪个命令的最后一行。Windows 下子进程输出可能非 UTF-8,errors='replace'
    防止解码崩;打印侧再经 _make_output_safe 兜底,不因控制台编码中断编译。
    """
    _make_output_safe()
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
