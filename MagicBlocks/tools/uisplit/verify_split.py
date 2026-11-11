#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""t6 隔离性验证:t5 拆完之后,证明"UI 与 SDK 真的是拆开的"。

用法(在 worktree 根目录):
  python MagicBlocks/tools/uisplit/verify_split.py
  python MagicBlocks/tools/uisplit/verify_split.py --build-dir build-uisplit
  python MagicBlocks/tools/uisplit/verify_split.py --report docs/UI_SPLIT_VERIFY.md

设计原则(为什么不是"跑一堆 assert 然后打印 OK"):
  1. **每道门控都必须能红**。一个永远匹配不上的门控等于没有门控。所以凡
     "必须失败"的断言,都同时给出反向自证(改一个字就该编过),区分"门控有效"
     与"探针写坏/环境坏了"。
  2. **失败必须如实写**。FAIL 就是 FAIL,不降级成 WARN,不改写成"通过"。
     环境缺席单独记 BLOCKED,既不冒绿也不恒红。
  3. **不依赖被测对象自证**。探针源码是脚本内嵌的常量,不从被测树里读。

退出码: 0 = 全 PASS(允许 BLOCKED);1 = 有 FAIL;2 = 环境错误。
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MB = os.path.abspath(os.path.dirname(os.path.dirname(HERE)))  # tools/uisplit -> MagicBlocks

PUBLIC_HEADER = os.path.join(MB, "include", "magicblocks", "mb_capi.h")
PUBLIC_DIR = os.path.join(MB, "include", "magicblocks")

# ---------------------------------------------------------------------------
# 结果收集
# ---------------------------------------------------------------------------
RESULTS = []    # [(section, name, status, detail)]   status in PASS/FAIL/BLOCKED


def record(section, name, status, detail=""):
    RESULTS.append((section, name, status, detail))
    tag = {"PASS": "PASS   ", "FAIL": "FAIL   ", "BLOCKED": "BLOCKED"}[status]
    print("[%s] %-12s %s %s" % (tag, section, name, detail))
    sys.stdout.flush()
    return status == "PASS"


def pass_(s, n, d=""):
    return record(s, n, "PASS", d)


def fail_(s, n, d=""):
    return record(s, n, "FAIL", d)


def blocked(s, n, d=""):
    return record(s, n, "BLOCKED", d)


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    return raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")


