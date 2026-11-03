"""CMake + Ninja 统一预编译驱动。"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from collections import deque

from . import manifest, pool
from .manifest import LibSpec, ver_dir


def _fetchcontent_source_opts(root: str, lib: LibSpec) -> list:
    """给需 FetchContent 的三方库注入本地源码目录,避免 configure 期直连 GitHub。

    ink-stroke-modeler 的 CMakeLists 在 INK_STROKE_MODELER_FIND_GTEST=OFF 时用
    FetchContent 从 https://github.com/google/googletest.git 现拉 googletest(不走
    本脚本的镜像层)。Windows 实测直连失败(Recv failure / Failed to connect)→
    configure 崩、两变体全 FAILED。把 FETCHCONTENT_SOURCE_DIR_GOOGLETEST 指到池内
    已(镜像)拉好的 googletest 源码,FetchContent 直接复用本地目录、不走网络。
    googletest 的拉取由 deps.yaml 的 depends_on 保证在 ink 之前完成。
    清单缺失/目录未就位时静默跳过(宁可走原逻辑报错,也不指错路径)。
    """
    if lib.name != "ink-stroke-modeler":
        return []
    try:
        gm = manifest.load_global_manifest(root)
    except FileNotFoundError:
        return []
    g = (gm.get("libs") or {}).get("googletest")
    if not g:
        return []
    src = os.path.join(root, "third_party", "_src", f"googletest-{str(g.get('tag', ''))}")
    if not os.path.isdir(src):
        return []
    return [f"-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={src}"]


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
    # 部分仓库的 CMakeLists 不在根目录(如 Clipper2 的 CPP/ 子目录),按清单 CMake
    # 子目录配置;标签为 "a/b" 形式时与库名里的 '/' 替换规则保持一致。
    sub = (lib.cmake_subdir or "").strip("/")
    if sub:
        src = os.path.join(src, sub)
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    cmd = [
        "cmake", "-S", src, "-B", bdir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=" + variant,
        "-DCMAKE_INSTALL_PREFIX=" + idir,
    ]
    for opt in lib.options:
        cmd.append("-D" + opt)
    # FetchContent 依赖指到池内源码(ink 的 googletest),避免 configure 期直连 GitHub
    cmd.extend(_fetchcontent_source_opts(root, lib))
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


def _stream(cmd: list, tail_lines: int = 60, cwd: str | None = None) -> tuple:
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
            cwd=cwd,
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


# 纯头文件库:无编译产物,跳过 configure/build/install,由 _post_install_copy 把源码里的
# 头文件直接拷进池前缀。值为「需要拷贝的相对路径」(目录则整棵拷)。
HEADER_ONLY_LIBS = {
    "vma": ["include/vk_mem_alloc.h", "LICENSE.txt"],
}

# 无 CMake 的纯 C 库(libtess2 只有 premake4.lua):
# {"源文件": [...相对源码根], "头文件": [...], "库名": "libtess2"}
SOURCE_OBJECT_LIBS = {
    "libtess2": {
        "sources": [
            "Source/bucketalloc.c", "Source/dict.c", "Source/geom.c",
            "Source/mesh.c", "Source/priorityq.c", "Source/sweep.c", "Source/tess.c",
        ],
        "headers": ["Include/tesselator.h"],
        "lib": "tess2",
    },
}


def _build_source_objects(root: str, lib: LibSpec, variant: str, jobs: int) -> tuple:
    """按硬编码源文件清单编静态库(给没有 CMake 的 C 库用),再 install。

    与 cmake 分支的落盘一致:`<idir>/include/*.h` + `<idir>/lib/<lib>.lib|.a`
    + `<idir>/lib/cmake/<lib>/<lib>Config.cmake`(消费者用 find_package(<lib> CONFIG))。
    """
    src_root = pool.src_dir(root, lib.name, lib.tag)
    spec = SOURCE_OBJECT_LIBS[lib.name]
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    inc, libdir = os.path.join(idir, "include"), os.path.join(idir, "lib")
    os.makedirs(inc, exist_ok=True)
    os.makedirs(libdir, exist_ok=True)

    cflags = ["/MD"] if variant.lower() == "release" else ["/MDd"]
    if variant.lower() != "release":
        cflags.append("/Zi")
    else:
        cflags.append("/O2")
    objs = []
    for rel in spec["sources"]:
        src = os.path.join(src_root, rel)
        if not os.path.isfile(src):
            return False, f"source-objects: 源文件缺失 {src}"
        obj = os.path.join(bdir, os.path.splitext(os.path.basename(src))[0] + ".obj")
        cmd = ["cl", "/nologo", "/c", "/TC", *cflags,
               "/I", os.path.join(src_root, "Include"),
               "/I", os.path.join(src_root, "Source"), "/Fo" + obj, src]
        ok, tail = _stream(cmd)
        if not ok:
            return False, f"source-objects 编译失败 {rel}:\n{tail}"
        objs.append(obj)

    libfile = os.path.join(libdir, spec["lib"] + ".lib")
    ok, tail = _stream(["lib", "/nologo", "/OUT:" + libfile, *objs])
    if not ok:
        return False, f"source-objects 打包失败:\n{tail}"

    for rel in spec["headers"]:
        shutil.copy2(os.path.join(src_root, rel), inc)
    lic = os.path.join(src_root, "LICENSE.txt")
    if os.path.isfile(lic):
        shutil.copy2(lic, idir)

    cdir = os.path.join(libdir, "cmake", spec["lib"])
    os.makedirs(cdir, exist_ok=True)
    with open(os.path.join(cdir, spec["lib"] + "Config.cmake"), "w", encoding="utf-8") as f:
        f.write(
            f'# 由 Mine 工作区 tools/build-deps.py 生成:源码编译的 {spec["lib"]} 静态库\n'
            'if(NOT TARGET ' + spec["lib"] + ')\n'
            '  add_library(' + spec["lib"] + " STATIC IMPORTED)\n"
            '  set_target_properties(' + spec["lib"] + " PROPERTIES\n"
            '    IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../' + spec["lib"] + '.lib"\n'
            '    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../include")\n'
            'endif()\n'
        )
    return True, ""


def build_lib(root: str, lib: LibSpec, variant: str, jobs: int) -> tuple:
    """configure + build + install,成功后写 .built。返回 (ok, err_log)。"""
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    os.makedirs(bdir, exist_ok=True)
    os.makedirs(idir, exist_ok=True)

    key = f"{ver_dir(lib.name, lib.tag)} [{variant}]"
    if lib.build == "source-objects":
        print(f"---- source-objects {key}: 直接编译源文件", flush=True)
        ok, err = _build_source_objects(root, lib, variant, jobs)
        if not ok:
            return False, err
        cmds = []
    elif lib.name in HEADER_ONLY_LIBS:
        print(f"---- header-only {key}: 跳过 configure/build/install", flush=True)
        cmds = []
    else:
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
        # 记录当时构建选项:再改 deps.yaml 的 options 后 is_built/setup-env 探针据此自动重编
        f.write(f"opts={pool.options_sig(lib.options)}\n")
    return True, ""


def _post_install_copy(lib: LibSpec, bdir: str, idir: str) -> tuple:
    """cmake --install 后的库定制落盘。

    SwiftShader 顶层 CMakeLists 没有任何 install() 规则 —— 其 Vulkan ICD
    (`vk_swiftshader_icd.json`) 与动态库产出在 `${CMAKE_BINARY_DIR}/${CMAKE_SYSTEM_NAME}/`
    构建树目录里,`cmake --install` 只装了 SPIRV-Tools。这里把 ICD 及同目录动态库
    拷进池安装前缀,使 `_install/<ver_dir>/<variant>/vk_swiftshader_icd.json` 可达
    (供 win-deps.sh / VK_ICD_FILENAMES 引用)。

    VMA 官方只有 INTERFACE 目标、无 install() 规则 —— 头文件从源码树
    `<repo>/src/vk_mem_alloc.h` 直接拷到 `_install/<ver_dir>/<variant>/include/`。
    """
    if lib.name in HEADER_ONLY_LIBS:
        return _copy_header_only(lib, idir)
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


def _copy_header_only(lib: LibSpec, idir: str) -> tuple:
    """纯头文件库:从 pool 源码树拷指定文件/目录到安装前缀。

    bdir 不可靠(纯头库的 build_dir 没建过),这里用 build_dir 的父级
    `third_party/_build` 反推不可能 —— 直接靠 pool.src_dir(lib 的 root 在 CMakeLists
    层不易拿到),改由调用方在 build_lib 里把源码目录传进来,故此处校验 src 存在。
    """
    if not lib.source_dir:
        return False, f"{lib.name}: 纯头库缺少源码目录信息"
    for rel in HEADER_ONLY_LIBS[lib.name]:
        src = os.path.join(lib.source_dir, rel)
        dst = os.path.join(idir, "include" if rel.endswith(".h") else "", os.path.basename(rel))
        if rel.endswith(".h"):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(src):
            return False, f"{lib.name}: 源码树缺少 {rel}（{src}）"
        shutil.copy2(src, dst)
    return True, ""
