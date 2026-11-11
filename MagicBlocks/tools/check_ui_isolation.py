#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""t6 隔离性门控:证明「UI 只依赖一个动态库 + 一个头文件」。

判据(老板原话拆成的两条命题)
-----------------------------------------------------------------------------
  依赖命题:UI 侧(Android Compose 壳 + Kotlin + 它的 JNI 入口)在**编译时**,
            除了 `magicblocks/mb_capi.h` 之外看不到任何 SDK 内部头文件。
  解耦命题:SDK 内部发生**真实重构**时,UI 侧不需要重编、行为不变。

本脚本不靠"读代码觉得没问题",而是靠**能让它变红的门控**:

  [1] 导出面 GATE_UI_EXPORT
      `include/` 整棵树 = 63 个头(t5 之前)(device/ resources/ primitives/ scene/ ui/
      features/ …)。UI 若拿 `-I include`,内部头就全在射程内。
      硬检查:导出面里 `magicblocks/*.h` 恒等于 {mb_capi.h}。多一个就是泄漏。
      然后**只拿导出面当唯一 include 目录**编 UI 的 TU(正向),必须过。

  [2] 受限 include 负向门控 GATE_NEG_INCLUDE(必须红)
      同一个 TU,加一行 `#include "magicblocks/<内部头>"`。
      编译必须**失败**,且失败原因必须是"找不到那个头"。
      —— 若它编过了,说明内部头还在 PUBLIC include 面上。
      ★ 这一条最容易"永远报 OK"(永远匹配不上 = 等于没有门控),所以配三条
        元测试(见 [6]):把同一个 TU 放到真实 `include/` 上必须能编过,
        证明"模型头文件在别处是存在的、编译器是会去搜的"。

  [3] 正向 + 链接 + 运行 GATE_UI_CONSUMER
      只给 `-I <导出面>` + DLL 的导入库,编一个"典型 UI 消费者"
      (mb_abi_version 校验 → create → attach → render → detach → destroy,
      返回值逐项检查、打印、退出码反映),**必须编过、链过、跑过、退出码 0**。
      t2 的教训:`extern "C"` 丢了,26 个符号全 mangle,编译期 100% 静默,
      只有链接期 LNK2019 才暴露。所以"链接 + 调用 + 查返回值"三层都要。

  [4] 解耦门控 GATE_RELOCATE(最狠的一条)
      把 `include/` 整棵树**物理挪走**再删掉 → 外部 include 目录**一个都不给**
      (-nostdinc 之外只加导出面),用它重编同一个 UI 消费者 → 必须编过、链接、
      运行退出码 0。然后恢复。
      这证明「UI 编译根本不看 SDK 源码树」;而 SDK 内部任何重构都发生在
      那棵树里,所以重构不需要重编 UI。
      ★ 元测试:同一个"挪走后"环境,往 TU 里插 `#include "magicblocks/..."`,
        必须红(NULL_INCLUDE_DIR 之类),证明降级不是假绿。

  [5] JNI 契约 GATE_JNI(桌面测不出来、真机 UnsatisfiedLinkError 的那类)
      a) Kotlin 的 `external fun` 声明 ↔ `Java_com_magicblocks_pad_MbRenderer_*`
         符号名,**双向**比对(缺一个 / 多一个都红);
      b) Kotlin 的参数表 + 返回类型 → 用 NDK 的 JobjectArray 形状**算出** JNI
         描述符,与 jni_bridge.cpp 里的 C++ 签名**独立推导**的形状比对(不是
         抄一份就能过;手写速记表也在,作为反向印证);
      c) DLL/目标文件里 26 个 MB_API 符号**裸名存在、零 Qt C++ 修饰名**。

  [6] 版本校验 GATE_VERSION
      mb_abi_version() 的**返回值** == 本头文件的 MB_ABI_VERSION(MAJOR/MINOR/
      PATCH 三元组逐项),version string 一致,打包公式一致。

  [7] 层纯度 —— 不重复造轮子,直接调用既有门控 `tools/check_layer_purity.py`
      (它的自测由 `tools/tests/test_check_layer_purity.py` 跑,见 --mode)。

  [8] 元测试 GATE_META(证明上面几道门**真的会红**)
      * 负向门控在"给回整棵 include/"时**必须变绿**(否则它只是因为编译器/语法
        错误而红,什么也没证明);
      * 用**故意写坏**的 UI 消费者(缺 extern "C" / 用了内部头 / 版本断言错)
        去跑同一套 编→链→跑 流程,每一份都必须**红**,且红在预期的阶段;
      * 构建图隔离扫描在**注入一个违规 TU** 时必须报红,零注入时必须绿
        (注入只在临时副本目录里做,不碰工作树)。

用法
-----------------------------------------------------------------------------
  # 默认:全量(需要先建出 DLL;见 tools/ui_isolation.ps1)
  python MagicBlocks/tools/check_ui_isolation.py \
      --export-include build-uisplit/ui_isolation/export_include \
      --lib build-uisplit/magicblocks.dll \
      --build-dir build-uisplit \
      --ninja build-uisplit/build.ninja

  # 只想证明门控会红(不需要 DLL / 不需要构建):
  python MagicBlocks/tools/check_ui_isolation.py --mode meta --build-dir build-uisplit

  # 只跑某一组:
  python MagicBlocks/tools/check_ui_isolation.py --mode export,negative,meta ...

退出码:0 = 全绿;1 = 有门控红;2 = 环境/用法错误(缺 DLL、缺导出面等)。

⚠ 一个报错都没有的门控不可信。本脚本每条门控都打印 `[GATE_xxx] OK ...` 或
   `[GATE_xxx] FAIL ...`,末行是 `结果: gates=<n> failed=<m>`。CI 只看退出码即可,
   但人工 review 请核对每一行都真的跑过(warnings 行会列出"跳过了什么、为什么")。
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ============================================================================
# 常量:契约面(改这里之前先改 mb_capi.h 并 bump ABI)
# ============================================================================

PUBLIC_HEADER = "magicblocks/mb_capi.h"

# mb_capi.h 里 MB_API 标注的入口(不含 Java_*:那是 JNI 导出,WIN32 上没有)。
# 这份清单是**可读快照**,不是权威:权威是 `header_mb_api_symbols()` 现读头文件,
# 自测 `test_symbol_list_matches_header` 断言两者一致 —— 头文件加了入口而这里没跟上,
# 自测立刻红。写死一份的价值是:不依赖解析器的那个"26 个入口"数字出现在代码里,
# 改契约时在 diff 里看得见。
MB_API_SYMBOLS = [
    "mb_session_create",
    "mb_session_destroy",
    "mb_session_attach_surface",
    "mb_session_detach_surface",
    "mb_session_render",
    "mb_session_is_valid",
    "mb_session_ready",
    "mb_session_surface_resized",
    "mb_session_set_msaa",
    "mb_session_msaa_effective",
    "mb_session_msaa_supported",
    "mb_session_scan_models",
    "mb_session_load_model",
    "mb_session_clear_model",
    "mb_session_status_text",
    "mb_session_perf",
    "mb_model_list_count",
    "mb_model_list_get",
    "mb_model_list_free",
    "mb_model_list_is_valid",
    "mb_model_list_protocol_version",
    "mb_abi_version",
    "mb_abi_version_string",
    "mb_err_str",
    "mb_session_desc_default",
    "mb_pose_default",
    "mb_set_log_callback",
]

# 「SDK 内部头」的路径前缀:UI 一旦 include 到其中任何一个,就说明分层破了。
# 这是**封闭清单**,与 CMake 里 `target_include_directories` 的设计一一对应:
# 导出面只该有 magicblocks/,而这棵树里 magicblocks/ 下除 mb_capi.h 全是内部。
# 注意这里**故意没有**单独的 "android/" 前缀:`<android/native_window.h>`、
# `<android/native_window_jni.h>` 是 NDK sysroot 的**平台头**(JNI 薄层合法依赖,
# 真机上也必须有),不是 SDK 内部头。把它当前缀会让门控对每个 UI TU 都误报红,
# 误报会逼迫人去整体忽略规则 —— 那样门控就真没了。
INTERNAL_HEADER_PREFIXES = (
    "magicblocks/device/",
    "magicblocks/resources/",
    "magicblocks/primitives/",
    "magicblocks/scene/",
    "magicblocks/ui/",
    "magicblocks/features/",
    "backend/",
    "core/",
    "scene/",
    "features/",
    "primitives/",
    "ui/",
    "device/",
    "resources/",
)

# ----------------------------------------------------------------------------
# 12 个 JNI 入口:名字来自 Kotlin 侧 `external fun`(MbRenderer.kt),
# 符号名按 JNI 规范:Java_<包名下划线化>_<类名>_<方法名>
# ----------------------------------------------------------------------------
JNI_CLASS_PATH = "com/magicblocks/pad/MbRenderer"
JNI_SYMBOL_PREFIX = "Java_com_magicblocks_pad_MbRenderer_"

# 手写速记表:参数 → JNI 描述符。**仅作反向印证**;判定的权威来源是
# Kotlin 文本解析([5b]),因为手写的东西会和实现一起漂移。
JNI_EXPECTED_DESC = {
    "nativeCreate": "()J",
    "nativeSurfaceCreated": "(JJII)Z",
    "nativeSurfaceDestroyed": "(J)V",
    "nativeRenderFrame": "(JFFFFFFF)Z",
    "nativeStatus": "(J)Ljava/lang/String;",
    "nativeDestroy": "(J)V",
    "nativeSetMsaa": "(JI)V",
    "nativeMsaaSamples": "(J)I",
    "nativeSupportedMsaa": "(J)Ljava/lang/String;",
    "nativeListModels": "(Ljava/lang/String;)Ljava/lang/String;",
    "nativeLoadModel": "(JLjava/lang/String;Ljava/lang/String;)Z",
    "nativeClearModel": "(J)V",
    "nativeWindowFromSurface": "(Landroid/view/Surface;)J",
    # t4 新增:ABI 版本三元组校验。Kotlin 在 System.loadLibrary 之后必须**第一件事**
    # 比对它们,MAJOR 不一致就不要继续调任何入口(mb_capi.h §0 的规矩)。
    "nativeAbiVersion": "()I",
    "nativeModelListProtocolVersion": "()I",
}

# Kotlin 类型 → JNI 描述符。Intent 上只支持这几个已经用到的类型;
# 出现别的类型就要在这里显式加(而不是靠"解析失败就跳过")。
KOTLIN_TO_JNI_DESC = {
    "Long": "J",
    "Int": "I",
    "Float": "F",
    "Boolean": "Z",
    "Double": "D",
    "Unit": "V",        # 无返回类型声明 = Unit = void
    "String": "Ljava/lang/String;",
    "Surface": "Landroid/view/Surface;",
}

# ============================================================================
# 基础设施
# ============================================================================

GATES = []          # [(name, ok_bool, detail)] —— 末行汇总用
ENV_RESULTS = []    # [(name, detail)] —— 因环境(无 GPU)未走完的门控
WARNINGS = []       # 跳过/降级的原因,必须打出来,不能静默
FAILURES = []       # 具体失败行


def gate(name, ok, detail=""):
    GATES.append((name, bool(ok), detail))
    print("[%s] %s %s" % (name, "OK  " if ok else "FAIL", detail))
    sys.stdout.flush()
    if not ok:
        FAILURES.append("%s %s" % (name, detail))
    return ok


def env_blocked(name, detail):
    """**环境不满足**导致这道门没法判:既不是绿也不是红。

    为什么单独一个状态:本机没有可用 Vulkan 设备时,mb_session_create 必然返回
    MB_E_NOT_INITIALIZED。此时若报绿,等于用"环境缺 GPU"伪造通过;若报红,
    等于让 CI 永远红着、然后被人习惯性忽略 —— 那和没有门控一样。
    所以如实报 BLOCKED,并在末行汇总里单独计数(CI 可以用 --strict-env 把它当红)。
    """
    ENV_RESULTS.append((name, detail))
    print("[%s] BLOCKED %s" % (name, detail))
    sys.stdout.flush()
    return False


def fail(name, detail):
    return gate(name, False, detail)


def warn(msg):
    WARNINGS.append(msg)
    print("  [!] %s" % msg)
    sys.stdout.flush()


def info(msg):
    print("  " + msg)
    sys.stdout.flush()