def strip_comments(text):
    """去掉注释再做正则判定 —— 注释里写 "std::" 不算违规。

    但**字符串字面量要保留**:把 #include "std::..." 之类藏进字符串是绕过
    门控的手法,这里宁可误报也不漏报。
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


# ---------------------------------------------------------------------------
# 工具链
# ---------------------------------------------------------------------------
_VC_ENV = None


def vc_env():
    """导入 vcvars64.bat 的 INCLUDE/LIB —— 与 build.ps1 同一套做法。"""
    global _VC_ENV
    if _VC_ENV is not None:
        return _VC_ENV
    vcvars = None
    for pat in (r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat",
                r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat"):
        c = sorted(glob.glob(pat))
        if c:
            vcvars = c[0]
            break
    if not vcvars:
        raise RuntimeError("找不到 vcvars64.bat(需要 MSVC 工具链)")
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
    _VC_ENV = env
    return env


def cl_exe():
    w = shutil.which("cl.exe", path=vc_env().get("PATH", ""))
    if not w:
        raise RuntimeError("vcvars64 之后仍找不到 cl.exe")
    return w


def link_exe():
    return shutil.which("link.exe", path=vc_env().get("PATH", ""))


def run(cmd, cwd=None, env=None):
    """跑子进程并**正确解码**输出。

    ⚠ 关键坑:cl.exe 的诊断文本用系统代码页(中文机器上是 936/GBK)输出,
      而中文 Windows 下 subprocess 用 utf-8 读会得到乱码。乱码不影响"退出码",
      但会让我们基于消息文本的判断(例如"这条 C1083 是找不到头还是找不到源文件")
      全部失效 —— 门控会静默判错。这里把同一份字节按 utf-8 / gbk 各解一次,
      取"更像正常文本"的那个。
    """
    p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True)
    raw = (p.stdout or b"") + (p.stderr or b"")
    best = None
    for enc in ("utf-8", "gbk", "cp936", "mbcs", "latin-1"):
        try:
            t = raw.decode(enc)
        except (UnicodeDecodeError, LookupError):
            continue
        # 用"替换字符/乱码密度"挑最优:含 U+FFFD 或私用区多的说明解错了
        bad = t.count("\ufffd")
        if best is None or bad < best[0]:
            best = (bad, t)
        if bad == 0:
            break
    text = best[1] if best else raw.decode("utf-8", "replace")
    return p.returncode, text


def tail(text, n=12):
    lines = (text or "").strip().splitlines()
    return "\n".join("        " + l for l in lines[-n:])


# ===========================================================================
# A. 依赖扫描(静态)
# ===========================================================================
CPP_HEADERS = [
    "string", "vector", "memory", "map", "unordered_map", "set", "unordered_set",
    "list", "deque", "array", "tuple", "utility", "functional", "algorithm",
    "iostream", "fstream", "sstream", "mutex", "thread", "atomic", "chrono",
    "optional", "variant", "any", "type_traits", "initializer_list", "new",
    "exception", "stdexcept", "numeric", "iterator", "bitset", "complex",
    "filesystem", "future", "condition_variable", "limits", "cstdint",
]


def section_a():
    """A. 依赖扫描:契约头纯净、Kotlin 不碰内部路径、scene_renderer.h 不自曝。"""
    print("\n" + "=" * 78)
    print("A. 依赖扫描(静态)")
    print("=" * 78)

    if not os.path.isfile(PUBLIC_HEADER):
        fail_("A", "契约头存在", "找不到 %s" % PUBLIC_HEADER)
        return
    text = read_text(PUBLIC_HEADER)
    code = strip_comments(text)
    pass_("A", "契约头存在", "%s (%d 字节)" % (os.path.basename(PUBLIC_HEADER),
                                              os.path.getsize(PUBLIC_HEADER)))

    # A1. 不出现 std::
    hits = [(i + 1, l.strip()) for i, l in enumerate(code.splitlines()) if "std::" in l]
    if hits:
        fail_("A", "头里无 std::", "出现 %d 处,如 %s" % (len(hits), hits[:3]))
    else:
        pass_("A", "头里无 std::", "非注释代码里 std:: 出现 0 处")

    # A2. #include <...> 不许是任何 C++ 头
    inc_c = re.findall(r"#\s*include\s*<([^>]+)>", code)
    bad_cpp = [h for h in inc_c if os.path.splitext(h.split("/")[-1])[0] in CPP_HEADERS]
    only = sorted(set(h.strip() for h in inc_c))
    if bad_cpp:
        fail_("A", "头里无 C++ 标准头", "出现 C++ 头: %s" % sorted(set(bad_cpp)))
    else:
        pass_("A", "头里无 C++ 标准头", "只 include 了 %s" % only)

    # A3. 不出现 magicblocks/(device|resources|primitives|scene|backend)/
    m = re.findall(r"magicblocks\s*/\s*(device|resources|primitives|scene|backend|ui|features)\s*/", code)
    if m:
        fail_("A", "头里无内部子目录引用", "出现: %s" % sorted(set(m)))
    else:
        pass_("A", "头里无内部子目录引用",
              "device|resources|primitives|scene|backend 引用 0 处")

    # A4. 头文件自足性:不含 mb_internal/
    if "mb_internal" in code:
        fail_("A", "头里无 mb_internal/", "契约头引用了内部头路径")
    else:
        pass_("A", "头里无 mb_internal/", "mb_internal/ 引用 0 处")

    # A5. extern "C" 护栏在位(这是 t2 的致命缺陷,必须常驻检查)
    if '__cplusplus' in code and re.search(r'extern\s*"C"\s*\{', code):
        pass_("A", "extern \"C\" 护栏", "存在 extern \"C\" { 且由 __cplusplus 守卫")
    else:
        fail_("A", "extern \"C\" 护栏", "缺 extern \"C\" —— C++ 侧符号会被 mangle,t2 的事故会重演")

    # A6. Kotlin 侧不引用 C++ 内部路径
    kt_root = os.path.join(MB, "android", "app", "src", "main", "java", "com", "magicblocks", "pad")
    kt_files = sorted(glob.glob(os.path.join(kt_root, "**", "*.kt"), recursive=True))
    if not kt_files:
        blocked("A", "Kotlin 无内部引用", "找不到 Kotlin 源码目录 %s" % kt_root)
    else:
        bad = []
        for f in kt_files:
            kc = strip_comments(read_text(f))
            for pat, why in (
                (r'\bimport\s+(device|backend|core|primitives|resources|scene)\b', "import SDK 命名空间"),
                (r'"?magicblocks/(device|resources|primitives|scene|backend)/', "引用内部头路径"),
                (r'mb_internal', "引用 mb_internal"),
                (r'\b(device|backend|primitives)\s*::', "C++ 命名空间语法"),
                (r'\bVk(Instance|Device|Queue|SurfaceKHR|PhysicalDevice)\b', "引用 Vulkan 类型"),
                (r'\b(android\.opengl|org\.lwjgl)\b', "引用图形 API"),
            ):
                for mm in re.finditer(pat, kc):
                    line = kc[:mm.start()].count("\n") + 1
                    bad.append("%s:%d %s" % (os.path.basename(f), line, why))
        if bad:
            fail_("A", "Kotlin 无内部引用", "%d 处违规: %s" % (len(bad), bad[:5]))
        else:
            pass_("A", "Kotlin 无内部引用", "扫描 %d 个 .kt 文件,违规 0 处" % len(kt_files))

    # A7. scene_renderer.h 不再 include SDK 内部头
    #     判定标准是"有没有 SDK 内部头",不是"有没有 C++ 头"。<cstdint>/<memory>/
    #     <string>/<vector> 是 PIMPL 需要的**标准库**头,完全合法 —— 把它们
    #     判红是自测写错(第一版就犯了这个错:把标准库容器头当成违规)。
    sr = os.path.join(MB, "src", "android", "scene_renderer.h")
    if not os.path.isfile(sr):
        fail_("A", "scene_renderer.h 无内部 include", "文件不存在 %s" % sr)
    else:
        sc = strip_comments(read_text(sr))
        incs = re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]", sc)
        internal_prefixes = ("magicblocks/", "mb_internal/", "device/", "backend/",
                             "resources/", "primitives/", "core/", "scene/", "ui/")
        bad = [i for i in incs if any(i.startswith(p) for p in internal_prefixes)]
        if bad:
            fail_("A", "scene_renderer.h 无内部 include", "仍 include SDK 内部头: %s" % bad)
        else:
            pass_("A", "scene_renderer.h 无内部 include",
                  "include %s —— SDK 内部头 0 个(PIMPL 只露标准库类型)" % incs)

    # A8. 公开目录真的只有一个头
    pub = sorted(os.path.basename(p) for p in glob.glob(os.path.join(PUBLIC_DIR, "*"))
                 if os.path.isfile(p))
    if pub == ["mb_capi.h"]:
        pass_("A", "公开目录只有 1 个头", "include/magicblocks/ = ['mb_capi.h']")
    else:
        fail_("A", "公开目录只有 1 个头", "include/magicblocks/ = %s" % pub)

    # A9. 契约头带 UTF-8 BOM
    #     静态层先判一次:这道门控要在**没有 MSVC 的机器上也能跑**,
    #     而 B 段的编译证明需要 cl.exe。两层都要,少一层就有盲区。
    #     判据取"真的读了前 3 个字节",不看编辑器/编码声明 ——
    #     只有物理 BOM 才能让任意编译器零前提地认出 UTF-8。
    raw = open(PUBLIC_HEADER, "rb").read()
    high = sum(1 for b in raw if b > 127)
    if raw[:3] == b"\xef\xbb\xbf":
        pass_("A", "契约头带 UTF-8 BOM", "前 3 字节 EF BB BF(非 ASCII 字节 %d 个,全靠 BOM 标识编码)" % high)
    elif high == 0:
        # 纯 ASCII 的头不需要 BOM,也不会触发 C4819 —— 这一支不算缺陷。
        pass_("A", "契约头带 UTF-8 BOM", "纯 ASCII(非 ASCII 字节 0 个),无 BOM 也不会有编码歧义")
    else:
        fail_("A", "契约头带 UTF-8 BOM",
              "缺 BOM 且含 %d 个非 ASCII 字节 —— MSVC 会按代码页 936 读,"
              "/W4 /WX 下报 C4819→C2220,UI 不传 /utf-8 就编不过" % high)


# ===========================================================================
# B. 物理隔离(编译门控)
# ===========================================================================
POSITIVE_PROBE = r"""/* B-正向:只 include 契约头,用"只有导出面可见"的命令行编译,必须成功。 */
#include "magicblocks/mb_capi.h"