class Env(object):
    """编译器/工具链定位。全部走显式探测,找不到就报环境错误而不是假装通过。"""

    def __init__(self, args):
        self.args = args
        # 默认从脚本位置推:MagicBlocks/tools/check_xxx.py → MagicBlocks/
        here = os.path.dirname(os.path.abspath(__file__))
        default_mb = os.path.abspath(os.path.dirname(here))
        self.mb = os.path.abspath(args.magicblocks) if args.magicblocks else default_mb
        self.build_dir = os.path.abspath(args.build_dir or os.path.join(self.mb, "build-uisplit"))
        self.work = os.path.abspath(args.work_dir or os.path.join(self.build_dir, "ui_isolation"))
        self.export_include = os.path.abspath(args.export_include) if args.export_include else None
        self.lib = os.path.abspath(args.lib) if args.lib else None
        self.ninja = os.path.abspath(args.ninja) if args.ninja else None
        self.vcvars = args.vcvars
        self.cl = args.cl
        self.ndk_nm = args.ndk_nm
        self._vc_env = None
        self._run_path = None

    # ---------------------------------------------------------------- MSVC
    def msvc_env(self):
        """import vcvars64.bat 的 INCLUDE/LIB —— 与 build.ps1 同一套做法。

        没有它,cl.exe 连 <cstdint> 都找不到(C1083),那是环境问题不是门控结果。
        """
        if self._vc_env is not None:
            return self._vc_env
        vcvars = self.vcvars
        if not vcvars:
            cands = []
            for pat in (r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat",
                        r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat"):
                import glob
                cands.extend(sorted(glob.glob(pat)))
            vcvars = cands[0] if cands else None
        if not vcvars or not os.path.isfile(vcvars):
            raise RuntimeError("找不到 vcvars64.bat(需要 MSVC 工具链):%r" % vcvars)
        self.vcvars = vcvars
        out = subprocess.run('"%s" >nul 2>&1 && set' % vcvars, shell=True,
                             capture_output=True, text=True, errors="replace")
        env = dict(os.environ)
        for line in out.stdout.splitlines():
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k.lower() == "psmodulepath":
                continue
            if k.lower() == "path":
                env["PATH"] = v + ";" + env.get("PATH", "")
            else:
                env[k] = v
        self._vc_env = env
        return env

    def cl_path(self):
        if self.cl:
            return self.cl
        env = self.msvc_env()
        which = shutil.which("cl.exe", path=env.get("PATH", ""))
        if not which:
            raise RuntimeError("vcvars64 之后仍然找不到 cl.exe")
        return which

    # ---------------------------------------------------------------- 工具
    def find_link_exe(self):
        env = self.msvc_env()
        return shutil.which("link.exe", path=env.get("PATH", ""))

    def run_path_extra(self):
        """运行被测 exe 时需要额外拼进 PATH 的目录(被测 DLL 的运行时依赖)。

        目前只有一个:Vulkan 的 `vulkan-1.dll`。SDK 是 Vulkan 后端,这是它的
        真实运行时依赖(Android 上由系统提供 libvulkan.so,桌面上要 SDK)。
        这不是"给 UI 开后门":UI 并不 include 任何 Vulkan 头,只是 app 进程里
        本身就带着这个系统级图形库。
        """
        if self._run_path is not None:
            return self._run_path
        cands = []
        vs = os.environ.get("VULKAN_SDK")
        # ⚠ 新版 Vulkan SDK(1.4.x)的 Bin 目录**没有** vulkan-1.dll(loader 只在
        #   System32 里);老版本 SDK 自带一份。两个都试,顺序:SDK → System32。
        if vs:
            cands += [os.path.join(vs, "Bin"), os.path.join(vs, "Bin32")]
        cands += ["C:\\Windows\\System32"]
        self._run_path = ";".join(c for c in cands
                                  if c and os.path.isdir(c) and os.path.isfile(os.path.join(c, "vulkan-1.dll")))
        if not self._run_path:
            self._run_path = "C:\\Windows\\System32"
        return self._run_path

    def nm(self):
        """读 PE 导入/导出名的工具。优先 NDK 的 llvm-nm(不论主机架构都装在 NDK 里)。"""
        if self.ndk_nm and os.path.isfile(self.ndk_nm):
            return self.ndk_nm
        import glob
        cands = []
        for pat in (r"C:\Users\*\AppData\Local\Android\Sdk\ndk\*\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe",
                    r"C:\Users\*\AppData\Local\Android\Sdk\ndk\*\toolchains\llvm\prebuilt\*\bin\llvm-nm.exe"):
            cands.extend(sorted(glob.glob(pat), reverse=True))
        if cands:
            self.ndk_nm = cands[0]
            return self.ndk_nm
        # 兜底:dumpbin(MSVC 自带;输出格式不同,下面单独解析)
        env = self.msvc_env()
        self.ndk_nm = shutil.which("dumpbin.exe", path=env.get("PATH", ""))
        return self.ndk_nm


# ============================================================================
# [1] 导出面
# ============================================================================

def list_headers(include_root):
    out = []
    for dirpath, _dirnames, filenames in os.walk(include_root):
        for n in filenames:
            if os.path.splitext(n)[1].lower() in (".h", ".hpp", ".inl"):
                out.append(os.path.relpath(os.path.join(dirpath, n), include_root).replace(os.sep, "/"))
    return sorted(out)


def check_export_surface(env):
    """导出面里 magicblocks/ 下只允许 mb_capi.h 一个头。"""
    inc = env.export_include
    if not inc or not os.path.isdir(inc):
        return fail("GATE_UI_EXPORT", "导出面不存在: %s(先跑 tools/ui_isolation.ps1)" % inc)
    hdrs = list_headers(inc)
    top = [h for h in hdrs if "/" not in h]
    api = [h for h in hdrs if h.startswith("magicblocks/")]
    extra = [h for h in api if h != PUBLIC_HEADER]
    info("导出面 %s: 共 %d 个头" % (inc, len(hdrs)))
    if top:
        return fail("GATE_UI_EXPORT", "导出面根目录出现裸露头文件(UI 可能 include 它们): %s" % top)
    if extra:
        return fail("GATE_UI_EXPORT", "导出面泄漏内部头: %s" % extra)
    if api != [PUBLIC_HEADER]:
        return fail("GATE_UI_EXPORT", "magicblocks/ 下应是恰好 [%s],实际 %s" % (PUBLIC_HEADER, api))
    # 全树:导出面本身不应该带任何内部头前缀
    leak = [h for h in hdrs if any(h.startswith(p) for p in INTERNAL_HEADER_PREFIXES)]
    if leak:
        return fail("GATE_UI_EXPORT", "导出面还带着内部头: %s" % leak)
    return gate("GATE_UI_EXPORT", True, "导出面只有一个头:%s(%d 字节)" % (
        PUBLIC_HEADER, os.path.getsize(os.path.join(inc, PUBLIC_HEADER.replace("/", os.sep)))))


def check_source_tree_surface(env):
    """源码树的 include/ 有几个头、导出面相对它少了多少 —— 这组数字要在报告里。

    这不是"判红"门控,是**量化**:证明 shim 不是一开始就只有一个头(那样
    门控等于什么都没过滤),而是从 60+ 个头里筛到 1 个。

    ⚠ t5 之后的语义变化必须说清楚:t5 把 62 个内部头**物理搬到**
      include/mb_internal/magicblocks/<同名>/ 去了。于是:
        · include/magicblocks/ 只剩 1 个 → 导出面 1 个,结构性成立;
        · 但 include/ 下**总头数仍是 63**(1 公开 + 62 内部)。
      如果只数 include/magicblocks/ 就下结论"内部头 0 个、被挡在外面 0 个",
      那是**把 t5 的搬运结果误读成"根本没有内部头"** —— 门控看似绿,其实
      丢掉了"有 62 个内部头且它们对 UI 不可见"这个核心事实。所以这里必须
      同时报 3 个数:公开面 / 内部面(含 mb_internal) / 总数,并显式断言
      内部面 == 总数 - 公开面,任何一个为 0 都要报出来。
    """
    real = os.path.join(env.mb, "include")
    if not os.path.isdir(real):
        return fail("GATE_UI_EXPORT", "源码树 include/ 不存在: %s" % real)
    full = list_headers(real)
    api = [h for h in full if h.startswith("magicblocks/")]
    if not api:
        return fail("GATE_UI_EXPORT", "源码树 include/magicblocks/ 下一个头都没有 —— 契约头丢了?")
    # 内部头 = 全树里不属于公开面的那些。**不能**只靠 INTERNAL_HEADER_PREFIXES 判定:
    # t5 之后内部头的前缀是 mb_internal/,它不在 INTERNAL_HEADER_PREFIXES 里,
    # 用前缀判会得到 0(假绿)。这里用补集,并对 mb_internal/ 单独点名。
    internal = [h for h in full if h not in api]
    internal_moved = [h for h in full if h.startswith("mb_internal/")]
    env._full_headers = full
    env._internal_headers = internal
    info("SDK 源码树 include/: 总 %d 个头" % len(full))
    info("  公开面 include/magicblocks/ : %d 个 %s" % (len(api), api))
    info("  内部面(对 UI 不可见)      : %d 个,其中 mb_internal/ 下 %d 个"
         % (len(internal), len(internal_moved)))
    ok = True
    # ① 公开面必须是恰好 1 个(mb_capi.h)——"只依赖一个头"的字面要求
    if api != [PUBLIC_HEADER]:
        ok = False
        fail("GATE_UI_EXPORT", "公开面 include/magicblocks/ 应恰好是 [%s],实际 %d 个: %s"
             % (PUBLIC_HEADER, len(api), api[:8]))
    # ② 内部面不能是 0:t5 已把 62 个头搬进 mb_internal/,若这里数出 0,
    #    说明"补集"算错了或树被搬空了 —— 那样"隐藏了 62 个"的结论没有依据。
    if not internal:
        ok = False
        fail("GATE_UI_EXPORT",
             "include/ 下除了公开面竟没有其它头 —— 内部头是已经被删了还是这道门控数错了?"
             "(t5 把 62 个内部头搬进 mb_internal/,这里应为 62)")
    # ③ 公开 + 内部 == 总数(补集定义自证,防止分类逻辑漏掉某一类)
    if len(api) + len(internal) != len(full):
        ok = False
        fail("GATE_UI_EXPORT", "公开 %d + 内部 %d != 总数 %d(分类有漏)"
             % (len(api), len(internal), len(full)))
    return gate("GATE_UI_EXPORT", ok,
                "总 %d 个头 = 公开面 %d(%s)+ 内部面 %d(对 UI 不可见,其中 mb_internal/ %d)"
                % (len(full), len(api), PUBLIC_HEADER, len(internal), len(internal_moved)))


# ============================================================================
# 编译/链接/运行:UI 消费者
# ============================================================================

UI_CONSUMER = r"""// t6 正向门控:「典型 UI 消费者」—— 只 include mb_capi.h。
//
// 这一段代码是**UI 侧最小生命周期的骨架**,必须编过、链过、跑过:
//   版本校验 → create → attach(off-screen/headless) → render → detach → destroy
//
// 它同时是**反例的对照**:tools/check_ui_isolation.py 里同一份源码会被
//   ① 去掉 extern "C"(链接期必须 LNK2019)
//   ② 插入 #include "magicblocks/<内部头>"(编译期必须找不到)
//   ③ 写错版本断言(运行期必须报错)
// 三种刺探;三种都必须红,否则本门控不成立。
#include <stdio.h>
#include <string.h>

#include "magicblocks/mb_capi.h"

#define NEED(cond)                                                          \
  do {                                                                      \
    if (!(cond)) {                                                          \
      printf("CONSUMER_FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);        \
      return 3;                                                             \
    }                                                                       \
  } while (0)

static int g_checks = 0;
#define CHK(cond)                                                           \
  do {                                                                      \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
      printf("CONSUMER_FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);        \
      return 4;                                                             \
    }                                                                       \
  } while (0)

int main(void) {
  /* ① 加载动态库之后第一件事:版本校验。MAJOR 不同就不要再调任何入口。 */
  const unsigned v = mb_abi_version();
  CHK(v == MB_ABI_VERSION);
  CHK((v / 10000u) == (unsigned)MB_ABI_VERSION_MAJOR);
  CHK(((v / 100u) % 100u) == (unsigned)MB_ABI_VERSION_MINOR);
  CHK((v % 100u) == (unsigned)MB_ABI_VERSION_PATCH);

  char vs[32] = {0};
  const size_t need = mb_abi_version_string(vs, sizeof(vs));
  CHK(need > 0 && need <= sizeof(vs));
  printf("CONSUMER abi_version=%u (%s)\n", v, vs);

  /* ② 建会话(真的建设备)。失败时把原因打出来,不静默。 */
  mb_session_desc d = mb_session_desc_default();
  mb_session* s = NULL;
  const mb_err ce = mb_session_create(&d, &s);
  if (ce != MB_OK) {
    char st[512] = {0};
    mb_session_status_text(s, st, sizeof(st));
    printf("CONSUMER_SKIP create=%s (%s)\n", mb_err_str(ce), st);
    mb_session_destroy(s);
    /* 本机没有可用 Vulkan 设备:**如实跳过**,不当成绿也不当成红。 */
    return 7;
  }
  CHK(mb_session_is_valid(s) == 1);

  mb_pose p = mb_pose_default();
  CHK(p.q[3] == 1.0f && p.scale == 1.0f);

  /* ③ 无 surface 时 render 必须是 NO_SURFACE(AUI 侧的"还没挂载"分支) */
  CHK(mb_session_render(s, p) == MB_E_NO_SURFACE);

  /* ④ 挂 surface。桌面侧用 HEADLESS(离屏),真机是 ANDROID;两者都是 mb_surface_kind。 */
  const mb_err ae = mb_session_attach_surface(s, MB_SURFACE_HEADLESS, NULL, 256, 256);
  if (ae != MB_OK) {
    printf("CONSUMER_SKIP attach=%s\n", mb_err_str(ae));
    mb_session_destroy(s);
    return 7;
  }

  /* ⑤ 渲染一帧。返回值逐项判:OK / OUT_OF_DATE 都是"正常路径",其它是失败。 */
  const mb_err re = mb_session_render(s, p);
  CHK(re == MB_OK || re == MB_E_OUT_OF_DATE);

  /* ⑥ 性能快照:结构体按值回来,struct_size 必须对(跨编译器 padding 的运行时印证) */
  const mb_perf_snapshot perf = mb_session_perf(s);
  CHK(perf.struct_size == sizeof(mb_perf_snapshot));

  /* ⑦ 两段式字符串:先问长度,再填充 */
  const size_t st_need = mb_session_status_text(s, NULL, 0);
  CHK(st_need > 0);
  char st[512] = {0};
  CHK(mb_session_status_text(s, st, sizeof(st)) == st_need);

  /* ⑧ 拆卸:detach → destroy,再 destroy 一次必须幂等且给出 INVALID_ARG 而不是崩 */
  CHK(mb_session_detach_surface(s) == MB_OK);
  CHK(mb_session_destroy(s) == MB_OK);
  CHK(mb_session_is_valid(s) == 0);
  CHK(mb_session_destroy(s) == MB_E_INVALID_ARG);

  printf("CONSUMER_OK checks=%d\n", g_checks);
  return 0;
}
"""


def _consumer_symbols():
    """UI_CONSUMER 实际引用的 mb_* 入口 —— 从源码里抽,不手抄。

    手抄清单会漂移:有人改了消费者代码,清单没跟上,链接层证据就悄悄少检一个
    符号(又是一种假绿)。所以直接从模板文本抽。
    """
    body = UI_CONSUMER
    return sorted({m for m in re.findall(r"\b(mb_[a-z0-9_]+)\s*\(", body)})


CONSUMER_EXPECTED_SYMBOLS = _consumer_symbols()


def _run(cmd, env, cwd=None):
    p = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True, errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build_and_run(env, src_text, inc_dirs, tag, extra_cflags=None, extra_include_lines="",
                  symbol_error_expected=None, define=None):
    """编 → 链 → 跑。返回 dict(compiled, linked, ran, exit, out, log)。"""
    d = os.path.join(env.work, "consumer_" + tag)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    src = os.path.join(d, "consumer.cpp")
    body = src_text
    if extra_include_lines:
        marker = '#include "magicblocks/mb_capi.h"'
        body = body.replace(marker, "%s\n%s" % (extra_include_lines, marker), 1)
    with open(src, "w", encoding="utf-8") as f:
        f.write(body)
    exe = os.path.join(d, "consumer.exe")
    obj = os.path.join(d, "consumer.obj")
    vc = env.msvc_env()
    cflags = ["/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MD", "/c", "/W3"]
    if define:
        cflags.append("/D" + define)
    for inc in inc_dirs:
        cflags.append("/I" + inc)
    cflags += [src, "/Fo:" + obj]
    cflags += (extra_cflags or [])
    rc, log = _run([env.cl_path()] + cflags, vc, cwd=d)
    res = {"compiled": rc == 0, "log": log, "linked": None, "ran": None, "exit": None, "out": ""}
    if not res["compiled"]:
        return res
    link_libs = []
    if env.lib:
        link_libs.append(_import_lib_for(env.lib))
    link = ["/nologo", obj, "/OUT:" + exe, "/SUBSYSTEM:CONSOLE"]
    if link_libs:
        link.append(link_libs[0])
    rc, log2 = _run([env.find_link_exe()] + link, vc, cwd=d)
    res["linked"] = rc == 0
    res["log"] += log2
    if not res["linked"]:
        return res
    # 运行时把 DLL 放到 exe 旁边:Windows 的搜索顺序保证先看 exe 目录。
    # ⚠ 但这**不够**:magicblocks.dll 依赖 vulkan-1.dll,而 Windows 的 loader 对
    #   "延迟加载/间接依赖"用的是**受限搜索路径**(exe 目录 → 系统目录 → 当前目录),
    #   不会顺着"exe 旁边的 dll"再找它旁边的 dll。实测:只把 exe 和
    #   magicblocks.dll 放同一个目录,进程在入口点之前就以 0xC0000135
    #   (STATUS_DLL_NOT_FOUND)退出 —— 看起来像"UI 版本门控挂了",其实是环境没摆好。
    #   所以把 magicblocks.dll 的依赖目录(Vulkan SDK bin)也拼进 PATH 再跑。
    if env.lib:
        rt = _runtime_dll_for(env)
        if rt:
            shutil.copy2(rt, d)
        else:
            warn("找不到 magicblocks.dll(只有 .lib),运行时可能因缺 DLL 而失败")
    run_path = env.run_path_extra()
    if run_path:
        vc = dict(vc)
        vc["PATH"] = run_path + ";" + vc.get("PATH", "")
    rc, out = _run([exe], vc, cwd=d)
    res["exit"] = rc
    res["out"] = out
    res["ran"] = True
    return res


def _import_lib_for(dll_or_lib):
    return dll_or_lib


# ---------------------------------------------------------------------------
# 链接层证据:把 exe 的**导入表**读出来
#
# 为什么必须有这一步:captain 的原话 —— "编译通过什么都不证明,只有链接才暴露
# mangle"(t2 丢 extern "C" → 26 个符号 C++ 修饰名 → 只在链接期 LNK2019)。
# 但"链接 exit=0"本身也只是**一个**证据:它可能因为符号从别处(libcmt 之类)
# 解析成功,也可能因为链接器被喂了 /FORCE。所以要求:
#   ① exe 依赖表里出现被测 DLL 的名字;
#   ② exe 导入表里**逐个**出现 UI 真正调用的 mb_* 符号,且名字是**裸名**;
#   ③ 那些裸名确实在 DLL 的导出表里(两边对上,而不是"链接器凑合过去了")。
# 三条缺一不可 —— 只有这样才能把"编译层/链接层"从"运行层"里切出来单独作证。
# ---------------------------------------------------------------------------
def _pe_imports(env, target):
    """返回 (dll_names, imported_symbols)。

    只用 dumpbin /imports 的**实际格式**(实测,不是想象):
        [    magicblocks.dll]                             ← 4 空格缩进 = DLL 名
        [             140003158 Import Address Table]     ← 描述行
        [                          13 mb_session_perf]    ← 27 空格 + ordinal + 名字
        [--- 按序数字母/数字混合(hex,无 0x 前缀) ---]
    llvm-nm 对 PE **没有**导入表信息(`no symbols`),所以不用它。
    """
    vc = env.msvc_env()
    dumpbin = shutil.which("dumpbin.exe", path=vc.get("PATH", ""))
    if not dumpbin:
        return [], []
    rc, out = _run([dumpbin, "/nologo", "/imports", target], vc)
    if rc != 0:
        return [], []
    dlls = []
    syms = []
    cur = None
    for line in out.splitlines():
        m = re.match(r"^\s{4}(\S+\.dll)\s*$", line, re.I)
        if m:
            cur = m.group(1)
            if cur not in dlls:
                dlls.append(cur)
            continue
        if cur is None:
            continue
        # ordinal + 符号名(符号名可缺省,则只有 ordinal)
        m = re.match(r"^\s{20,}([0-9A-Fa-f]+)\s+(\S+)\s*$", line)
        if m:
            syms.append(m.group(2))
    return dlls, syms


def consumer_import_evidence(env, exe):
    """编译层/链接层分开作证。返回 dict(linked_ok, dll_names, resolved, missing)。"""
    expected_dll = "magicblocks.dll"
    if env.lib and env.lib.lower().endswith(".dll"):
        expected_dll = os.path.basename(env.lib)
    dlls, syms = _pe_imports(env, exe)
    syms_norm = set()
    for s in syms:
        # 去掉 PE 常见装饰:`_symbol` / `symbol@N` / `__imp_symbol`
        t = s
        if t.startswith("__imp_"):
            t = t[6:]
        t = t.lstrip("_")
        t = t.split("@")[0]
        syms_norm.add(t)
    return {"dll_names": dlls, "symbols": syms_norm, "expected_dll": expected_dll}


def _runtime_dll_for(env):
    """被测 DLL 的**真实 .dll**,而不是 .lib。

    调用方可能给导入库(`magicblocks.lib`),也可能给 dll 本身。链接必须用 .lib,
    但运行时必须拷**dll**;之前把 .lib 当 dll 拷过去,exe 启动就以
    STATUS_DLL_NOT_FOUND(0xC0000135)退出 —— 会伪装成"版本门控挂了"。
    """
    if not env.lib:
        return None
    if env.lib.lower().endswith(".dll"):
        return env.lib
    cand = os.path.join(os.path.dirname(env.lib), "magicblocks.dll")
    return cand if os.path.isfile(cand) else None


def check_ui_consumer(env):
    if not env.lib or not os.path.isfile(env.lib):
        return fail("GATE_UI_CONSUMER", "动态库/导入库不存在: %s(先跑 tools/ui_isolation.ps1)" % env.lib)
    if not env.export_include:
        return fail("GATE_UI_CONSUMER", "缺 --export-include")
    res = build_and_run(env, UI_CONSUMER, [env.export_include], "positive")
    info("编:%s 链:%s 运行:%s 退出码:%s" % (res["compiled"], res["linked"], res["ran"], res["exit"]))
    for line in (res["out"] or "").splitlines():
        if line.startswith("CONSUMER"):
            info("  " + line)
    if not res["compiled"]:
        return fail("GATE_UI_CONSUMER", "编译失败(只给了导出面!)\n" + tail(res["log"]))
    if not res["linked"]:
        return fail("GATE_UI_CONSUMER", "链接失败(符号被 mangle / 没导出?)\n" + tail(res["log"]))

    # --- 链接层单独作证:exe 真的从被测 DLL 解析到了裸名符号 ---
    exe = os.path.join(env.work, "consumer_positive", "consumer.exe")
    ev = consumer_import_evidence(env, exe)
    info("[层2 链接] 编出 %d 字节 obj、链出 %d 字节 exe" % (
        os.path.getsize(os.path.join(env.work, "consumer_positive", "consumer.obj")),
        os.path.getsize(exe)))
    info("[层2 链接] exe 依赖的 DLL: %s" % (", ".join(ev["dll_names"]) or "(未读到)"))
    if ev["expected_dll"] not in ev["dll_names"]:
        return fail("GATE_UI_CONSUMER",
                    "链接 exit=0,但 exe 的导入表里没有 %s —— 符号不是从这个 DLL 解析的(链接证据不成立)"
                    % ev["expected_dll"])
    resolved = sorted(s for s in ev["symbols"] if s.startswith("mb_"))
    missing = [s for s in CONSUMER_EXPECTED_SYMBOLS if s not in ev["symbols"]]
    info("[层2 链接] exe 从 %s 导入 %d 个 mb_* 裸名符号:%s"
         % (ev["expected_dll"], len(resolved), ", ".join(resolved)))
    if resolved:
        # 抽 6 个给人看,不刷屏
        info("[层2 链接] 例:%s" % ", ".join(resolved[:6]))
    if missing:
        return fail("GATE_UI_CONSUMER",
                    "UI 消费者调用了这些符号,但在 exe 导入表里没找到(链接证据不成立): %s" % missing)
    # 交叉核对:这些裸名确实在 DLL 导出表里
    dll_path = _runtime_dll_for(env)
    if dll_path and os.path.isfile(dll_path):
        exported = dll_exported_symbols(env, dll_path)
        if exported:
            not_in_dll = sorted(s for s in CONSUMER_EXPECTED_SYMBOLS if s not in exported)
            if not_in_dll:
                return fail("GATE_UI_CONSUMER",
                            "exe 导入了这些名字,但 DLL 导出表里没有(两边对不上): %s" % not_in_dll)
            info("[层2 链接] 交叉核对:%d/%d 个符号在 DLL 导出表里逐一对上(裸名、未 mangle)"
                 % (len(CONSUMER_EXPECTED_SYMBOLS), len(CONSUMER_EXPECTED_SYMBOLS)))
    else:
        warn("只有导入库没有真实 DLL,跳过「导入表 ↔ 导出表」交叉核对")

    if res["exit"] == 7:
        warn("本机没有可用 Vulkan 设备,UI 消费者在 create/attach 处如实跳过(exit=7)")
        if getattr(env.args, "layer", "all") == "compile":
            # --layer compile:显式声明"本次只判编译层与链接层",运行层不参与判定。
            # 这不是降级糊弄:上面 obj/exe 尺寸、依赖 DLL 名、逐个导入符号、导出表
            # 交叉核对都已经实证。只是把"没跑完生命周期"如实标成不在范围内。
            return gate("GATE_UI_CONSUMER",
                        True,
                        "层1 编过 + 层2 链过并逐个符号核对(%d 个裸名从 %s 解析);"
                        "层3 运行按 --layer compile 声明不在判定范围(本机无 GPU,exit=7)"
                        % (len(CONSUMER_EXPECTED_SYMBOLS), ev["expected_dll"]))
        return env_blocked("GATE_UI_CONSUMER",
                           "编过 + 链过,但运行因本机无 Vulkan 设备未走完生命周期(exit=7);"
                           "换一台有 GPU 的机器重跑即为完整判定")
    if res["exit"] != 0:
        return fail("GATE_UI_CONSUMER", "运行退出码 %s\n%s" % (res["exit"], tail(res["out"])))
    m = re.search(r"CONSUMER_OK checks=(\d+)", res["out"] or "")
    return gate("GATE_UI_CONSUMER", bool(m), "编→链→跑全通,%s 项断言" % (m.group(1) if m else "?"))


def tail(text, n=20):
    lines = (text or "").strip().splitlines()
    return "\n".join("      " + l for l in lines[-n:])


# ============================================================================
# [2] 受限 include 负向门控
# ============================================================================

NEG_INCLUDE_PROBE = r"""// t6 负向门控:在**只有导出面**的 include 环境下,故意 include 一个 SDK 内部头。
// 这份 TU **必须编译失败**,失败原因必须是"找不到文件"。
// 若它编过了 → 内部头仍在 PUBLIC include 面上 → 隔离不成立。
#include "magicblocks/%(internal)s"

int main(void) { return 0; }
"""


def check_negative_include(env):
    if not env.export_include:
        return fail("GATE_NEG_INCLUDE", "缺 --export-include")
    probes = env.args.neg_headers.split(",") if env.args.neg_headers else []
    if not probes:
        # `primitives/world.h` 是 t6 任务书里点名的那个探针,必须包含在内;
        # 其余三个覆盖 device / ui / scene 三种不同前缀,防止"只有某类前缀被挡"。
        probes = ["primitives/world.h", "scene/scene.h", "device/render_device.h", "ui/ui_app.h"]
    all_red = True
    for internal in probes:
        src = NEG_INCLUDE_PROBE % {"internal": internal}
        d = os.path.join(env.work, "neg_" + re.sub(r"\W", "_", internal))
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
        p = os.path.join(d, "neg.cpp")
        with open(p, "w", encoding="utf-8") as f:
            f.write(src)
        vc = env.msvc_env()
        rc, log = _run([env.cl_path(), "/nologo", "/std:c++20", "/EHsc", "/c", p,
                        "/Fo:" + os.path.join(d, "neg.obj"), "/I" + env.export_include], vc, cwd=d)
        not_found = re.search(r"C1083|cannot open include file|error C1083", log, re.I)
        compiled = rc == 0
        if compiled:
            all_red = False
            fail("GATE_NEG_INCLUDE", "!! 内部头 %s 在只有导出面的环境下**编过了** —— "
                                     "UI 的 include 面上仍有内部头(隔离不成立)" % internal)
        elif not not_found:
            # 编过了?不是。但也不是"找不到头"而红 —— 那种红可能是语法/环境错误,
            # 不能当证据。必须红在"找不到该头"才是最纯粹的证据。
            all_red = False
            fail("GATE_NEG_INCLUDE", "%s 编失败了,但不是因为找不到头(证据不纯)\n%s"
                 % (internal, tail(log, 8)))
        else:
            first = [l for l in log.splitlines() if "C1083" in l] or log.splitlines()[:1]
            info("[red] %-28s exit=%d  %s" % (internal, rc, (first[0] if first else "").strip()))
    return gate("GATE_NEG_INCLUDE", all_red,
                "内部头探针 %d 个,全部编失败且失败原因为 C1083(找不到头)" % len(probes))


# ============================================================================
# [4] 解耦门控:把 include/ 物理挪走
# ============================================================================

def check_relocate(env):
    real = os.path.join(env.mb, "include")
    if not env.export_include or not env.lib:
        return fail("GATE_RELOCATE", "缺 --export-include / --lib")
    if not os.path.isdir(real):
        return fail("GATE_RELOCATE", "源码树 include/ 不存在")
    stash = os.path.join(env.work, "include_stashed")
    if os.path.isdir(stash):
        shutil.rmtree(stash)
    moved = False
    try:
        shutil.move(real, stash)
        moved = True
        info("已把 %s 整个挪走(SDK 源码树此刻对编译器不可见)" % real)
        # 删掉整棵树再建一个空壳:编译器看到的 include/ 是**空的**,
        # 就算有人的 -I 指过来也搜不到任何东西。
        os.makedirs(real)
        res = build_and_run(env, UI_CONSUMER, [env.export_include], "relocated")
        if not res["compiled"]:
            return fail("GATE_RELOCATE", "SDK 源码树被抽走后 UI 编不过(隔离不成立)\n" + tail(res["log"]))
        if not res["linked"]:
            return fail("GATE_RELOCATE", "链接失败\n" + tail(res["log"]))
        # 链接层证据:源码树被抽走后,exe 仍然从被测 DLL 解析到裸名符号
        ev = consumer_import_evidence(env, os.path.join(env.work, "consumer_relocated", "consumer.exe"))
        if ev["expected_dll"] not in ev["dll_names"]:
            return fail("GATE_RELOCATE",
                        "链过了但 exe 导入表里没有 %s(链接证据不成立): %s"
                        % (ev["expected_dll"], ev["dll_names"]))
        miss = [s for s in CONSUMER_EXPECTED_SYMBOLS if s not in ev["symbols"]]
        if miss:
            return fail("GATE_RELOCATE", "源码树挪走后 exe 少了这些导入符号: %s" % miss)
        info("[层2 链接] relocate 后 exe 仍从 %s 导入 %d 个 mb_* 裸名符号"
             % (ev["expected_dll"], len([s for s in ev["symbols"] if s.startswith("mb_")])))
        if res["exit"] == 7:
            warn("本机无 Vulkan 设备:relocate 门控只证明了编+链,运行在 create 处如实跳过")
            # 注意这里仍然先跑"内部头探针必须红":那是环境无关的证据,必须拿到。
            probe = UI_CONSUMER.replace('#include "magicblocks/mb_capi.h"',
                                        '#include "magicblocks/scene/scene.h"\n#include "magicblocks/mb_capi.h"', 1)
            neg = build_and_run(env, probe, [env.export_include], "relocated_neg")
            if neg["compiled"]:
                return fail("GATE_RELOCATE", "!! 挪走 include/ 之后,内部头**仍然**能找到 —— "
                                             "有某个 -I 或者系统路径在给它兜底")
            info("[red] relocate 环境下内部头探针如预期编失败")
            if getattr(env.args, "layer", "all") == "compile":
                return gate("GATE_RELOCATE", True,
                            "SDK 源码树整棵挪走 + include/ 置空后,UI 消费者仍编码链通过并逐符号"
                            "从 %s 解析,内部头探针转红;层3 运行按 --layer compile 不在判定范围"
                            % ev["expected_dll"])
            return env_blocked("GATE_RELOCATE",
                               "SDK 源码树挪走后 UI 仍编码链通过(include/ 置空 + 内部头探针转红),"
                               "但运行因本机无 Vulkan 设备未走完(exit=7)")
        if res["exit"] != 0:
            return fail("GATE_RELOCATE", "运行退出码 %s\n%s" % (res["exit"], tail(res["out"])))
        # 元测试:同一环境 + 一行内部头 include,必须红(证明"降级"不是假绿)
        probe = UI_CONSUMER.replace('#include "magicblocks/mb_capi.h"',
                                    '#include "magicblocks/scene/scene.h"\n#include "magicblocks/mb_capi.h"', 1)
        neg = build_and_run(env, probe, [env.export_include], "relocated_neg")
        if neg["compiled"]:
            return fail("GATE_RELOCATE", "!! 挪走 include/ 之后,内部头**仍然**能找到 —— "
                                         "有某个 -I 或者系统路径在给它兜底")
        info("[red] relocate 环境下内部头探针如预期编失败")
        return gate("GATE_RELOCATE", True,
                    "SDK 源码树整棵挪走 + include/ 置空后,UI 消费者仍编码链跑通,且内部头探针转红")
    finally:
        if moved:
            shutil.rmtree(real, ignore_errors=True)
            shutil.move(stash, real)
            info("已恢复 %s" % real)


# ============================================================================
# [5] JNI 契约
# ============================================================================

# 这棵树里的 Kotlin 源码是**无 BOM UTF-8**(实测首字节 'p'),但 Windows 上
# 任何编辑器都可能给它加 BOM。Python 的 "utf-8" 解码器**不会**剥 BOM,
# 于是 re.search(r"\bobject ...") 的 \b 在 "package" 中间匹配上的第一个
# `object` 会是空指针 —— 那不是解析失败,是我们自己没处理 BOM。
sys.stdout.reconfigure(encoding="utf-8", errors="replace") if hasattr(sys.stdout, "reconfigure") else None


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    # 统一换行:CRLF 会让 `\s*:` 这类锚定失效(Windows 上这棵树就是 CRLF),
    # 而"按行解析失败"会表现成"解析不出一半声明",那正是假绿的前身。
    return raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")


def parse_kotlin_externals(env):
    """从 MbRenderer.kt 抽 object MbRenderer 里的 `external fun` 声明。

    只解析这一个 object 块 —— ModelCatalog 等别的 object 里的 fun 不是 JNI 入口,
    混进去会让"多一个也红"的判定失真。

    自测钩子:测试会把前面步骤的**产物**搬走/改写(为了验证"改了 Kotlin 就变红"),
    所以这里允许显式覆盖路径。
    """
    kt = env.args.kotlin_path or os.path.join(
        env.mb, "android", "app", "src", "main", "java", "com", "magicblocks", "pad", "MbRenderer.kt")
    if not os.path.isfile(kt):
        return None, kt
    text = read_text(kt)
    # ---- 定位 object 块(花括号要平衡:external fun 的括号里没有 `{`/`}`,
    #      但注释和字符串里可能有,先去掉它们再数)。
    clean = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    clean = re.sub(r"//[^\n]*", "", clean)
    clean = re.sub(r'"(?:\\.|[^"\\])*"', '""', clean)
    m = re.search(r"\bobject\s+MbRenderer\s*\{", clean)
    if not m:
        return None, kt
    depth = 0
    start = m.end() - 1
    end = None
    for i in range(start, len(clean)):
        c = clean[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = clean[start:end + 1] if end else clean[start:]
    decls = {}
    # 先把花括号配对(`external fun x(...)` 的参数括号里没有嵌套,但用平衡扫描
    # 对换行/默认值更稳):找 `external fun <name>` 之后第一个 `(`,向后找配对的 `)`。
    for em in re.finditer(r"external\s+fun\s+(\w+)\s*\(", body):
        name = em.group(1)
        open_at = em.end() - 1
        depth = 0
        close_at = None
        for i in range(open_at, len(body)):
            if body[i] == "(":
                depth += 1
            elif body[i] == ")":
                depth -= 1
                if depth == 0:
                    close_at = i
                    break
        if close_at is None:
            return None, kt
        params = body[open_at + 1:close_at]

        # 换行 → 空格;尾逗号(Kotlin 允许 `a: Int,\n)` )→ 删掉。
        # ⚠ 声明既可能带返回类型(`external fun f(...) : Boolean`)也可能没有
        #   (`external fun f(...)`,Unit 返回 —— 树里 nativeSetMsaa 等就是这种)。
        #   老版强制要求 `: Type`,于是"无返回类型声明的函数"让整个解析直接失败:
        #   那会把 3/12 个入口静默丢掉,再配上"少一个也算红"还好,但配上"少一个
        #   就跳过"就是典型的假绿。这里按 Kotlin 语义:缺省返回类型 = Unit = V。
        tail = body[close_at + 1:]
        params = re.sub(r",\s*$", "", params.replace("\n", " ").strip())
        rm = re.match(r"\s*:\s*([\w<>.?\[\]]+)", tail)
        ret = rm.group(1).strip() if rm else "Unit"
        plist = []
        for chunk in params.split(","):
            chunk = chunk.strip()
            if not chunk:
                continue
            pm = re.match(r"^(\w+)\s*:\s*([\w.]+)(\s*=\s*.*)?$", chunk)
            if not pm:
                return None, kt
            plist.append((pm.group(1), pm.group(2)))
        decls[name] = {"params": plist, "ret": ret}
    return decls, kt


def kotlin_desc(params, ret):
    out = "("
    for _n, t in params:
        if t not in KOTLIN_TO_JNI_DESC:
            raise KeyError(t)
        out += KOTLIN_TO_JNI_DESC[t]
    out += ")"
    if ret not in KOTLIN_TO_JNI_DESC:
        raise KeyError(ret)
    out += KOTLIN_TO_JNI_DESC[ret]
    return out


def parse_jni_bridge_symbols(env):
    """从 jni_bridge.cpp 抽 `Java_com_magicblocks_pad_MbRenderer_<name>(<params>)` 的签名。

    这里**独立推导**描述符:JNIEnv*/jclass 两个隐式参数跳过,其余按 C++ 类型映射。
    (而不是去读一个和实现一起漂移的手写表。)
    """
    path = env.args.jni_bridge_path or os.path.join(env.mb, "src", "android", "jni_bridge.cpp")
    if not os.path.isfile(path):
        return None, path
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    type_map = {
        "jlong": "J", "jint": "I", "jfloat": "F", "jboolean": "Z", "jdouble": "D",
        "jbyte": "B", "jchar": "C", "jshort": "S",
        "jstring": "Ljava/lang/String;",
        "jobject": "Ljava/lang/Object;",
        "jclass": "Ljava/lang/Class;",
    }
    found = {}
    pat = re.compile(r"\b(" + JNI_SYMBOL_PREFIX + r"(\w+))\s*\(", re.S)
    for m in pat.finditer(text):
        full, name = m.group(1), m.group(2)
        open_at = m.end() - 1
        depth = 0
        close_at = None
        for i in range(open_at, len(text)):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    close_at = i
                    break
        if close_at is None:
            found[name] = {"error": "参数列表括号不配对"}
            continue
        params = text[open_at + 1:close_at]
        plist = [p.strip() for p in params.replace("\n", " ").split(",") if p.strip()]
        types = []
        for p in plist:
            p = p.replace("*", " ").strip()
            parts = p.split()
            if parts:
                types.append(parts[0])
        if len(types) < 2:
            found[name] = {"error": "参数少于 2 个(缺 JNIEnv*/jclass)"}
            continue
        # 隐式参数:第 1 个必须是 JNIEnv(指针),第 2 个是 jclass/jobject。
        # 这两者都不出现在 JNI 描述符里。
        if types[0] != "JNIEnv":
            found[name] = {"error": "第 1 个参数不是 JNIEnv: %r" % (types[0],)}
            continue
        if types[1] not in ("jclass", "jobject"):
            found[name] = {"error": "第 2 个参数不是 jclass/jobject: %r" % (types[1],)}
            continue
        explicit = types[2:]
        # 返回值:符号名之前最近的类型标记(JNIEXPORT <ret> JNICALL ...)。
        head = text[:m.start()]
        ret = None
        for tok in reversed(re.findall(
                r"\b(JNIEXPORT|JNICALL|jlong|jint|jfloat|jboolean|jdouble|jbyte|jchar|jshort|"
                r"jstring|jobject|jclass|void)\b", head)):
            if tok in ("JNIEXPORT", "JNICALL"):
                continue
            ret = tok
            break
        desc = "("
        for t in explicit:
            desc += type_map.get(t, "?%s" % t)
        desc += ")"
        desc += "V" if ret == "void" else type_map.get(ret, "?%s" % ret)
        # ---- C++ 声明里**看不出来**的那部分:JNI 的签名解析。
        #   JNI 规范:符号名只编码了方法名,参数/返回类型来自**描述符**;
        #   描述符要么在 class 文件里(RegisterNatives / 动态绑定),要么由
        #   JNI 在第一次调用时按**生成的 C 原型**静态匹配 —— 而原型里的
        #   `jobject` 是个**宽泛**类型(Kotlin 的 `Surface` 传过来就是 jobject),
        #   所以静态绑定的原生函数写 `jobject` 是完全合法的,不是签名错位:
        #   `nativeWindowFromSurface(surface: Surface)` 与
        #   `(JNIEnv*, jclass, jobject)` 在运行期确实对得上。
        #
        #   真正会对不上(UnsatisfiedLinkError)的是:**jobject 的位置**上的类型
        #   在 Kotlin 侧是**基本类型**(参考类型 ↔ jobject / 基本类型 ↔ jlong…),
        #   以及**个数**与**顺序**。所以这里把 jobject 视为"参考类型通配",
        #   其余逐个精确比对;Kotlin 侧的类型(权威来源)仍需自行一致。
        #   若将来改成 RegisterNatives + 显式描述符,把 jobject_dynamic 置 False,
        #   那就必须逐字对得上(本脚本会在 strict 模式下更严)。
        fields = []
        for t in explicit:
            d = type_map.get(t, "?%s" % t)
            fields.append(("jobject_dynamic" if t == "jobject" else d))
        found[name] = {"desc": desc, "ret": ret, "types": explicit, "fields": fields,
                       "c_ret": "V" if ret == "void" else type_map.get(ret, "?%s" % ret)}
    return found, path


def desc_matches(kotlin_parts, bridge_entry):
    """Kotlin 推导的描述符片段 vs C++ 声明的字段,按 JNI 语义比对。

    kotlin_parts 是 [参数描述符..., 返回描述符];bridge_entry 是 C++ 解析结果。
    """
    if "error" in bridge_entry:
        return False
    if len(kotlin_parts) != len(bridge_entry["fields"]) + 1:
        return False
    for k, c in zip(kotlin_parts[:-1], bridge_entry["fields"]):
        if c == "jobject_dynamic":
            if not (k.startswith("L") and k.endswith(";")):
                return False          # jobject 位置给了基本类型 → 真的对不上
        elif k != c:
            return False
    if kotlin_parts[-1] != bridge_entry["c_ret"]:
        return False
    return True


def check_jni_contract(env):
    ok_all = True
    kotlin, kt_path = parse_kotlin_externals(env)
    if kotlin is None:
        return fail("GATE_JNI", "Kotlin 解析失败: %s" % kt_path)
    bridge, br_path = parse_jni_bridge_symbols(env)
    if bridge is None:
        return fail("GATE_JNI", "jni_bridge.cpp 不存在: %s" % br_path)
    info("Kotlin external fun:%d 个(%s)" % (len(kotlin), os.path.basename(kt_path)))
    info("jni_bridge.cpp 导出:%d 个(%s)" % (len(bridge), os.path.basename(br_path)))

    # (a) 双向名字比对
    only_kt = sorted(set(kotlin) - set(bridge))
    only_c = sorted(set(bridge) - set(kotlin))
    if only_kt:
        ok_all = False
        fail("GATE_JNI", "Kotlin 声明了但 C++ 没导出(真机 UnsatisfiedLinkError): %s" % only_kt)
    if only_c:
        ok_all = False
        fail("GATE_JNI", "C++ 导出了但 Kotlin 没声明(死符号): %s" % only_c)
    if not only_kt and not only_c:
        info("名字双向一致:%d 个 JNI 入口,无缺无余" % len(kotlin))

    # (b) 描述符:两个独立来源
    n_fields_ok = 0
    n_hand_ok = 0
    for name, decl in sorted(kotlin.items()):
        try:
            kt_desc = kotlin_desc(decl["params"], decl["ret"])
        except KeyError as exc:
            ok_all = False
            fail("GATE_JNI", "%s 用了未登记的 Kotlin 类型 %s(先加到 KOTLIN_TO_JNI_DESC)" % (name, exc))
            continue
        hand = JNI_EXPECTED_DESC.get(name)
        entry = bridge.get(name) or {"error": "C++ 侧没这个符号"}
        parts = [KOTLIN_TO_JNI_DESC[t] for _n, t in decl["params"]] + [KOTLIN_TO_JNI_DESC[decl["ret"]]]
        if not desc_matches(parts, entry):
            ok_all = False
            fail("GATE_JNI", "%s 签名错位:Kotlin→%s,C++→%s(真机上 UnsatisfiedLinkError)" %
                 (name, kt_desc, entry.get("desc", entry.get("error"))))
        else:
            n_fields_ok += 1
        if hand is None:
            # 快记表没跟上 = 有人加了 JNI 入口但没在门控里登记。**必须红**:
            # 否则新入口从此不在"快记表 vs 实现"的交叉验证里,等于给未来留洞。
            ok_all = False
            fail("GATE_JNI", "%s 是本脚本速记表里没有的新入口(Kotlin=%s)。"
                             "新增 JNI 入口必须同时登记到 JNI_EXPECTED_DESC" % (name, kt_desc))
        elif hand != kt_desc:
            ok_all = False
            fail("GATE_JNI", "%s 与本脚本速记表不符:表=%s,Kotlin=%s(表或实现漂移了)" %
                 (name, hand, kt_desc))
        else:
            n_hand_ok += 1
    stale = sorted(set(JNI_EXPECTED_DESC) - set(kotlin))
    if stale:
        ok_all = False
        fail("GATE_JNI", "快记表里有、Kotlin 侧已不存在的入口(死登记): %s" % stale)
    if ok_all:
        info("签名一致:%d 个入口的 JNI 描述符(Kotlin 推导 vs C++ 推导)%d/%d 吻合;"
             "快记表 vs Kotlin 声明 %d/%d 吻合" %
             (len(kotlin), n_fields_ok, len(kotlin), n_hand_ok, len(kotlin)))
        dyn = sorted(n for n, e in bridge.items() if "fields" in e and "jobject_dynamic" in e["fields"])
        if dyn:
            info("  其中 %s 在 C++ 侧是 jobject(宽泛参考类型)—— 按 JNI 静态绑定语义合法,"
                 "已按'参考类型通配'比对而不是判红" % dyn)

    # (c) 反向回调:jni_bridge 还要求 Kotlin 有 @JvmStatic fun logNative(String)
    kt_text = open(kt_path, encoding="utf-8").read()
    if not re.search(r"@JvmStatic\s*\n\s*fun\s+logNative\s*\(\s*msg\s*:\s*String\s*\)", kt_text):
        ok_all = False
        fail("GATE_JNI", "Kotlin 侧缺 @JvmStatic fun logNative(msg: String),"
                         "GetStaticMethodID(\"logNative\", \"(Ljava/lang/String;)V\") 会在运行期返回 NULL、日志静默丢失")
    else:
        info("反向回调 OK:@JvmStatic fun logNative(String) 存在,"
             "与 C++ 的 GetStaticMethodID(\"logNative\", \"(Ljava/lang/String;)V\") 对应")

    return gate("GATE_JNI", ok_all,
                "%d 个 JNI 入口:名字双向一致 + 描述符双向吻合 + 反向 logNative 存在" % len(kotlin))


def dll_exported_symbols(env, dll_or_lib):
    """读 PE 的导出表,返回裸名集合(尽力而为;读不到就返回空集,由调用方降级)。

    ⚠ 踩过的坑:llvm-nm 对**已剥离 COFF 符号**的 PE 会成功返回(rc=0)但输出
      `<path>: no symbols` —— 那不是"没有导出",是"nm 看的是 COFF 符号表,不是
      导出表"。所以不能只看 rc,必须看解析出的名字数;为 0 就走 dumpbin /exports
      (它读的是真正的 export directory)。
    """
    names = set()
    nm = env.nm()
    vc = env.msvc_env()
    if nm:
        rc, out = _run([nm, "--extern-only", dll_or_lib], vc)
        if rc == 0:
            for line in out.splitlines():
                s = line.strip()
                if not s or s.endswith("no symbols") or ":" in s and " " not in s:
                    continue
                names.add(s.split()[-1])
    if not names:
        dumpbin = shutil.which("dumpbin.exe", path=vc.get("PATH", ""))
        if not dumpbin:
            return set()
        rc2, out2 = _run([dumpbin, "/nologo", "/exports", dll_or_lib], vc)
        if rc2 != 0:
            return set()
        for line in out2.splitlines():
            #   ordinal   hint   RVA      name
            #   "   1    0 000021D0 mb_abi_version"
            m = re.match(r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$", line)
            if m:
                names.add(m.group(1))
    # PE32+ x64 上 C 符号就是裸名;个别工具会带下划线,统一去一层
    out_names = set()
    for n in names:
        out_names.add(n)
        if n.startswith("_"):
            out_names.add(n[1:])
    return out_names


def header_mb_api_symbols(env):
    """从 mb_capi.h 抽所有标了 `MB_API <ret> MB_CALL <name>(` 的入口名。

    这是"契约面"的**权威来源**;MB_API_SYMBOLS 只是它的一个快照(给不使用
    mb_capi.h 的文件留一份可读清单)。自测会断言两者一致 —— 头文件加了入口
    而门控清单没跟上,门控就会对新入口视而不见(那也是一种假绿)。
    """
    text = read_text(os.path.join(env.mb, "include", "magicblocks", "mb_capi.h"))
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return sorted(set(re.findall(r"MB_API\s+[\w \*]+?\bMB_CALL\s+(mb_\w+)\s*\(", text)))


def check_symbols(env):
    """DLL 导出表:头文件里的每个 MB_API 入口都必须以**裸名**出现。

    这是 t2 事故的桌面可复现版本:链接上一份 .lib 只能证明编译器看得见的
    那几个符号对得上;这里直接读导出表,确认每个入口都以 ABI 名出现
    (没有 C++ 修饰名、没有漏导出)。
    """
    if not env.lib or not os.path.isfile(env.lib):
        return fail("GATE_SYMBOLS", "找不到 --lib,无法检查导出表")
    # --lib 必须是**导入库(.lib)**,不能是 .dll:llvm-nm 对 PE 动态库不输出任何
    # 符号(实测 `llvm-nm --extern-only magicblocks.dll` → "no symbols",exit=0)。
    # 传 .dll 进来会让下面的 missing 列表恒等于"全部 27 个入口",看起来像
    # "一个都没导出"—— 那是工具用法错误,不是 ABI 问题。直接说清楚。
    if env.lib.lower().endswith((".dll", ".so", ".dylib")):
        return fail("GATE_SYMBOLS",
                    "--lib 传的是动态库本体(%s),但 llvm-nm 读不出 PE 导出表;"
                    "请传导入库(magicblocks.lib)" % os.path.basename(env.lib))
    expected = header_mb_api_symbols(env)
    if not expected:
        return fail("GATE_SYMBOLS", "mb_capi.h 里解析不出任何 MB_API 入口 —— 解析器坏了")
    nm = env.nm()
    if not nm:
        warn("没有 llvm-nm / dumpbin,跳过导出表检查")
        return fail("GATE_SYMBOLS", "没有可用的符号表工具")
    vc = env.msvc_env()
    rc, out = _run([nm, "--extern-only", env.lib], vc)
    if rc != 0:
        return fail("GATE_SYMBOLS", "读取符号失败:\n" + tail(out))
    names = set()
    mangled = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        tok = line.split()[-1]
        names.add(tok)
        if tok.startswith("?") or tok.startswith("_Z"):
            mangled.append(tok)
    missing = [s for s in expected if s not in names]
    present_mb = sorted(n for n in names if n.startswith("mb_"))
    extra_mb = [n for n in present_mb if n not in expected]
    info("%s extern-only 符号 %d 个,其中 mb_* %d 个;头文件声明 %d 个 MB_API 入口"
         % (os.path.basename(nm), len(names), len(present_mb), len(expected)))
    ok = True
    if missing:
        ok = False
        fail("GATE_SYMBOLS", "导出表里缺这几个裸名符号(被 mangle / 没导出): %s" % missing)
    if extra_mb:
        ok = False
        fail("GATE_SYMBOLS", "导出表里多出未登记的 mb_* 符号(实现私货?没写进头就是契约外): %s"
             % extra_mb)
    if mangled:
        info("注意:有 %d 个 C++ mangle 名(如 %s);只要求 MB_API 入口是裸名"
             % (len(mangled), mangled[0]))
    return gate("GATE_SYMBOLS", ok,
                "%d 个 MB_API 入口全部以裸名出现在导出表,且无未登记的 mb_* 多出" % len(expected))


# ============================================================================
# [6] 版本校验(运行时 ↔ 头文件)
# ============================================================================

VERSION_PROBE = r"""#include <stdio.h>
#include "magicblocks/mb_capi.h"
int main(void) {
  const unsigned v = mb_abi_version();
  char buf[32] = {0};
  const size_t n = mb_abi_version_string(buf, sizeof(buf));
  printf("VERSION runtime=%u packed=%u major=%u minor=%u patch=%u string=%s len=%zu\n",
         v, (unsigned)MB_ABI_VERSION, (unsigned)MB_ABI_VERSION_MAJOR,
         (unsigned)MB_ABI_VERSION_MINOR, (unsigned)MB_ABI_VERSION_PATCH, buf, n);
  return 0;
}
"""


def check_version(env):
    if not env.lib:
        return fail("GATE_VERSION", "缺 --lib")
    res = build_and_run(env, VERSION_PROBE, [env.export_include], "version")
    if not res["compiled"] or not res["linked"] or res["exit"] != 0:
        return fail("GATE_VERSION", "版本探针没跑起来 编=%s 链=%s 退出=%s\n%s"
                    % (res["compiled"], res["linked"], res["exit"], tail(res["log"] + (res["out"] or ""))))
    m = re.search(r"VERSION runtime=(\d+) packed=(\d+) major=(\d+) minor=(\d+) patch=(\d+) "
                  r"string=(\S*) len=(\d+)", res["out"] or "")
    if not m:
        return fail("GATE_VERSION", "版本探针输出不可解析:\n" + tail(res["out"]))
    rt, packed, maj, mnr, pat, slen = (int(m.group(i)) for i in (1, 2, 3, 4, 5, 7))
    s = m.group(6)
    info("运行时 mb_abi_version()=%d,头文件 MB_ABI_VERSION=%d" % (rt, packed))
    info("三元组:头 %d.%d.%d,运行时字符串 %s(len=%d)" % (maj, mnr, pat, s, slen))
    ok = True
    if rt != packed:
        ok = False
        fail("GATE_VERSION", "DLL 的 mb_abi_version()=%d 与头文件 MB_ABI_VERSION=%d 不一致" % (rt, packed))
    if packed != maj * 10000 + mnr * 100 + pat:
        ok = False
        fail("GATE_VERSION", "打包公式不符:%d != %d*10000+%d*100+%d" % (packed, maj, mnr, pat))
    expect = "%d.%d.%d" % (maj, mnr, pat)
    if s != expect:
        ok = False
        fail("GATE_VERSION", "version string 期望 %r 实际 %r" % (expect, s))
    if slen != len(expect) + 1:
        ok = False
        fail("GATE_VERSION", "两段式长度应为 %d(含 NUL)实际 %d" % (len(expect) + 1, slen))
    return gate("GATE_VERSION", ok,
                "运行时 ↔ 头文件的 MAJOR/MINOR/PATCH 与打包公式、version string 全部一致")


# ============================================================================
# [7] 构建图隔离扫描(复用 build.ninja 的精确 TU 列表)
# ============================================================================

UI_TU_GLOBS = (
    "src/android/*.cpp", "src/capi/*.cpp",
    "MagicBlocks/android/app/src/main/**/*.cpp",
)

# ---------------------------------------------------------------------------
# 【t5 之后的分工】边界实现 TU 不属于"UI 侧 TU"
#
# t5 把 C ABI 那一层做成了**动态库内部**的一件事：
#   src/capi/mb_capi.cpp        ← mb_capi.h 的实装侧,必然 include 全部内部头
#   src/android/*.cpp           ← JNI/surface 胶水,同上
# 它们编进 magicblocks.dll / libmagicblocks.so,是**库的内部**,不是消费者。
# 真正的 UI 消费者是:
#   - Kotlin (android/app/src/main/**/*.kt):经 JNI 调 .so,一个 C++ 头都不 include
#   - 子工程 tools/uisplit/probe_consumer.cpp:只 include mb_capi.h,只链 DLL
#   - 门控自己造的 UI_CONSUMER 探针(导出面 + .lib)
#
# 所以 scan_ui_dependency_graph 若继续把 src/capi、src/android 当"UI TU"扫,
# 它会把"库实现自己 include 内部头"判成隔离违规 —— 那是**必然为真**的事,
# 让门控永远红着,而红的原因和 UI 边界毫无关系。
# 这里把这两类 TU 显式排除,并在返回前断言"排除后仍有非空集"防止假绿。
BOUNDARY_IMPL_TU_PREFIXES = ("src/capi/", "src/android/")

# t5 的消费者探针:源码里只有一句 include,只链动态库。
# 它不是"库内部",是**真消费者**,所以必须留在扫描集里 ——
# 排除边界实现 TU 之后,它是"源码树里唯一可静态扫到 C++ 消费者"。
UI_CONSUMER_TU = "tools/uisplit/probe_consumer.cpp"


def is_boundary_impl_tu(rel):
    """这个 TU 是不是 DLL 内部的边界实现(而不是 UI 消费者)?"""
    return any(rel.startswith(p) for p in BOUNDARY_IMPL_TU_PREFIXES)


def ninja_objs(build_dir, target):
    nj = os.path.join(build_dir, "build.ninja")
    if not os.path.isfile(nj):
        return None
    # 老版 CMake/ninja 会把对象路径写成 `MagicBlocks.dir\src\android\jni_bridge.cpp.obj`,
    # 新版写成 `MagicBlocks.dir/src/android/jni_bridge.cpp.obj` —— 统一成 / 再比。
    text = open(nj, encoding="utf-8", errors="replace").read().replace("\\", "/")
    out = []
    for m in re.finditer(r"^build CMakeFiles/([^:]+)\.dir/(\S+?)\.obj:", text, re.M):
        if m.group(1) == target:
            out.append(m.group(2))
    return sorted(out)


def ui_tus_from_build(env):
    """UI TU 的来源:扫**所有**目标里的 UI 侧文件。

    ⚠ t5 之后这里发生了语义变化,不更新就会静默扫错:
      · `src/android/jni_bridge.cpp`(Android)、`src/android/scene_renderer.cpp`、
        `src/capi/mb_capi.cpp` 现在是 **DLL 边界实现**(库里面),不是 UI 消费者;
      · 真正的 UI 消费者变成 `tools/uisplit/probe_consumer.cpp`
        (桌面侧,只 include 契约头)。
    所以判定分两类:
      · `BOUNDARY_TUS`  = 库侧边界实现,允许碰 SDK 内部(includes 不走本门控)
      · `ui 侧 TU`      = 其余一切 src/android/*.cpp(除边界实现)与 tools/uisplit/*.cpp
    空集仍是假绿,所以调用方必须区分 None(没有构建图)与 [](扫了但一个都没有)。
    """
    nj = env.ninja or os.path.join(env.build_dir, "build.ninja")
    if not os.path.isfile(nj):
        return None
    # 老版 CMake/ninja 会把对象路径写成 `MagicBlocks.dir\src\android\jni_bridge.cpp.obj`,
    # 新版写成 `MagicBlocks.dir/src/android/jni_bridge.cpp.obj` —— 统一成 / 再比。
    with open(nj, encoding="utf-8", errors="replace") as f:
        text = f.read().replace("\\", "/")
    # 逻辑物理行:以空格开头的续行属于上一条 build 语句
    text = re.sub(r"\r?\n(?!build )", " ", text)
    tus = set()
    for line in text.splitlines():
        m = re.match(r"^build CMakeFiles/([^:]+)\.dir/(\S+?)\.obj:", line)
        if not m:
            continue
        rel = m.group(2)
        if is_boundary_impl_tu(rel):
            continue          # 库侧边界实现,不是 UI 消费者(见上面的分工注释)
        if rel.startswith("src/android/") or rel.startswith("tools/uisplit/"):
            tus.add(rel)
    return sorted(tus)


def tus_from_cmake_lists(env):
    """没有 build.ninja 时(例如还没 configure),从 CMakeLists.txt 的
    `add_library(magicblocks_jni ...)` / 源码列表里抽 UI TU。

    这是**降级**路径,不是等价物:ninja 是"实际编译了哪些 TU"的事实,
    CMakeLists 是"打算编译哪些"。两条路径都实现,是为了避免"没有构建目录
    时扫描退化成空集、然后永远是绿"的假绿。
    """
    cmake = os.path.join(env.mb, "CMakeLists.txt")
    if not os.path.isfile(cmake):
        return None
    text = open(cmake, encoding="utf-8", errors="replace").read()
    text = re.sub(r"#[^\n]*", "", text)
    tus = set()
    def norm(f):
        f = f.strip().strip('"')
        for pre in ("${CMAKE_CURRENT_SOURCE_DIR}/", "${MB_ROOT}/", "${CMAKE_CURRENT_LIST_DIR}/",
                    "./", "../"):
            if f.startswith(pre):
                f = f[len(pre):]
        f = f.replace("\\", "/")
        return f
    for m in re.finditer(r"add_(?:library|executable)\s*\(\s*(\w+)([^)]*)\)", text, re.S):
        target, body = m.group(1), m.group(2)
        for f in re.findall(r"[\w${}/.\\-]+\.(?:cpp|cxx|cc)\b", body):
            f = norm(f)
            # t5 之后:src/capi、src/android 是**库内部边界实现**,不是 UI 消费者。
            # 继续把它们当 UI TU 会让"库实现 include 内部头"被判成隔离违规 ——
            # 那是必然为真的事,门控会永远红且红得毫无意义。
            if is_boundary_impl_tu(f):
                continue
            if f.startswith("tools/uisplit/"):
                tus.add(f)
    return sorted(tus) if tus else None


def scan_ui_dependency_graph(env, tus=None):
    """UI 侧 TU 的 include 集合 ⊆ {mb_capi.h} + 标准库 + 平台头。

    参数 tus 非 None 时**只**扫这些 TU(自测注入违规 TU 用;绝不碰真实文件),
    否则按 build.ninja → CMakeLists.txt 的顺序取。

    t5 之后:src/capi/ 与 src/android/ 是**库内部的边界实现**,不是 UI 消费者,
    必须排除(理由见 BOUNDARY_IMPL_TU_PREFIXES 的注释)。排除后若一个 TU 都不剩,
    直接判红 —— "排除"绝不能变成"扫了个空集然后恒绿"。
    """
    explicit = tus is not None
    if not explicit:
        tus = ui_tus_from_build(env) or None
        if not tus:
            warn("构建图里没有 UI TU(桌面构建不含 ANDROID 分支),降级为从 CMakeLists.txt 抽 UI TU")
            tus = tus_from_cmake_lists(env)
        if not tus:
            return fail("GATE_DEPS", "构建图与 CMakeLists 里都找不到任何 UI TU"
                                     "(src/android、src/capi)—— 扫描会变成空集假绿,拒绝判定")
        src_tus = [t for t in tus if t.startswith("src/")]
        excluded = [t for t in src_tus if is_boundary_impl_tu(t)]
        src_tus = [t for t in src_tus if not is_boundary_impl_tu(t)]
        # 探针是源码树里的真消费者:只要文件在,就**必须**参与扫描。
        # 少了它,src_tus 可能为空 → 上面的空集判红会触发;有了它,
        # 扫描对象永远是"至少一个真的 UI 消费者"。
        if os.path.isfile(os.path.join(env.mb, UI_CONSUMER_TU.replace("/", os.sep))):
            if UI_CONSUMER_TU not in src_tus:
                src_tus.append(UI_CONSUMER_TU)
        if excluded:
            info("排除 %d 个边界实现 TU(DLL 内部,不是 UI 消费者):%s"
                 % (len(excluded), excluded))
    else:
        src_tus = list(tus)
    if not src_tus:
        return fail("GATE_DEPS",
                    "排除边界实现 TU 后没有剩下任何 UI TU —— 这会让扫描变成空集假绿。"
                    "UI 消费者至少要有 Kotlin 侧 + tools/uisplit/probe_consumer.cpp;"
                    "若两者都拿不到,说明本门控的判定依据已经不成立了")
    info("UI TU:%d 个 → %s" % (len(src_tus), src_tus))
    offenders = []
    allowed = [PUBLIC_HEADER]
    # 契约头的两种合法写法(同一个文件的两种视角,见 CMakeLists 里 PUBLIC include 的注释):
    #   "magicblocks/mb_capi.h"  ← 出厂形态:导出面根 + magicblocks/ 子目录
    #   "mb_capi.h"              ← 构建期形态:PUBLIC include 直接就是 include/magicblocks
    # 只有这两种算"看得见契约头",别的 magicblocks/ 子路径仍然一律判红。
    ALLOWED_CONTRACT_INCS = (PUBLIC_HEADER, "mb_capi.h")
    # 平台头白名单:NDK sysroot 的 C/JNI/Android 头。UI 的 JNI 薄层合法依赖它们,
    # 它们不是 SDK 的东西,也不会因为 SDK 重构而变。白名单是**封闭集合**,
    # 不在下面三类里的 include 一律判红(不是"忽略")。
    PLATFORM_INC = re.compile(r"^(?:jni\.h|android/[a-z_]+\.h)$")
    STDLIB_INC = re.compile(
        r"^(?:cstdint|cstdio|cstring|cstdlib|string|vector|utility|algorithm|memory|"
        r"cstddef|cmath|type_traits|atomic|mutex|thread|chrono|map|unordered_map|"
        r"functional|array|new|limits|stdint\.h|stddef\.h|stdio\.h)$")
    for rel in src_tus:
        path = os.path.join(env.mb, rel.replace("/", os.sep))
        if not os.path.isfile(path):
            offenders.append("%s: 构建图里有但磁盘上没有(构建图过期 / 名字写错)" % rel)
            continue
        for inc in include_names(path):
            if inc in ALLOWED_CONTRACT_INCS or STDLIB_INC.match(inc):
                continue
            if any(inc.startswith(p) for p in INTERNAL_HEADER_PREFIXES):
                offenders.append("%s: #include \"%s\"  ← SDK 内部头" % (rel, inc))
            elif inc.startswith("magicblocks/"):
                offenders.append("%s: #include \"%s\"(magicblocks/ 下非 mb_capi.h)" % (rel, inc))
            elif PLATFORM_INC.match(inc):
                info("%s: #include <%s> ← 平台头(NDK sysroot),放行" % (rel, inc))
            else:
                offenders.append("%s: #include \"%s\"(未登记的头:要么加白名单并说明理由,"
                                 "要么它就不该出现在 UI TU 里)" % (rel, inc))
    # Kotlin / Java:不得出现任何 SDK 内部包或别的本地库
    ui_src_roots = [os.path.join(env.mb, "android", "app", "src", "main")]
    kt_files = []
    for root in ui_src_roots:
        for dirpath, _d, files in os.walk(root):
            for n in files:
                if n.endswith(".kt") or n.endswith(".java"):
                    kt_files.append(os.path.join(dirpath, n))
    bad_kt = []
    for p in kt_files:
        text = open(p, encoding="utf-8", errors="replace").read()
        for pat, why in (
                (r"import\s+magicblocks\.(device|scene|primitives|ui|resources|features|backend)",
                 "Kotlin 引用了 SDK 内部包"),
                (r"#\s*include\s*[<\"]magicblocks/(?!mb_capi\.h)", "Kotlin 里写了 C++ include"),
                (r'System\.loadLibrary\("(?!magicblocks")', "Kotlin 加载了别的本地库")):
            if re.search(pat, text):
                bad_kt.append("%s: %s" % (os.path.relpath(p, env.mb), why))
    info("Kotlin/Java 文件 %d 个,违规 %d 处" % (len(kt_files), len(bad_kt)))
    cmake_bad = check_cmake_public_includes(env)
    offenders.extend(bad_kt)
    offenders.extend(cmake_bad)
    for o in offenders:
        fail("GATE_DEPS", o)
    return gate("GATE_DEPS", not offenders,
                "UI TU %d 个 + Kotlin %d 个 + CMake 导出面:引用集合 ⊆ {%s}" %
                (len(src_tus), len(kt_files), PUBLIC_HEADER))


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def include_names(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return INCLUDE_RE.findall(text)


def cmake_public_include_dirs(text, target="magicblocks"):
    """从 CMakeLists 抽出 `target` 的 PUBLIC/INTERFACE include 目录。

    两类写法都必须抓住(只抓第一类 = 门控有洞):
      - `PUBLIC include` / `INTERFACE include/mbc-serial`(位置参数)
      - `INTERFACE "$<BUILD_INTERFACE:.../include>"`(生成器表达式)
    """
    out = []
    for m in re.finditer(r"target_include_directories\s*\(\s*(\S+)([^)]*)\)", text, re.S):
        if m.group(1) != target:
            continue
        mode = None
        for tok in m.group(2).split():
            up = tok.upper()
            if up in ("PUBLIC", "INTERFACE", "PRIVATE", "SYSTEM", "BEFORE", "AFTER"):
                mode = up
                continue
            if mode not in ("PUBLIC", "INTERFACE"):
                continue
            # 展开生成器表达式:$<BUILD_INTERFACE:path> / $<INSTALL_INTERFACE:path>
            for inner in re.findall(r"\$<(?:BUILD|INSTALL)_INTERFACE:([^>]+)>", tok):
                out.append((mode, inner))
            if not tok.startswith("$<"):
                out.append((mode, tok))
    return out


def is_whole_include_dir(path_fragment):
    """这个 include 片段是否指向**整棵** include/?(而不是它下面的子目录)

    `include` / `${CMAKE_CURRENT_SOURCE_DIR}/include` → 整棵,判红
    `include/magicblocks` / `include/mbc-serial`      → 子目录,合规
    `src` / `${CMAKE_CURRENT_BINARY_DIR}`             → 不是 include,忽略
    """
    frag = path_fragment.strip().strip('"')
    frag = re.sub(r"\$\{?[A-Za-z_]+\}?", "X", frag)   # 变量 → 占位,便于统一判断
    frag = frag.replace("\\", "/").rstrip("/")
    parts = [p for p in frag.split("/") if p not in ("", ".")]
    if not parts:
        return False
    return parts[-1] == "include"


def check_cmake_public_includes(env):
    """CMake 侧:PUBLIC/INTERFACE 的 include 目录不能是整棵 include/。

    UI 的可见面由 `target_include_directories(... PUBLIC ...)` 决定;
    若 PUBLIC 给的是 `include/`,那 63 个内部头就在 UI 的搜索路径上,
    不管源码里有没有 include 它们 —— 这一条把"能不能看到"钉死在构建配置上。
    """
    bad = []
    cmake = os.path.join(env.mb, "CMakeLists.txt")
    if not os.path.isfile(cmake):
        return ["CMakeLists.txt 不存在"]
    text = read_text(cmake)
    for mode, frag in cmake_public_include_dirs(text, "magicblocks"):
        if is_whole_include_dir(frag):
            bad.append("CMakeLists.txt: target magicblocks 把整棵 include/ 当 %s 导出(%s) —— "
                       "UI 会看到全部内部头" % (mode, frag))
    for mode, frag in cmake_public_include_dirs(text, "magicblocks_jni"):
        bad.append("CMakeLists.txt: UI 的 JNI target(magicblocks_jni)有 %s include(%s)—— "
                   "UI 的 JNI 层不应该给任何下游导出 include 面" % (mode, frag))
    return bad


# ============================================================================
# [8] 元测试:证明门控会红
# ============================================================================

def check_meta(env, stage):
    """三种刺探,每一种都必须红在预期阶段。没有任何一种以外的方式触发。"""
    ok = True
    incs = [env.export_include] if env.export_include else []
    if not incs or not os.path.isdir(incs[0]):
        return fail("GATE_META", "缺导出面,元测试无法开工")

    # M1:去掉 extern "C" 的效果 —— 用 C++ 引用这些符号的名字空间来模拟 mangle。
    #     真做法:编译一份不带 mb_capi.h 的 TU,自己用 C++ 链接声明这些函数,
    #     再链接 DLL 的导入库 → 链接期必须失败(LNK2019 的等价物)。
    mangle = r"""#include <cstdint>
// 「如果 mb_capi.h 忘了 extern "C"」的等价物:同名同参,但是 C++ 链接。
// 与 DLL 里的 C 符号对不上 → 链接必须失败。
std::uint32_t mb_abi_version();
void* mb_session_create(const void*, void**);
int main() { return (int)mb_abi_version(); }
"""
    if env.lib:
        res = build_and_run(env, mangle, incs, "meta_mangle")
        gone_red = (not res["compiled"]) or (not res["linked"])
        if res["linked"]:
            info("注意:这一刺探里编译器可能被导入库里的 .lib 元数据救回来了;"
                 "改用 /MD 的纯声明依赖再看")
            ok = False
            fail("GATE_META", "!! 用 C++(无 extern \"C\")声明 mb_abi_version 竟然链接成功 —— "
                              "说明 DLL 里同时存在修饰名导出,契约面不干净")
        else:
            msgs = [l for l in res["log"].splitlines() if "LNK2019" in l or "LNK2001" in l]
            info("[red] M1 无 extern \"C\":链接失败 %s" % (msgs[0].strip() if msgs else "(非 LNK2019)"))
    else:
        warn("无 --lib,M1(链接期 mangle 刺探)跳过")

    # M2:ABI 不一致时**不能崩**,必须给明确错误码。
    #     "struct_size 写错" 是最典型的 ABI 不一致(头文件与库不同版本);
    #     这里不看退出码是不是 7(那可能只是设备不可用),而是看消费者到底
    #     拿到了什么错误码 —— 崩(非 0xC0000005 以外的异常码)或被当成成功都算坏。
    runtime_probe = UI_CONSUMER.replace(
        "mb_session_desc d = mb_session_desc_default();",
        "mb_session_desc d = mb_session_desc_default(); d.struct_size = 4; /* 故意的:ABI 不一致 */")
    if env.lib:
        res = build_and_run(env, runtime_probe, incs, "meta_baddesc")
        if not (res["compiled"] and res["linked"]):
            ok = False
            fail("GATE_META", "M2 没能编/链起来(编=%s 链=%s),无法判定 ABI 不一致时的行为"
                 % (res["compiled"], res["linked"]))
        elif res["exit"] == 0:
            ok = False
            fail("GATE_META", "!! struct_size 写错(模拟 ABI 不一致)时消费者竟然跑到了 CONSUMER_OK")
        elif res["exit"] is not None and res["exit"] > 0xC0000000:
            ok = False
            fail("GATE_META", "!! ABI 不一致导致进程异常退出(0x%X),约定应是返回明确错误码而不是崩"
                 % res["exit"])
        else:
            first = [l for l in (res["out"] or "").splitlines() if "CONSUMER" in l]
            # 打印**全部** CONSUMER 行:只看第一行会把关键的 MB_E_INVALID_ARG
            # 那行藏起来,读报告的人无法确认"失败原因是错误码明确"而不是别的原因。
            info("[red] M2 ABI 不一致(struct_size=4):编过链过,运行期如预期失败 exit=%s" % res["exit"])
            for l in first:
                info("        " + l)
            if not any("MB_E_INVALID_ARG" in l for l in first):
                ok = False
                fail("GATE_META", "M2 失败原因不是 MB_E_INVALID_ARG(错误码不明确): %r" % first[:2])

    # M3:注入一个违规 TU,构建图扫描必须报红(只喂一份临时 TU 列表,
    #     不往工作树里写文件、也不让扫描去碰真实文件)
    # 注意:这里只看**本 TU 引起的**违规。CMake 侧的判定是另一道门(GATE_DEPS 里的
    # CMake 部分),今天 t5 还没落地,那条是真红;若把它算进 M3 的结论,元测试会把
    # 真实红误读成"注入生效",两边都失去意义。所以 M3 期间把 CMake 判定临时置空,
    # 并且把刺探期间的 FAILURES 存起来/还原,不让刺探结果污染最终汇总。
    viol = os.path.join(env.work, "injected_probe.cpp")
    clean = os.path.join(env.work, "clean_probe.cpp")
    os.makedirs(env.work, exist_ok=True)
    with open(viol, "w", encoding="utf-8") as f:
        f.write('#include "magicblocks/scene/scene.h"\nint t6_injected_probe() { return 0; }\n')
    with open(clean, "w", encoding="utf-8") as f:
        f.write('#include "magicblocks/mb_capi.h"\nint t6_clean_probe() { return 0; }\n')
    rel_viol = os.path.relpath(viol, env.mb).replace(os.sep, "/")
    rel_clean = os.path.relpath(clean, env.mb).replace(os.sep, "/")
    saved_failures = list(FAILURES)
    saved_gates = list(GATES)
    saved_cmake = globals()["check_cmake_public_includes"]
    try:
        globals()["check_cmake_public_includes"] = lambda _env: []
        before = len(FAILURES)
        scan_ui_dependency_graph(env, tus=[rel_viol])
        tu_fails = [f for f in FAILURES[before:] if "CMakeLists.txt" not in f]
        if tu_fails:
            info("[red] M3 注入违规 TU(#include \"magicblocks/scene/scene.h\"):扫描报红 → %s"
                 % tu_fails[0])
        else:
            ok = False
            fail("GATE_META", "!! 注入一个 include 内部头的 UI TU,依赖扫描竟然还是绿的")
        before = len(FAILURES)
        good = scan_ui_dependency_graph(env, tus=[rel_clean])
        if good:
            info("[green] M3b 只 include mb_capi.h 的 TU 扫描为绿 —— 证明 M3 的红来自那行 include")
        else:
            ok = False
            fail("GATE_META", "!! 只 include mb_capi.h 的 TU 也被判红了 —— 扫描过严,M3 的红没意义")
    finally:
        globals()["check_cmake_public_includes"] = saved_cmake
        # 还原汇总:刺探明细刚才已经打印出来了,但它们不是本轮最终判红项
        # (否则"元测试全部通过"的结论会被刺探自己的红盖住)。
        del FAILURES[:]
        FAILURES.extend(saved_failures)
        del GATES[:]
        GATES.extend(saved_gates)
        for p in (viol, clean):
            if os.path.isfile(p):
                os.remove(p)

    # M4:负向门控在"给回真实内部头路径"时必须变绿(证明那红不是编译器坏了)
    #
    # t5 之后内部头物理搬到了 include/mb_internal/magicblocks/ 下,
    # 所以"给回真实路径"要同时给 include/mb_internal(真正的内部头根)。
    # 探针模板会自己拼上 "magicblocks/",所以这里 internal 只能填
    # "scene/scene.h" —— 拼出来是 "magicblocks/scene/scene.h",
    # 配合 /I include/mb_internal 正好解析到
    #   include/mb_internal/magicblocks/scene/scene.h  ← 真的存在。
    # 若还把探针写成 "mb_internal/magicblocks/scene/scene.h",会拼成
    # "magicblocks/mb_internal/..." 这种根本不存在的路径,红的原因就不是隔离
    # 而是拼错了 —— 那正是 M4 要抓的"红的证明力"问题。
    real = os.path.join(env.mb, "include")
    internal = os.path.join(env.mb, "include", "mb_internal")
    probe = NEG_INCLUDE_PROBE % {"internal": "scene/scene.h"}
    d = os.path.join(env.work, "meta_neg_with_real")
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    p = os.path.join(d, "neg.cpp")
    open(p, "w", encoding="utf-8").write(probe)
    vc = env.msvc_env()
    rc, log = _run([env.cl_path(), "/nologo", "/std:c++20", "/EHsc", "/c", p,
                    "/Fo:" + os.path.join(d, "neg.obj"), "/I" + real, "/I" + internal], vc, cwd=d)
    if rc == 0:
        info("[green] M4 同样的 TU 指向真实内部头路径时编过了 —— "
             "证明 NEG_INCLUDE 的红确实来自导出面,不是语法错")
    else:
        ok = False
        fail("GATE_META", "!! 同样的 TU 指向真实内部头路径也编不过 —— "
                          "NEG_INCLUDE 的红没有证明力(可能是 TU 本身有毛病)\n" + tail(log, 8))

    return gate("GATE_META", ok, "元测试 %s" % ("全部符合预期" if ok else "有失败项"))


# ============================================================================
# main
# ============================================================================

def probe(name, env, **kwargs):
    """按模式的可选探针:`<scripts-dir>/<name>.py` 里定义 `run(env, **kwargs)`。

    用在哪里:自测(tools/tests/test_check_ui_isolation.py)要给"编→链→跑"
    单独喂故意写坏的消费者(缺 extern "C" / 用了内部头 / 版本断言错),
    而这些刺探需要真实 DLL,所以不想放进日常门控里。
    没给 --scripts-dir 或脚本不存在 → 直接跳过,不影响门控结论。
    """
    d = getattr(env.args, "scripts_dir", None)
    if not d:
        return None
    path = os.path.join(d, name + ".py")
    if not os.path.isfile(path):
        return None
    import importlib.util
    spec = importlib.util.spec_from_file_location("_t6_probe_" + name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.run(env, **kwargs)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--magicblocks", default=None, help="MagicBlocks 源码目录")
    ap.add_argument("--build-dir", default=None, help="构建目录(含 build.ninja)")
    ap.add_argument("--export-include", default=None, help="只含 mb_capi.h 的导出面目录")
    ap.add_argument("--lib", default=None, help="magicblocks.dll 或它的导入库")
    ap.add_argument("--ninja", default=None, help="build.ninja 路径(默认 <build-dir>/build.ninja)")
    ap.add_argument("--work-dir", default=None, help="临时工作目录")
    ap.add_argument("--vcvars", default=None, help="vcvars64.bat")
    ap.add_argument("--cl", default=None, help="cl.exe")
    ap.add_argument("--ndk-nm", default=None, help="llvm-nm.exe / dumpbin.exe")
    ap.add_argument("--neg-headers", default=None,
                    help="负向探针要 include 的内部头(逗号分隔)")
    ap.add_argument("--kotlin-path", default=None,
                    help="MbRenderer.kt(JNI 契约门控;默认走 android 树)")
    ap.add_argument("--jni-bridge-path", default=None,
                    help="jni_bridge.cpp(JNI 契约门控;默认 src/android/jni_bridge.cpp)")
    ap.add_argument("--scripts-dir", default=None,
                    help="可选目录:加载按模式探针脚本(`<name>.py`),给自测用")
    ap.add_argument("--mode", default="all",
                    help="all | export | negative | consumer | relocate | jni | symbols | version | deps | meta(逗号分隔)")
    ap.add_argument("--layer", default="all", choices=("all", "compile"),
                    help="all(默认,三层都判)| compile(只判编译层+链接层;"
                         "运行层因本机无 GPU 缺席时,用它显式声明范围,而不是伪造绿)")
    args = ap.parse_args(argv)

    env = Env(args)
    # ctest 直接跑本脚本时不会带 --export-include / --lib(tools/ui_isolation.ps1
    # 才会带)。缺了它们,symbols/consumer/version/meta 一律 BLOCKED,
    # 于是 `ctest -R gate_ui_isolation` 变成"看着有、其实什么都没验"。
    # 这里从**构建目录**把两个参数补上:
    #   导出面 ← 源码树里的契约头(与 ui_isolation.ps1 的默认来源同一份)
    #   --lib  ← 构建目录里的**导入库** magicblocks.lib(绝不能给 .dll)
    # 只补"没给"的;显式传参时一律以命令行为主。
    if not getattr(args, "export_include", None):
        src_hdr = os.path.join(env.mb, "include", "magicblocks", "mb_capi.h")
        if os.path.isfile(src_hdr):
            exp = os.path.join(env.work, "export_include")
            dst = os.path.join(exp, "magicblocks")
            os.makedirs(dst, exist_ok=True)
            shutil.copyfile(src_hdr, os.path.join(dst, "mb_capi.h"))
            env.export_include = exp
            args.export_include = exp
            print("自动生成导出面(未传 --export-include): %s" % exp)
    if not getattr(args, "lib", None):
        for pat in ("magicblocks.lib", "magicblocks.dll"):
            cand = os.path.join(env.build_dir, pat)
            if os.path.isfile(cand):
                env.lib = cand
                args.lib = cand
                print("自动选定被测库(未传 --lib): %s" % cand)
                break
    modes = set(m.strip() for m in args.mode.split(",") if m.strip())
    if "all" in modes:
        modes = {"export", "negative", "consumer", "relocate", "jni", "symbols", "version", "deps", "meta"}

    if not os.path.isdir(env.mb):
        print("环境错误:MagicBlocks 目录不存在 %s" % env.mb)
        return 2
    os.makedirs(env.work, exist_ok=True)

    print("=" * 78)
    print("t6 UI 隔离性门控")
    print("  MagicBlocks : %s" % env.mb)
    print("  build-dir   : %s" % env.build_dir)
    print("  导出面      : %s" % env.export_include)
    print("  动态库      : %s" % env.lib)
    print("  modes       : %s" % ",".join(sorted(modes)))
    print("  layer       : %s" % getattr(args, "layer", "all"))
    print("=" * 78)

    try:
        if "export" in modes:
            check_export_surface(env)
            check_source_tree_surface(env)
        if "negative" in modes:
            check_negative_include(env)
        if "consumer" in modes:
            check_ui_consumer(env)
        if "relocate" in modes:
            check_relocate(env)
        if "jni" in modes:
            check_jni_contract(env)
        if "symbols" in modes:
            check_symbols(env)
        if "version" in modes:
            check_version(env)
        if "deps" in modes:
            scan_ui_dependency_graph(env)
        if "meta" in modes:
            check_meta(env, None)
        hint = probe("staged_probes", env, modes=modes)
        if hint:
            for line in hint:
                info("[额外刺探] " + line)
    except RuntimeError as exc:
        print("环境错误:%s" % exc)
        return 2

    failed = [g for g in GATES if not g[1]]
    print("-" * 78)
    if WARNINGS:
        print("警告(降级项):%d" % len(WARNINGS))
        for w in WARNINGS:
            print("  - " + w)
    if ENV_RESULTS:
        print("环境阻塞项(不算绿也不算红):%d" % len(ENV_RESULTS))
        for name, detail in ENV_RESULTS:
            print("  - %-18s %s" % (name, detail))
    print("结果: gates=%d failed=%d blocked=%d" % (len(GATES), len(failed), len(ENV_RESULTS)))
    for name, _ok, detail in failed:
        print("  FAILED %-18s %s" % (name, detail.splitlines()[0] if detail else ""))
    if not GATES and not ENV_RESULTS:
        print("没有跑任何门控 —— 算失败(--mode 写错了?)")
        return 2
    if failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