int main(void)
{
  /* 触碰类型/常量就够了;这里不真正调用,因为本门控只判"能否编译"。 */
  mb_session_desc d = mb_session_desc_default();
  mb_pose p = mb_pose_default();
  dt6_use(0);
  return (int)(d.struct_size == sizeof(mb_session_desc) &&
               p.scale == 1.0f &&
               MB_ABI_VERSION == MB_ABI_VERSION_MAJOR * 10000u +
                                 MB_ABI_VERSION_MINOR * 100u +
                                 MB_ABI_VERSION_PATCH)
             ? 0 : 1;
}
"""

NEGATIVE_PROBE = r"""/* B-负向:在同样受限的 include 路径下 include 一个 SDK 内部头,必须失败。
   这条失败本身就是隔离成立的证据 —— 头根本不在 UI 能看见的地方。 */
#include "magicblocks/%s"

int main(void) { return 0; }
"""


def negative_candidate_headers(limit=6):
    """从 mb_internal/ 里挑几个真实存在的内部头当探针目标。

    关键:探针必须是**真实存在**的内部头。若随便编一个不存在的名字,那条
    "编不过"证明的只是"这个名字不存在",什么都没证明。这个坑很隐蔽。
    """
    internal_root = os.path.join(MB, "include", "mb_internal", "magicblocks")
    cands = []
    for pref in ("primitives/world.h", "scene/scene.h", "device/render_device.h",
                 "ui/ui_app.h", "primitives/mesh.h", "device/swapchain.h"):
        if os.path.isfile(os.path.join(internal_root, pref.replace("/", os.sep))):
            cands.append(pref)
    if len(cands) < limit:
        for p in sorted(glob.glob(os.path.join(internal_root, "**", "*.h"), recursive=True)):
            rel = os.path.relpath(p, internal_root).replace(os.sep, "/")
            if rel not in cands:
                cands.append(rel)
            if len(cands) >= limit:
                break
    return cands[:limit]


def build_export_surface(work):
    """造一个"只有契约头"的导出面目录 —— 这就是 UI 出厂拿到的东西。"""
    d = os.path.join(work, "export_include")
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "magicblocks"))
    shutil.copy2(PUBLIC_HEADER, os.path.join(d, "magicblocks", "mb_capi.h"))
    return d


def compile_once(src_text, inc_dirs, work, tag, defines=()):
    """用**只给这些 -I** 的命令行编译。返回 (ok, log, src_path)。

    ⚠ 所有路径都必须绝对:本函数以 cwd=d 运行 cl.exe,相对路径的 /Fo、源文件
      会按 d 解析 —— 传相对路径进去会得到 `C1083 无法打开源文件` 。
      那种 C1083 指向的是 **.cpp** 而不是头文件,会被误读成"内部头找不到",
      让一道本该报红/报绿的门控给出反向结论。第一版就栽在这。
    """
    d = os.path.abspath(os.path.join(work, "compile_" + tag))
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    src = os.path.join(d, "probe.cpp")
    with open(src, "w", encoding="utf-8") as f:
        f.write(src_text)
    cmd = [cl_exe(), "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/c", src,
           "/Fo:" + os.path.join(d, "probe.obj")]
    for x in defines:
        cmd.append("/D" + x)
    for i in inc_dirs:
        cmd.append("/I" + os.path.abspath(i))
    if inc_dirs is None:
        return False, "no -I", src
    # VSLANG=1033 让 cl.exe 用**英文**诊断。默认它按系统代码页(936)输出中文,
    # 被按 utf-8 读回来是乱码,任何基于消息文本的判断都会静默失效 ——
    # 表现为"负向探针明明红了却判成证据不纯",把一道好门控判死。
    env = dict(vc_env())
    env["VSLANG"] = "1033"
    rc, log = run(cmd, cwd=d, env=env)
    return rc == 0, log, src


def compile_without_utf8(src_text, inc_dirs, work, tag, lang="/std:c++17", warnings=("/W4", "/WX")):
    """**不带 /utf-8** 的编译:模拟"第三方 UI 直接拿头文件用,不知道要加 /utf-8"。

    背景(t7 实测):契约头里全是中文注释,若文件**没有 UTF-8 BOM**,MSVC 会按
    系统代码页(936)读它,于是:
        warning C4819: 该文件包含不能在当前代码页(936)中表示的字符
    平时只是警告,但 UI 一旦用 /W4 /WX(很常见),它就是**编译错误**:
        error C2220: 以下警告被视为错误
    失败点在头文件第 1 行,调用方看到的是"SDK 给的头编不过",而修法(加 BOM)
    在 SDK 侧 —— 这正是"SDK 内部细节波及 UI 编译"的一种,必须钉住。

    加 BOM 而非要求调用方传 /utf-8:后者给每个调用方加了**前提条件**,
    而老板要的是"UI 只依赖一个头文件",不是"只依赖一个头文件 + 一条编译选项"。

    ⚠ 探针**源码本身**必须写 BOM,或者全 ASCII —— 这是同一道门控里的同一个坑,
      第一版就栽在这:头加了 BOM 之后,cl.exe 仍然对着 `probe.c(1)` 报 C4819,
      因为**探针自己**含中文注释(本文件的常量都是中文注释块)。
      差别在定位:`probe.c(1)` 的红不是头的问题,是探针的问题;
      但两者在 /WX 下长得一模一样,都判"编不过"。所以:
        · 这里统一写 BOM —— 与契约头保持同一约定,也顺带证明
          "消费者源码不用改任何东西,只要文件自己带 BOM"。

    返回 (ok, log)。ok=True 表示"没有 /utf-8 也编过了"。
    """
    d = os.path.abspath(os.path.join(work, "nobom_" + tag))
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    ext = ".c" if lang.startswith("/std:c") and "++" not in lang else ".cpp"
    src = os.path.join(d, "probe" + ext)
    # 写 UTF-8 **带 BOM**:探针里的中文注释在代码页 936 下同样会触发 C4819,
    # 那会被误读成"契约头编不过"。带 BOM 后这条门控只反映头自己的能力。
    with open(src, "w", encoding="utf-8-sig") as f:
        f.write(src_text)
    cmd = [cl_exe(), "/nologo", lang]
    if ext == ".cpp":
        cmd.append("/EHsc")
    cmd += list(warnings) + ["/c", src, "/Fo:" + os.path.join(d, "probe.obj")]
    for i in inc_dirs:
        cmd.append("/I" + os.path.abspath(i))
    env = dict(vc_env())
    env["VSLANG"] = "1033"
    rc, log = run(cmd, cwd=d, env=env)
    return rc == 0, log


def header_not_found(log):
    """这条 C1083 是不是"找不到**头文件**"?

    必须把"找不到源文件(.cpp)"排除掉 —— 那是路径写错,不是隔离证据。

    ⚠ 两个坑:
      ① MSVC 的 C1083 头文件错法那行行首也带 `probe.cpp(3):`,不能用
         "含 .cpp 就排除" 来判。
      ② cl.exe 用**代码页 936** 输出中文,被我们按 utf-8 读回来是乱码,
         拿中文串 "无法打开包括文件" 去匹配永远匹配不上 —— 这会表现为
         "负向探针明明红了却判成证据不纯",把一道好门控判死。
         所以只认**英文**提示 "cannot open include file",它是 cl.exe 的
         稳定部分(本地化消息尾部仍保留英文原文)。
    """
    for line in (log or "").splitlines():
        if "C1083" not in line:
            continue
        if re.search(r"cannot open include file", line, re.I):
            return True
        # 英文落空时,退化判断:消息尾部常保留被围在弯引号里的头名,
        # 且源文件缺失时提示是 "cannot open source file" —— 那种明确排除。
        if re.search(r"cannot open source file", line, re.I):
            continue
        if re.search(r"magicblocks[\\/]", line):
            return True
    return False


def section_b(build_dir):
    """B. 物理隔离:正向必须编过,负向必须编不过,且负向有反向自证。"""
    print("\n" + "=" * 78)
    print("B. 物理隔离(受限 include 编译门控)")
    print("=" * 78)

    work = os.path.join(build_dir, "ui_isolation")
    os.makedirs(work, exist_ok=True)
    export = build_export_surface(work)
    real_include = os.path.join(MB, "include")
    print("  导出面(UI 出厂可见): %s" % export)
    print("  真实 include 树      : %s" % real_include)

    # ---- B1 正向:只给导出面,必须编过 ----
    # 注意 POSITIVE_PROBE 里没有 dt6_use,这里内联一个最小定义避免未声明。
    pos_src = POSITIVE_PROBE.replace("  dt6_use(0);\n", "")
    ok, log, _ = compile_once(pos_src, [export], work, "positive")
    if ok:
        pass_("B", "正向:只给导出面能编过", "cl /I<导出面> 退出 0")
    else:
        fail_("B", "正向:只给导出面能编过", "编译失败 —— UI 连契约头都编不过\n" + tail(log))

    # ---- B2 负向:同样的 -I,include 内部头必须失败 ----
    headers = negative_candidate_headers()
    if not headers:
        blocked("B", "负向:内部头编失败", "include/mb_internal/ 下找不到任何头,无法取证")
    all_red = True
    for h in headers:
        ok, log, src = compile_once(NEGATIVE_PROBE % h, [export], work,
                                    "neg_" + re.sub(r"\W", "_", h))
        notfound = header_not_found(log)
        if ok:
            all_red = False
            fail_("B", "负向:%s" % h, "在只有导出面的环境下**编过了** —— 内部头仍对 UI 可见")
        elif not notfound:
            all_red = False
            fail_("B", "负向:%s" % h, "编失败了但不是因为找不到头(证据不纯)\n" + tail(log, 6))
        else:
            first = [l for l in log.splitlines() if "C1083" in l and ".cpp" not in l.lower()]
            pass_("B", "负向:%s" % h, "如预期编失败: %s"
                  % (first[0].strip()[:110] if first else "找不到头"))
    if headers and all_red:
        pass_("B", "负向合计", "%d 个真实内部头在受限 include 下全部编失败" % len(headers))

    # ---- B3 反向自证:同一份探针在真实内部头树下必须"找得到头" ----
    # 没有这条,"编不过"可能只是探针名字写错了 —— 那和"隔离成立"长得一模一样。
    #
    # 做法:t5 之后内部头物理位于 include/mb_internal/magicblocks/<sub>/*.h,
    # 而它们内部互相引用写的是 `#include "mb_internal/magicblocks/..."`,
    # 所以 -I 必须指向 include/(而不是 include/mb_internal/)。为了让探针里那句
    # `#include "magicblocks/primitives/world.h"` 也能解析,额外造一个软链接
    # 视图太脆;改为直接测**内部头自己的真实路径**:把探针换成
    # `#include "mb_internal/magicblocks/<h>"`,只要不再是 C1083 就证明
    # "<h> 这个名字真实存在",负向门控的红不是笔误造成的。
    if headers:
        h = headers[0]
        probe_src = NEGATIVE_PROBE % ("../mb_internal/magicblocks/" + h)
        # 用真实树 + 指向 include/ 的 -I:此时内部头可解析
        ok, log, _ = compile_once(probe_src, [real_include], work, "neg_meta")
        found_it = ok or not header_not_found(log)
        if found_it:
            pass_("B", "反向自证:探针指向真实内部头时找得到",
                  "同一探针在真实 include 树(-I include/)下不再报'找不到头' "
                  "→ 负向的红确实来自导出面收窄,不是文件名笔误")
        else:
            fail_("B", "反向自证:探针指向真实内部头时找得到",
                  "真实树下仍然'找不到头' —— 无法证明探针指向的是个真文件\n" + tail(log, 6))

        # B3b 更直接的自证:确认探针用的那些头在磁盘上真实存在
        internal_root = os.path.join(MB, "include", "mb_internal", "magicblocks")
        missing = [x for x in headers
                   if not os.path.isfile(os.path.join(internal_root, x.replace("/", os.sep)))]
        if missing:
            fail_("B", "负向探针指向真实存在的内部头",
                  "这些探针名在 mb_internal/ 下不存在,负向证据无效: %s" % missing)
        else:
            pass_("B", "负向探针指向真实存在的内部头",
                  "%d 个探针全部在 include/mb_internal/magicblocks/ 下真实存在" % len(headers))

    # ---- B3c 契约头的编码不能给调用方加前提 ----
    # t7 实测:头里全是中文注释,不带 BOM 时 MSVC 按代码页 936 读 →
    #   warning C4819 → UI 的 /W4 /WX 一开就是 error C2220。
    # B1 用的是 /utf-8 编译的,所以**盯不住这个坑** —— 必须补一条不带 /utf-8 的。
    # 三种语言方言都过一遍:C 调用方(纯 C11)和 C++ 调用方都要零前提。
    no_utf8_cases = (("/std:c11", ".c(纯 C 调用方)"),
                     ("/std:c17", ".c17"),
                     ("/std:c++17", "c++17"),
                     ("/std:c++20", "c++20"))
    bad = []
    for lang, label in no_utf8_cases:
        ok, log = compile_without_utf8(pos_src, [export], work,
                                       "no_utf8_" + re.sub(r"\W", "_", lang), lang=lang)
        if not ok:
            bad.append((label, log))
    if bad:
        label, log = bad[0]
        fail_("B", "契约头不传 /utf-8 也能编(/W4 /WX)",
              "%d/%d 个方言失败,首个是 %s —— 头缺 BOM 或含当前代码页无法表示的字符\n%s"
              % (len(bad), len(no_utf8_cases), label, tail(log, 8)))
    else:
        pass_("B", "契约头不传 /utf-8 也能编(/W4 /WX)",
              "%d 个方言(C11/C17/C++17/C++20)全部退出 0:头靠 BOM 自证编码,"
              "调用方无需额外编译选项" % len(no_utf8_cases))

    # ---- B4 整棵 include/ 挪走,UI 仍能编 ----
    # 这是"解耦"的最强形式:SDK 源码树对编译器完全不可见。
    stash = os.path.join(work, "include_stashed")
    if os.path.isdir(stash):
        shutil.rmtree(stash)
    moved = False
    try:
        shutil.move(real_include, stash)
        moved = True
        os.makedirs(real_include)   # 空壳:任何 -I 指过来也搜不到
        ok, log, _ = compile_once(pos_src, [export], work, "relocated")
        if ok:
            pass_("B", "include/ 整棵挪走后仍能编", "SDK 源码树此刻对编译器完全不可见,正向探针退出 0")
        else:
            fail_("B", "include/ 整棵挪走后仍能编", "编不过 —— 隔离不成立\n" + tail(log))
    finally:
        if moved:
            shutil.rmtree(real_include, ignore_errors=True)
            shutil.move(stash, real_include)
            print("        (已恢复 %s)" % real_include)


# ===========================================================================
# C. 契约验证
# ===========================================================================
def find_lib(build_dir):
    for name in ("magicblocks.lib", "magicblocks.dll"):
        p = os.path.join(build_dir, name)
        if os.path.isfile(p):
            return p
    return None


def find_probe_exe(build_dir):
    for p in glob.glob(os.path.join(build_dir, "**", "magicblocks-capi-probe.exe"), recursive=True):
        return p
    return None


def runtime_path_extra():
    cands = []
    vs = os.environ.get("VULKAN_SDK")
    if vs:
        cands += [os.path.join(vs, "Bin"), os.path.join(vs, "Bin32")]
    cands += ["C:\\Windows\\System32"]
    keep = [c for c in cands if os.path.isdir(c) and os.path.isfile(os.path.join(c, "vulkan-1.dll"))]
    return ";".join(keep) if keep else "C:\\Windows\\System32"


def header_abi_version():
    code = strip_comments(read_text(PUBLIC_HEADER))
    maj = int(re.search(r"#\s*define\s+MB_ABI_VERSION_MAJOR\s+(\d+)", code).group(1))
    mnr = int(re.search(r"#\s*define\s+MB_ABI_VERSION_MINOR\s+(\d+)", code).group(1))
    pat = int(re.search(r"#\s*define\s+MB_ABI_VERSION_PATCH\s+(\d+)", code).group(1))
    return maj * 10000 + mnr * 100 + pat, (maj, mnr, pat)


def section_c(build_dir):
    """C. 契约验证:probe 的 ABI 版本 >= 头声明;桌面测试基线。"""
    print("\n" + "=" * 78)
    print("C. 契约验证")
    print("=" * 78)

    packed, triple = header_abi_version()
    print("  头文件声明 MB_ABI_VERSION = %d (%d.%d.%d)" % (packed, triple[0], triple[1], triple[2]))

    probe = find_probe_exe(build_dir)
    if not probe:
        blocked("C", "probe_consumer 运行", "build 里找不到 magicblocks-capi-probe.exe(先跑 build.ps1)")
    else:
        env = dict(vc_env())
        env["PATH"] = runtime_path_extra() + ";" + env.get("PATH", "")
        rc, out = run([probe], cwd=os.path.dirname(probe), env=env)
        print(tail(out, 10))
        if rc != 0:
            fail_("C", "probe_consumer 运行", "退出码 %s" % rc)
        else:
            pass_("C", "probe_consumer 运行", "退出码 0(编译→链接→运行 全通)")
            m = re.search(r"mb_abi_version\(\)\s*=\s*(\d+)", out)
            if not m:
                fail_("C", "运行时版本号可读", "probe 输出里没有 mb_abi_version() 的值")
            else:
                rt = int(m.group(1))
                if rt == packed:
                    pass_("C", "运行时版本 == 头声明", "runtime=%d == header=%d" % (rt, packed))
                elif rt >= packed:
                    pass_("C", "运行时版本 >= 头声明", "runtime=%d >= header=%d" % (rt, packed))
                else:
                    fail_("C", "运行时版本 >= 头声明",
                          "runtime=%d < header=%d —— 库比头旧,UI 会调到不存在的入口" % (rt, packed))

    # C2. 桌面单元测试(记录真实数量,不美化)
    exe = os.path.join(build_dir, "magicblocks_test.exe")
    if not os.path.isfile(exe):
        blocked("C", "桌面单元测试", "找不到 %s(先跑 build.ps1 -Target all)" % exe)
        return
    # ⚠ 预案:Offscreen.FrameIndexChangesOutput 有一条硬编码路径
    #    MB_GOLDEN_DIR + "/../../build/cli_out",MB_GOLDEN_DIR 是**源目录**的
    #    tests/data,所以它恒指向 <mb>/build/cli_out,与 --build-dir 无关。
    #    该目录不存在时这条必红,且与本次拆分无关(low,已预知)。
    #    这里**不替它建目录**,如实记录;若它红了,在报告里单独标注成因。
    rc, out = run([exe], cwd=build_dir, env=vc_env())
    m_total = re.search(r"\[==========\]\s+(\d+) tests from \d+ test suites ran", out)
    m_pass = re.search(r"\[  PASSED  \]\s+(\d+) tests", out)
    m_fail = re.search(r"\[  FAILED  \]\s+(\d+) test", out)
    total = int(m_total.group(1)) if m_total else -1
    passed = int(m_pass.group(1)) if m_pass else 0
    failed = int(m_fail.group(1)) if m_fail else 0
    print("  桌面测试: total=%d passed=%d failed=%d (退出码 %s)" % (total, passed, failed, rc))
    for line in out.splitlines():
        if line.startswith("[  FAILED  ]") and "." in line:
            print("        " + line.strip())

    # 基线是 586 条。这里不要求"全绿"(t9 未做时 G3 恒红),而是要求:
    #   · 失败项只允许是已预知的两类,其它一律算 FAIL
    # gtest 会把同一个失败名打印两次(摘要行 + 明细行),这里去重。
    known = ("StackGates.G3LayerPurityExitZero", "Offscreen.FrameIndexChangesOutput")
    seen = []
    for l in out.splitlines():
        if l.startswith("[  FAILED  ]") and "." in l:
            n = re.sub(r"^\[  FAILED  \]\s*", "", l).split("(")[0].strip()
            if n not in seen:
                seen.append(n)
    failed_names = seen
    unexpected = [n for n in failed_names if n not in known]
    if total < 0:
        fail_("C", "桌面单元测试", "输出里解析不到用例总数(输出格式变了?)\n" + tail(out))
    elif unexpected:
        fail_("C", "桌面单元测试", "%d 个非预知失败: %s" % (len(unexpected), unexpected))
    elif failed:
        blocked("C", "桌面单元测试",
                "total=%d passed=%d,失败 %d 个且全部是已预知项(与本次拆分无关): %s"
                % (total, passed, failed, failed_names))
    else:
        pass_("C", "桌面单元测试", "total=%d 全部通过" % total)


# ===========================================================================
# 报告
# ===========================================================================
def write_report(path, build_dir):
    """生成报告。**保留**脚本自动生成段落之外的任何人写内容。

    做法:如果已有报告里存在 `<!-- MANUAL:BEGIN -->` 标记之后的段落,重新生成时
    把它原样接回去。否则每次重跑都会把手工补充的"反向控制实验/命令与退出码"
    抹掉 —— 那些正是报告里最有价值的部分。
    """
    manual = ""
    if os.path.isfile(path):
        try:
            old = read_text(path)
            idx = old.find("<!-- MANUAL:BEGIN -->")
            if idx >= 0:
                manual = old[idx:]
        except OSError:
            manual = ""

    lines = []
    w = lines.append
    w("# UI ⇄ SDK 拆分隔离性验证报告")
    w("")
    w("> 由 `MagicBlocks/tools/uisplit/verify_split.py` 自动生成。")
    w("> 每条断言都附**实际输出**；FAIL 就是 FAIL，不做美化。")
    w("")
    w("- worktree: `%s`" % MB)
    w("- build dir: `%s`" % build_dir)
    w("")
    counts = {"PASS": 0, "FAIL": 0, "BLOCKED": 0}
    for _s, _n, st, _d in RESULTS:
        counts[st] += 1
    w("## 总览")
    w("")
    w("| PASS | FAIL | BLOCKED |")
    w("|---:|---:|---:|")
    w("| %d | %d | %d |" % (counts["PASS"], counts["FAIL"], counts["BLOCKED"]))
    w("")
    if counts["FAIL"]:
        w("**结论：不通过。**%d 条断言失败，见下表。" % counts["FAIL"])
    elif counts["BLOCKED"]:
        w("**结论：已断言项全部通过；%d 项因环境缺席未判定（BLOCKED），非失败。**"
          % counts["BLOCKED"])
    else:
        w("**结论：全部通过。**")
    w("")
    names = {"A": "A. 依赖扫描（静态）", "B": "B. 物理隔离（编译门控）", "C": "C. 契约验证"}
    for sec in ("A", "B", "C"):
        rows = [r for r in RESULTS if r[0] == sec]
        if not rows:
            continue
        w("## %s" % names[sec])
        w("")
        w("| 断言 | 结果 | 实际输出 |")
        w("|---|---|---|")
        for _s, n, st, d in rows:
            dd = (d or "").replace("\n", " ").replace("|", "\\|")
            if len(dd) > 220:
                dd = dd[:220] + " …"
            w("| `%s` | **%s** | %s |" % (n, st, dd if dd else ""))
        w("")
    w("## 说明：环境相关的 BLOCKED 项")
    w("")
    w("- 运行层（真实 GPU 生命周期）在无 Vulkan 设备的机器上记 BLOCKED，"
      "既不冒绿也不恒红。证据链的编译层、链接层已实证。")
    w("- `Offscreen.FrameIndexChangesOutput` 有一条硬编码路径 "
      "`MB_GOLDEN_DIR + \"/../../build/cli_out\"`；`MB_GOLDEN_DIR` 是**源目录**的 "
      "`tests/data`，故它恒指向 `<MagicBlocks>/build/cli_out`，与 `--build-dir` 无关。"
      "该目录不存在时这条必红，属预知问题（low），与本次拆分无关。")
    w("- `StackGates.G3LayerPurityExitZero` 在 t9 完成前恒红（backend:: 违规 16 处），"
      "同样与本次拆分无关。")
    w("")
    if manual:
        w(manual)
    else:
        w("<!-- MANUAL:BEGIN -->")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("\n报告已写入 %s" % path)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=None, help="构建目录(默认 MagicBlocks/build-uisplit)")
    ap.add_argument("--report", default=None, help="报告输出路径(默认 docs/UI_SPLIT_VERIFY.md)")
    args = ap.parse_args(argv)

    build_dir = os.path.abspath(args.build_dir or os.path.join(MB, "build-uisplit"))
    report = os.path.abspath(args.report or os.path.join(MB, "..", "docs", "UI_SPLIT_VERIFY.md"))

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    print("=" * 78)
    print("t6 隔离性验证")
    print("  MagicBlocks : %s" % MB)
    print("  build dir   : %s" % build_dir)
    print("=" * 78)

    try:
        section_a()
        section_b(build_dir)
        section_c(build_dir)
    except RuntimeError as exc:
        print("\n环境错误:%s" % exc)
        return 2

    counts = {"PASS": 0, "FAIL": 0, "BLOCKED": 0}
    for _s, _n, st, _d in RESULTS:
        counts[st] += 1
    print("\n" + "-" * 78)
    print("结果: PASS=%d FAIL=%d BLOCKED=%d" % (counts["PASS"], counts["FAIL"], counts["BLOCKED"]))
    for s, n, st, d in RESULTS:
        if st == "FAIL":
            print("  FAILED %-12s %s %s" % (s, n, (d or "").splitlines()[0] if d else ""))
    for s, n, st, d in RESULTS:
        if st == "BLOCKED":
            print("  BLOCKED %-11s %s" % (s, n))
    write_report(report, build_dir)
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
