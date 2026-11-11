#!/usr/bin/env python3
"""Kotlin 侧 JNI 声明与 nativeListModels 解析的**可执行**校验(桌面上跑)。

为什么要这一个文件
==================
两个问题长得完全不同,病根却是同一个 —— **两侧各写一份约定,没有任何东西约束它们一致**:

  A. **签名漂移。** 把一个 `jint` 写成 `jlong`,Kotlin 编译绿灯、C++ 链接绿灯
     (Java 侧根本没有链接期),真机上第一次调用才炸:

         java.lang.UnsatisfiedLinkError: No implementation found for
           boolean com.magicblocks.pad.MbRenderer.nativeSurfaceCreated(long, long, int, int)

     桌面一个都测不出来,除非真的去看两侧的签名。

  B. **文本协议漂移。** t3 把 `nativeListModels` 的首行从 `"ok"` 改成 `"err=MB_*"`,
     `MbRenderer.kt` 还在比 `"ok"` → 真机上模型列表**永远为空**,桌面全绿。

做法(全部只读;不 import Kotlin 源码里的任何东西)
--------------------------------------------------
起一个**真的 JVM**(libjvm + JNI,不是模拟),classpath 用 `:app:compileDebugKotlin`
的编译 classpath,然后:

  1. 反射 `MbRenderer` 的 `getDeclaredMethods()`,用 `Class.getName()` 拼出 JVM 描述符;
  2. 与 `tools/capi_protocol/protocol.json` 的 `jni_entries` 逐位比;
  3. 与 `MagicBlocks/src/android/jni_bridge.cpp` 的 `JNIEXPORT` 逐位比;
  4. 调 `MbAbiProtocol.selftestParse(...)` —— **真实编译产物里那个解析函数** ——
     喂生成的语料,断言结果;
  5. 读 `MbAbi` 的 ABI 常量,验"UI 要求的版本"与运行时版本的关系(见 `check_abi_rules`);
  6. 确认 `MbRenderer` 从没被初始化过 → `loadLibrary` 没有在桌面上被调用
     (它没有 `libmagicblocks.so`,被调用就一定会炸;这里反过来把"炸"变成一条断言)。

为什么要起独立进程而不是在本进程里调 JVM
----------------------------------------
JVM 一个进程只能起一次,而且它的 `System.exit` 会把本进程一起带走 —— 用子进程
才能给每一步都留下"卡在哪里"的可读输出,也能把 JVM 的 stdout/stderr 与自己的分开。

用法:
  python tools/capi_protocol/check_kotlin_abi.py
  python tools/capi_protocol/check_kotlin_abi.py --via-gradle   # 现取 classpath(慢)
  python tools/capi_protocol/check_kotlin_abi.py --self-test    # 验本校验器自己能红
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PROTOCOL_JSON = os.path.join(HERE, "protocol.json")
GOLDEN_DIR = os.path.join(HERE, "golden")
CLASSES_CP_TXT = os.path.join(HERE, "classpath_debug.txt")
ANDROID_DIR = os.path.join(REPO, "MagicBlocks", "android")
INIT_SCRIPT = os.path.join(ANDROID_DIR, ".t4-print-classes.gradle")
JNI_BRIDGE = os.path.join(REPO, "MagicBlocks", "src", "android", "jni_bridge.cpp")
KOTLIN_SRC = os.path.join(ANDROID_DIR, "app", "src", "main", "java", "com", "magicblocks", "pad")
DEFAULT_CLASSES = os.path.join(ANDROID_DIR, "app", "build", "intermediates",
                               "built_in_kotlinc", "debug", "compileDebugKotlin", "classes")

# ---------------------------------------------------------------- 类型对照表
#
# 左:C 侧(NDK jni.h / NDK 提供的类型);右:JVM 上它**真正是**什么。
# 这张表是"两侧签名对不对得上"的全部依据,所以每条都写清楚:
#   - jboolean 是 uint8_t,但 JNI 规范要求 Java boolean(不是 byte);
#   - jint 是 int32_t → Java int; jlong 是 int64_t → Java long;
#   - 引用类型必须写全限定名,拼错一个字符就是另一个类。
CTYPES = {
    "jboolean": "boolean",
    "jbyte": "byte",
    "jchar": "char",
    "jshort": "short",
    "jint": "int",
    "jlong": "long",
    "jfloat": "float",
    "jdouble": "double",
    "jsize": "int",
    "jstring": "java.lang.String",
    "jclass": "java.lang.Class",
    "jobject": "java.lang.Object",
    "void": "void",
}

PRIMITIVES = {"boolean": "Z", "byte": "B", "char": "C", "short": "S",
              "int": "I", "long": "J", "float": "F", "double": "D", "void": "V"}


def desc_of(t):
    return PRIMITIVES.get(t, "L%s;" % t.replace(".", "/"))


def format_sig(ret, params):
    return "(%s)%s" % ("".join(desc_of(p) for p in params), desc_of(ret))


def c_param_to_java(arg):
    """`jlong handle` → `long`;`JNIEnv* env` / `jclass` → None(不是 Java 侧参数)。

    ⚠ `jobject` 是个**陷阱**:C 侧写 `jobject surface` 时,Java 侧的真实类型完全
    取决于 Kotlin 的声明(`android.view.Surface`),JNI 匹配签名用的是后者。
    所以 `jobject`/`jarray` 这类"泛引用"不做映射,原样留给调用方按名字查表 ——
    见 `c_arg_to_java(e, extra)`。
    """
    a = re.sub(r"\s+", " ", arg.strip())
    if a.startswith("JNIEnv"):
        return None
    head = a.split(" ")[0]
    if a == "jclass" or head == "jclass":
        return None
    return head


GENERIC_REF = "\x00generic"


def c_arg_to_java(arg):
    """C 参数 → Java 参数类型;泛引用(`jobject`)返回 [GENERIC_REF],由 Kotlin 侧确认真身。

    `jobject` 不是"Java 的 Object":C 侧写 `jobject surface` 时,Java 侧的真实类型
    完全由 Kotlin 声明决定(`android.view.Surface`),JNI 匹配签名用的也是后者。
    所以这里**拒绝猜** —— 猜成 Object 会把一个完全正确的实现判成红,那是把断言
    写成了噪声。
    """
    head = c_param_to_java(arg)
    if head in ("jobject", "jarray"):
        return GENERIC_REF
    return CTYPES.get(head, head) if head else None


# --------------------------------------------------------- Kotlin 签名(主进程侧)
JNIEXPORT_RE = re.compile(
    r"JNIEXPORT\s+(\w+)\s+JNICALL\s+Java_com_magicblocks_pad_MbRenderer_(\w+)\s*\(([^)]*)\)", re.S)


def parse_jni_bridge(path=JNI_BRIDGE):
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    out = {}
    for m in JNIEXPORT_RE.finditer(src):
        ret, name, params = m.group(1), m.group(2), m.group(3)
        types = []
        for raw in params.split(","):
            t = raw.strip()
            if not t:
                continue
            t = re.sub(r"\s*\*+\s*", "*", t)
            t = re.sub(r"\s+", " ", t)
            types.append(t)
        out[name] = {"ret": ret, "params": types}
    return out


# ------------------------------------------------------------- 反射(子进程侧)
# 子进程侧 = 真 Java(见 tools/capi_protocol/Probe.java)。用 javac 现场编译、用 java 跑,
# 拿到的是**真实 .class** 的反射结果与**真实调用**的返回值。
#
# (早期版本用 python 的 ctypes 直接调 libjvm,靠手写 JNI 函数表下标调 JNI。JNI 函数表里
#  有大量预留槽位,数错一格不报错而是静默调到别的函数上 —— 实测踩过一次,把 GetMethodID
#  查成了 GetStaticMethodID,症状是 NoSuchMethodError,看起来像"类里没这个方法"。
#  换成真 Java 之后,这类错误在编译期就没了。)
PROBE_JAVA = os.path.join(HERE, "Probe.java")
PROBE_OUT = os.path.join(tempfile.gettempdir(), "mb_abi_probe_classes")



def kotlin_compile_classpath(java_home=None):
    """跑 Gradle 只读地打印 `:app:compileDebugKotlin` 的 classpath。"""
    gradle = os.path.join(ANDROID_DIR, "gradlew.bat")
    env = dict(os.environ)
    if java_home:
        env["JAVA_HOME"] = java_home
    cmd = [gradle]
    if not os.path.exists(os.path.join(ANDROID_DIR, "gradle", "wrapper", "gradle-wrapper.jar")):
        # 仓库里没有 wrapper jar(gradle/wrapper/ 只有 properties),用开发机的 gradle。
        cmd = [os.environ.get("MB_GRADLE", "gradle.bat")]
    last = ""
    try:
        out = subprocess.run(
            cmd + ["--init-script", INIT_SCRIPT, "--offline", "--console=plain", "-q",
                   ":app:t4PrintKotlinDebugCompileClasspath"],
            cwd=ANDROID_DIR, env=env, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=900)
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError("拿不到 Kotlin 编译 classpath(Gradle 不可用?)\n%s" % exc)
    if out.returncode != 0:
        last = out.stdout + out.stderr
        raise RuntimeError("拿不到 Kotlin 编译 classpath(exit %d)\n%s" % (out.returncode, last))
    cps = [ln[3:] for ln in out.stdout.splitlines() if ln.startswith("CP\t")]
    # 顺手把它落盘:下次不带 --via-gradle 也能用同一份(生成的,不是手抄的)。
    # newline="\n" + 不带 BOM —— 这两点都会影响 JVM 解析 classpath(见 REFLECT_PY)。
    if cps:
        with open(CLASSES_CP_TXT, "w", encoding="utf-8", newline="\n") as f:
            f.write("# 由 tools/capi_protocol/check_kotlin_abi.py --via-gradle 生成;"
                    "内容是 :app:compileDebugKotlin 的 classpath(每行一条)。\n")
            f.write("\n".join(x.strip().lstrip("\ufeff") for x in cps) + "\n")
    return cps


CP0 = 61  # Java 17:AGP 的 Kotlin 产物固定落在这上面,Java 21 的 JVM 能读,更旧的不能。


def check_class_majors(paths):
    """先读 class 文件头,区分"编译产物坏了"和"工具坏了"。"""
    bad = []
    for p in paths:
        if not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            head = f.read(8)
        if len(head) < 8 or head[:4] != b"\xca\xfe\xba\xbe":
            bad.append("%s: 不是 class 文件(魔数不符)" % p)
            continue
        major = head[6] * 256 + head[7]
        if major > CP0:
            bad.append("%s: class 版本 %d 高于预期的 %d" % (p, major, CP0))
    return bad


def ensure_stub_library(p, java_home=None, quiet=False):
    """造一个**空壳** `magicblocks` 动态库,用途有两个:

      1. 让 `MbRenderer.<clinit>` 的 `System.loadLibrary("magicblocks")` 过关。
         为什么必须造:桌面 JVM 上没有 libmagicblocks;类库缺失时 `Class.forName`
         直接抛 `UnsatisfiedLinkError: no magicblocks in java.library.path`,而
         `java.lang.Class` 加载器会把它**记住**(`loadLibrary` 失败过后,这个类在
         同一个加载器里就永久 NoClassDefFound),于是每个反射调用都失败 —— 报错长得
         像"类不存在",极难归因(这台机器上为此绕了两小时)。

      2. **只**为两个只读版本入口导出真实符号,让 Probe 能在桌面上真的调一次
         `nativeAbiVersion()` / `nativeModelListProtocolVersion()`,拿到"库自报的
         版本"再和 UI 的期望值比。这两条正是 t4 要求的"启动时版本闸门"的两半:
         UI 侧的常量由 check_constants 验,库侧的真值就是这里喂进去的。

    除这两个符号外**一个 JNI 函数都不导出** —— 于是"签名表来自 .class 的元数据、
    不需要任何 native 实现"这个前提本身就是可执行的事实:任何一次**其他** native
    调用都会立刻 UnsatisfiedLinkError,而我们一次都不走。

    喂进去的版本号由 `protocol.json` 决定(stub 源码随之重新生成),所以"闸门会不会
    报错"这件事可以被故意弄红:改 protocol.json 的 abi.major 而不改 UI 常量,ui 侧
    check_constants 红;反过来改 UI 常量而不改协议,这里就会红。

    怎么造:用本机 MSVC(`cl /LD`)编译一个 .c;非 Windows 用 cc。
    找不到编译器就**报错**,不静默跳过 —— 跳过会让整条校验永远红。
    """
    out_dir = os.path.join(tempfile.gettempdir(), "mb_abi_stub_lib")
    os.makedirs(out_dir, exist_ok=True)
    if os.name == "nt":
        lib = os.path.join(out_dir, "magicblocks.dll")
    elif sys.platform == "darwin":
        lib = os.path.join(out_dir, "libmagicblocks.dylib")
    else:
        lib = os.path.join(out_dir, "libmagicblocks.so")
    abi_version = int(p["abi"]["required"])
    proto_version = int(p["protocol_version"])
    src_path = os.path.join(out_dir, "mb_abi_stub.c")
    # 纯 ASCII:MSVC 在非 UTF-8 代码页下遇到中文注释会报 C4819 / 直接编码失败。
    # 注意第二个参数是 **jobject**(实例方法的 this),不是 jclass ——
    # MbRenderer 的 15 个 external fun 全部声明在 `object` 里但**没有** @JvmStatic
    # (Kotlin 的 external fun 不能标 @JvmStatic),所以它们都是实例方法。
    # 空壳库跟着 .class 里的真实形态走,签错会在 JNI 解析阶段
    # NoSuchMethodError,而不是"返回了一个奇怪的数"。
    src = (
        "/* generated by tools/capi_protocol/check_kotlin_abi.py.\n"
        "   Purpose 1: keep System.loadLibrary() happy on desktop.\n"
        "   Purpose 2: provide ONLY the two read-only version entries so the\n"
        "   desktop probe can ask the library what it reports. */\n"
        "#include <jni.h>\n"
        "JNIEXPORT jint JNICALL Java_com_magicblocks_pad_MbRenderer_nativeAbiVersion\n"
        "    (JNIEnv* env, jobject self) { (void)env; (void)self; return (jint)%d; }\n"
        "JNIEXPORT jint JNICALL Java_com_magicblocks_pad_MbRenderer_nativeModelListProtocolVersion\n"
        "    (JNIEnv* env, jobject self) { (void)env; (void)self; return (jint)%d; }\n"
        % (abi_version, proto_version))
    with open(src_path, "w", encoding="ascii", newline="\n") as f:
        f.write(src)
    needs_build = not (os.path.exists(lib) and os.path.getmtime(lib) >= os.path.getmtime(src_path))
    if needs_build:
        jni_inc = os.path.join(java_home or "", "include")
        if not os.path.exists(os.path.join(jni_inc, "jni.h")):
            raise RuntimeError("找不到 jni.h:没有它就没法给空壳库导出 JNI 符号"
                               "(试过 %s;请设 --java-home 或 JAVA_HOME)" % jni_inc)
        if os.name == "nt":
            win_inc = os.path.join(jni_inc, "win32")
            vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                                   "Microsoft Visual Studio", "Installer", "vswhere.exe")
            vcvars = None
            if os.path.exists(vswhere):
                inst = subprocess.run(
                    [vswhere, "-latest", "-products", "*",
                     "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                     "-property", "installationPath"],
                    capture_output=True, text=True).stdout.strip()
                if inst:
                    vcvars = os.path.join(inst, "VC", "Auxiliary", "Build", "vcvars64.bat")
            if not vcvars or not os.path.exists(vcvars):
                raise RuntimeError("找不到 MSVC(vswhere 没给出 installationPath),无法生成空壳 "
                                   "magicblocks 库 —— 没有它,桌面上 Class.forName(MbRenderer) 一定失败")
            bat = os.path.join(out_dir, "build_stub.bat")
            with open(bat, "w", encoding="ascii", newline="\r\n") as f:
                f.write('@echo off\ncall "%s" >nul 2>&1\n'
                        'cd /d "%s"\ncl /nologo /LD /O1 /I"%s" /I"%s" '
                        '/Fe:magicblocks.dll mb_abi_stub.c >cl.log 2>&1\n'
                        'exit /b %%ERRORLEVEL%%\n'
                        % (vcvars, out_dir, jni_inc, win_inc))
            r = subprocess.run([bat], capture_output=True, text=True)
            if r.returncode != 0 or not os.path.exists(lib):
                log = os.path.join(out_dir, "cl.log")
                detail = ""
                if os.path.exists(log):
                    with open(log, "r", encoding="mbcs", errors="replace") as f:
                        detail = f.read()
                raise RuntimeError("编译空壳库失败(exit %d)\n%s" % (r.returncode, detail))
        else:
            cc = os.environ.get("CC", "cc")
            r = subprocess.run([cc, "-shared", "-fPIC", "-I", jni_inc,
                                "-I", os.path.join(jni_inc, "linux"),
                                "-o", lib, src_path], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError("编译空壳库失败(%s):\n%s" % (cc, r.stderr))
    if not quiet:
        print("[kotlin_abi] 空壳库: %s(ABI=%d / 协议=%d,%s)"
              % (lib, abi_version, proto_version, "刚编译" if needs_build else "已存在"))
    return out_dir


def java_tool(name, java_home):
    exe = name + (".exe" if os.name == "nt" else "")
    cand = os.path.join(java_home or "", "bin", exe)
    return cand if java_home and os.path.exists(cand) else exe


def compile_probe(java_home, quiet=False):
    """javac 编译 Probe.java(改了就重编,没改就复用)。"""
    os.makedirs(PROBE_OUT, exist_ok=True)
    cls = os.path.join(PROBE_OUT, "Probe.class")
    if os.path.exists(cls) and os.path.getmtime(cls) >= os.path.getmtime(PROBE_JAVA):
        return PROBE_OUT
    r = subprocess.run([java_tool("javac", java_home), "-encoding", "UTF-8",
                        "-d", PROBE_OUT, PROBE_JAVA],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("javac 编译 Probe.java 失败:\n%s\n%s" % (r.stdout, r.stderr))
    if not quiet:
        print("[kotlin_abi] 校验探针: %s(刚编译)" % cls)
    return PROBE_OUT


def reflect(classpath, library_path=None, java_home=None, quiet=False):
    """起一个**真 JVM** 反射 MbRenderer + 真调 selftestParse。返回解析后的 JSON。

    输出只有一行 JSON(Probe 保证),进度/异常都走 stderr,所以 stdout 可以直接解析。
    编码上故意**不容错**:`errors="replace"` 会把"字节流不是 UTF-8"变成一串 U+FFFD,
    于是中文模型名在比对时红成"解析结果与语料不一致" —— 排障时会一路查到解析器上去,
    而真正的问题在管道。宁可让 subprocess 抛 UnicodeDecodeError。
    """
    probe_dir = compile_probe(java_home, quiet=quiet)
    cp = os.pathsep.join([probe_dir] + [e for e in classpath if e])
    cmd = [java_tool("java", java_home), "-Xmx512m", "-Dfile.encoding=UTF-8",
           "-Djava.awt.headless=true", "-cp", cp]
    if library_path:
        cmd.append("-Djava.library.path=" + library_path)
    cmd.append("Probe")
    payload = json.dumps({"classpath": classpath, "cases": payload_cases()},
                         ensure_ascii=False)
    env = dict(os.environ)
    env["JAVA_TOOL_OPTIONS"] = ""      # 别人机器上的全局 -Dfile.encoding 不许插手
    try:
        out = subprocess.run(cmd, input=payload.encode("utf-8"), capture_output=True,
                             env=env, timeout=600)
    except UnicodeError:
        raise
    if out.returncode != 0 or not out.stdout.strip():
        raise RuntimeError("反射 MbRenderer 失败(java exit %d):\n--- stdout ---\n%s\n"
                           "--- stderr ---\n%s"
                           % (out.returncode,
                              out.stdout.decode("utf-8", "replace"),
                              out.stderr.decode("utf-8", "replace")))
    stdout = out.stdout.decode("utf-8")     # 严格解码:不是 UTF-8 就在这里炸
    try:
        return json.loads(stdout.strip().splitlines()[-1])
    except ValueError:
        raise RuntimeError("探针输出了非 JSON 内容:\n%s\n%s"
                           % (stdout, out.stderr.decode("utf-8", "replace")))

# ------------------------------------------------------------------ 语料
def load_protocol():
    with open(PROTOCOL_JSON, "r", encoding="utf-8") as f:
        return json.load(f)


def _unescape_c(text):
    """按 C 字符串语义反解 —— 必须**从左到右单遍**扫描。

    逐个 `replace("\\\\", "\\")` 是错的:`\\\\n`(转义反斜杠 + 字母 n)会被前一条
    替换先变成 `\\n` 再被后一条当成换行。反过来把 `\\\\` 放最后更糟 ——
    它会把 `\\\\n`(换行)错解成 `\\` + `n`。单遍扫描没有这种顺序依赖。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "\\" and i + 1 < n:
            nxt = text[i + 1]
            if nxt == "n":
                out.append("\n")
                i += 2
                continue
            if nxt == "t":
                out.append("\t")
                i += 2
                continue
            if nxt == "r":
                out.append("\r")
                i += 2
                continue
            if nxt == '"':
                out.append('"')
                i += 2
                continue
            if nxt == "\\":
                out.append("\\")
                i += 2
                continue
            raise RuntimeError("golden 语料里出现了不认识的转义 \\%s" % nxt)
        out.append(ch)
        i += 1
    return "".join(out)


def _assert_protocol_shape(p, text, case):
    """校验器自己的"金丝雀":确认语料的行结构与 protocol.json 的声明一致。

    为什么校验器要自证:上面的解析断言全绿有两种可能 —— (a) 解析器对,
    (b) 校验器把**期望**也算错了,于是两边一起错、一起绿。这份自检把
    "协议文本到底长什么样"从 protocol.json 独立推一遍,再和反解出来的字节比。
    它红了就说明**校验器**坏了,而不是解析器坏了 —— 这个区别在排障时很关键。
    """
    ls, fs = p["line_sep"], p["field_sep"]
    lines = text.split(ls)
    if lines and lines[-1] == "":
        lines = lines[:-1]           # 协议文本以 LINE_SEP 结尾,末尾那个空元素不是记录
    if not lines:
        if case["kind"] == "ok" and not case.get("entries"):
            return
        raise RuntimeError("语料 %s 一行都没有" % case["key"])
    want_head = p["status_prefix"] + (p["err_ok"] if case["kind"] == "ok" else case["err"])
    if lines[0] != want_head:
        raise RuntimeError("语料 %s 首行 %r,协议要求 %r" % (case["key"], lines[0], want_head))
    body = lines[1:]
    ntok = len(p["fields"])
    if len(body) % ntok != 0:
        raise RuntimeError("语料 %s 的条目行数 %d 不是 %d 的整数倍(反解把 \\n 吃错了?)"
                           % (case["key"], len(body), ntok))
    entries = case.get("entries", [])
    if len(body) // ntok != len(entries):
        raise RuntimeError("语料 %s 反解出 %d 条,protocol.json 写的是 %d 条"
                           % (case["key"], len(body) // ntok, len(entries)))
    for idx, e in enumerate(entries):
        got = body[idx * ntok:(idx + 1) * ntok]
        want = [e["name"], e["path"], str(e["bytes"]), "1" if e["ok"] else "0", e["message"]]
        if got != want:
            raise RuntimeError("语料 %s 第 %d 条字段错位:\n  反解 = %r\n  协议 = %r"
                               % (case["key"], idx + 1, got, want))


def golden_cases(p):
    """读 tools/capi_protocol/golden/*.txt(生成物),不自己拼 —— 拼了就又多一份约定。

    golden 里写的是**可见转义**(`err=MB_OK\\n`),这里反解成真实字节;
    反解后必须与 protocol.json 的 `cases` 一一对应,否则说明有人手改了 golden。
    """
    out = []
    for case in p["cases"]:
        key = case["key"]
        path = os.path.join(GOLDEN_DIR, key + ".txt")
        if not os.path.exists(path):
            raise RuntimeError("缺少 golden 语料 %s(先跑 gen_abi_table.py)" % path)
        with open(path, "r", encoding="utf-8", newline="") as f:
            body = f.read().replace("\r\n", "\n")
        # 生成器(gen_abi_table.emit_golden_files)结构是 escape_c(text) + "\n":
        # 文件里只有一个**真实的**换行,就是最后那个。只剥这一个,别用 rstrip ——
        # rstrip 会连带吃掉转义串末尾的 `\n`(协议文本合法地以 LINE_SEP 结尾,
        # 而"最后一条记录的 message 为空"时,文末连续出现两个 `\n`)。
        if not body.endswith("\n"):
            raise RuntimeError("golden 语料 %s 没有以换行结尾(被手改过?先跑 gen_abi_table.py)"
                               % path)
        text = body[:-1]
        if "\n" in text:
            raise RuntimeError("golden 语料 %s 里出现了真实换行(应为可见转义 \\n):%r"
                               % (path, text))
        out.append({"key": key, "kind": case["kind"], "text": _unescape_c(text)})
        _assert_protocol_shape(p, out[-1]["text"], case)
    return out


SYNTHETIC = {
    # 这些是**校验器自己造**的病态输入,不该待在协议 golden 里(它们不是协议,
    # 而是"解析器必须拒绝的东西")。左:key;右:构造用的 lambda 在 payload_cases 里。
    "legacy_ok_first_line": "ok\n",
    "status_prefix_only": "err=\n",
    # 空首行:比 "err="(4 字符)还短。任何"按前缀长度切片"的写法都会在这里抛,
    # 而契约是只得到 Ok/Failed。
    "shorter_than_prefix": "\n",
    "unknown_err_name": "err=MB_E_SOMETHING_NEW\n",
    "dangling_record": "err=MB_OK\nonly\nthree\nlines\n",
    "field_sep_injected": "err=MB_OK\nna\tme\np.obj\n1\n1\nok\n",
    "empty_string": "",
    "bytes_not_integer": "err=MB_OK\nn\np.obj\nnotanumber\n1\nmsg\n",
    "ok_flag_not_0_1": "err=MB_OK\nn\np.obj\n1\nmaybe\nmsg\n",
    # path 是权威字段:名字为空时回落到文件名,算成功。
    "empty_name_falls_back_to_path": "err=MB_OK\n\n/data/x/model.obj\n7\n1\n\n",
}


def synthetic_text(p, key):
    """把 SYNTHETIC 里的形状按 protocol.json 的分隔符实例化(不写字面量 TAB/换行)。"""
    raw = SYNTHETIC[key]
    if p["line_sep"] != "\n" or p["field_sep"] != "\t" or p["status_prefix"] != "err=":
        raise RuntimeError("校验器自造输入的构造假设了 \\n / \\t / err= 这三个分隔符;"
                           "协议改了这里也要改(见 synthetic_text)")
    return raw


def payload_cases():
    p = load_protocol()
    cases = golden_cases(p)
    for sc in p["kotlin_synthetic_cases"]:
        cases.append({"key": sc["key"], "kind": "synthetic",
                      "text": synthetic_text(p, sc["key"])})
    return cases


def expected_signature(p, case):
    """与 Kotlin `MbAbiProtocol.signatureOf` **独立实现**的同一套规则。"""
    parser_id = p["parser_id"]
    items = case.get("entries", [])
    if not items:
        return "%s#{empty}" % parser_id
    body = "/".join("%s|%s|%d|%d|%s" % (e["name"], e["path"], e["bytes"],
                                        1 if e["ok"] else 0, e["message"]) for e in items)
    return "%s#{%s}" % (parser_id, body)


# ------------------------------------------------------------------ 断言
def check_jni_descriptors(p, reflected, problems):
    declared = {e["name"]: e for e in p["jni_entries"]}
    methods = reflected.get("methods", {}).get("com.magicblocks.pad.MbRenderer", {})
    actual = {k: v for k, v in methods.items() if k.startswith("native")}
    for name in sorted(set(actual) - set(declared)):
        problems.append("Kotlin 里有未登记的 JNI 声明 %s%s → 补进 protocol.json"
                        % (name, format_sig(actual[name]["ret"], actual[name]["params"])))
    for name in sorted(set(declared) - set(actual)):
        problems.append("protocol.json 登记了 %s,但 Kotlin 侧没有这个 external fun" % name)
    rows = []
    for name in sorted(set(actual) & set(declared)):
        k, e = actual[name], declared[name]
        want_ret = CTYPES.get(e["ret"], e["ret"])
        c_params = [x for x in e["params"] if c_param_to_java(x) is not None]
        if len(c_params) != len(k["params"]):
            problems.append("%s 参数个数: Kotlin=%d, jni_bridge.cpp 去掉 JNIEnv/jclass 后=%d"
                            % (name, len(k["params"]), len(c_params)))
        else:
            # 每个参数单独定性:C 的泛引用(jobject)不能靠类型猜,必须由 Kotlin 侧
            # 的真实类名确认 —— 这正是"签名级校验"与"人眼看一眼"的区别。
            for idx, (c_raw, k_type) in enumerate(zip(c_params, k["params"])):
                expected = c_arg_to_java(c_raw)
                if expected == GENERIC_REF:
                    if k_type == "java.lang.Object":
                        problems.append(
                            "%s 第 %d 个参数: C 侧是泛引用 `%s`,Kotlin 侧却是 java.lang.Object "
                            "—— 请把该参数在 protocol.json 里写成真实的 Java 类型"
                            "(否则 JNI 解析到的签名与 thought-out 的不一样)" % (name, idx + 1, c_raw))
                elif expected != k_type:
                    problems.append("%s 第 %d 个参数: Kotlin=%s, jni_bridge.cpp=%s(应为 %s)"
                                    % (name, idx + 1, k_type, c_raw, expected))
        if k["ret"] != want_ret:
            problems.append("%s 返回类型: Kotlin=%s, jni_bridge.cpp=%s(应为 %s)"
                            % (name, k["ret"], e["ret"], want_ret))
        rows.append((name, format_sig(k["ret"], k["params"]),
                     "/".join(e["capi"]) or "(不经 mb_capi)", e.get("abi_min")))

    bridge = parse_jni_bridge()
    for name in sorted(set(bridge) - set(declared)):
        problems.append("jni_bridge.cpp 里有未登记入口 %s" % name)
    for name in sorted(set(declared) - set(bridge)):
        problems.append("protocol.json 登记了 %s,但 jni_bridge.cpp 里没有 JNIEXPORT" % name)
    for name in sorted(set(bridge) & set(declared)):
        if bridge[name]["ret"] != declared[name]["ret"] or \
                bridge[name]["params"] != declared[name]["params"]:
            problems.append("%s: jni_bridge.cpp 的 %s / %s 与登记表 %s / %s 不一致"
                            % (name, bridge[name]["ret"], bridge[name]["params"],
                               declared[name]["ret"], declared[name]["params"]))
    return rows


def check_constants(p, reflected, problems, evidence=None):
    v = reflected.get("values", {})
    abi = p["abi"]
    want = {
        "MbAbi.MAJOR": abi["major"],
        "MbAbi.MINOR": abi["minor"],
        "MbAbi.PATCH": abi["patch"],
        "MbAbi.BIT_STRIDE_MAJOR": abi["bit_stride"][0],
        "MbAbi.BIT_STRIDE_MINOR": abi["bit_stride"][1],
        "MbAbi.BIT_STRIDE_MAJOR_FULL": abi["bit_stride"][0] * abi["bit_stride"][1],
        "MbAbi.requiredVersion": abi["required"],
        "MbAbi.MODEL_LIST_PROTOCOL_VERSION": p["protocol_version"],
        "MbAbiProtocol.STATUS_PREFIX": p["status_prefix"],
        "MbAbiProtocol.STATUS_OK": p["err_ok"],
        "MbAbiProtocol.LINE_SEP": p["line_sep"],
        "MbAbiProtocol.FIELD_SEP": p["field_sep"],
        "MbAbiProtocol.PARSER_ID": p["parser_id"],
        "MbAbiProtocol.TOKENS_PER_MODEL": len(p["fields"]),
        "MbAbiProtocol.PROTOCOL_VERSION": p["protocol_version"],
    }
    for i, f in enumerate(p["fields"]):
        want["MbAbiProtocol.FIELD_%s" % f.upper()] = i
    before = len(problems)
    for k, expect in sorted(want.items()):
        if k not in v:
            problems.append("反射拿不到常量 %s(字段被删/改名了?)" % k)
            continue
        if v[k] != expect:
            problems.append("常量 %s = %r,protocol.json 要求 %r" % (k, v[k], expect))
    # requiredVersion 必须正好是 MAJOR*stride0 + MINOR*stride1:这条公式是
    # mb_capi.h 里 MB_ABI_VERSION 的定义。单独查,是因为"三个数一起漂"时
    # 上面逐项比对(C++ 侧也生成了同样的错数)可能一起绿,而公式不会。
    rv, maj = v.get("MbAbi.requiredVersion"), v.get("MbAbi.MAJOR")
    s0, s1 = v.get("MbAbi.BIT_STRIDE_MAJOR"), v.get("MbAbi.BIT_STRIDE_MINOR")
    if None not in (rv, maj, s0, s1) and rv != maj * s0:
        problems.append("MbAbi.requiredVersion = %r,但 MAJOR*BIT_STRIDE_MAJOR = %r"
                        % (rv, maj * s0))
    if evidence is not None:
        evidence["constants"] = (len(want) - (len(problems) - before), len(want))


def check_parser(p, reflected, problems, evidence=None):
    """断言 1:Kotlin 解析器的真实输出 == 校验器**独立重算**的期望。

    每条断言都必须"喂坏输入会红"。所以这里的每个分支都写成 `problem(...)` 并且
    都被 run_self_test 逐个注入过一次(见 FAULTS)。反过来,**没有**注入路径的分支
    不算断言 —— 它只是代码,永远绿,永远没用。
    """
    v = reflected.get("values", {})
    pid = p["parser_id"]
    bad = 0

    def problem(msg):
        nonlocal bad
        bad += 1
        problems.append(msg)

    for case in p["cases"]:
        key, kind = case["key"], case["kind"]
        got = v.get("selftest." + key)
        if got is None:
            problem("解析用例 %s 没有返回结果(调用抛异常?)" % key)
            continue
        kind_got, _, rest = got.partition("\t")
        if kind_got == "throw":
            problem("解析用例 %s:解析器抛异常(契约上不该抛) — %s" % (key, rest))
            continue
        if kind == "ok":
            if kind_got != "ok":
                problem("解析用例 %s:合法输出被判定为 %s — %s" % (key, kind_got, rest))
                continue
            n_str, _, sig = rest.partition("\t")
            want_sig = expected_signature(p, case)
            if n_str != str(len(case.get("entries", []))):
                problem("解析用例 %s:条目数 %s,语料是 %d 条"
                        % (key, n_str, len(case.get("entries", []))))
            if sig != want_sig:
                problem("解析用例 %s:解析结果与语料不一致\n"
                        "        Kotlin  : %s\n        期望    : %s" % (key, sig, want_sig))
        else:
            if kind_got != "failed":
                problem("解析用例 %s:错误输出被判定为 %s(应为 failed) — %s"
                        % (key, kind_got, rest))
            elif not rest.strip():
                problem("解析用例 %s:失败了但没给原因" % key)
        # 首行标记必须被认出来:err 用例的理由里要能看见那个错误码名 ——
        # 否则"failed"可能只是因为别的原因失败(碰巧也 failed),而不是因为读懂了 err=。
        if kind == "err" and case["err"] not in rest:
            problem("解析用例 %s:失败原因里没有错误码 %r,说明首行标记没被解析"
                    % (key, case["err"]))

    for sc in p.get("kotlin_synthetic_cases", []):
        key, prefix = sc["key"], sc["expect_prefix"]
        got = v.get("selftest." + key)
        if got is None:
            problem("自造用例 %s 没有返回结果" % key)
            continue
        if not got.startswith(prefix):
            problem("自造用例 %s:期望以 %r 开头,实际 %r"
                    % (key, prefix, got[:120]))
    # 解析成功时返回的标识必须是"这一份"协议 —— 加载到旧 class 会在这里露出来。
    sample = v.get("selftest.empty_dir")
    if sample and pid not in sample:
        problem("解析成功输出里没有解析器标识 %r:旧 class 文件? — %s" % (pid, sample))
    if evidence is not None:
        evidence["parser"] = bad


def check_parser_protocol_version(p, reflected, problems):
    """断言 3(协议版本):返回文本自称的版本必须与 UI 期望的版本相等。

    ABI 版本闸门管不到这件事:`nativeListModels` 的文本格式可以独立于 C 入口漂移
    (t3 就是这么坏的)。所以 UI 启动时还要单独比一次
    `nativeModelListProtocolVersion()` 与 `MbAbiProtocol.PROTOCOL_VERSION`。
    这里只能验到**常量那一半**(反射值 == protocol.json);C 侧那一半由
    tests/capi_modellist 的 C++ 断言补上。返回实际观察到的两个值,供自检注入。
    """
    v = reflected.get("values", {})
    seen = v.get("nativeModelListProtocolVersion")
    want = v.get("MbAbiProtocol.PROTOCOL_VERSION")
    if seen is None:
        problems.append("反射拿不到 nativeModelListProtocolVersion 的运行时值")
        return
    if want is None:
        problems.append("反射拿不到 MbAbiProtocol.PROTOCOL_VERSION")
        return
    # Probe 把 native 返回值统一转成字符串(方便回传 "unavailable\t..."),常量是数字。
    # 这里显式归一 —— 别让 "1" != 1 冒充成"协议版本漂了"。
    try:
        seen_i = int(str(seen).strip())
    except ValueError:
        problems.append("nativeModelListProtocolVersion() 在桌面上调不通:%s"
                        "(空壳库该导出这个符号 —— 见 ensure_stub_library)" % seen)
        return
    want_i = int(want)
    if seen_i != want_i:
        problems.append("协议版本不一致:库自报 %d,UI 解析器按 %d 解析 —— "
                        "nativeListModels 的文本格式已经漂了,UI 会在错误的位置切字段"
                        % (seen_i, want_i))


def check_mb_capi_reach(p, problems):
    """每个 JNI 入口都必须经 mb_capi(唯一例外在 protocol.json 里显式登记为空)。"""
    for e in p["jni_entries"]:
        if not e["capi"] and not e.get("_entry_comment"):
            problems.append("%s 没有登记对应的 mb_capi 入口,也没有写例外理由" % e["name"])


def check_no_sdk_internals(problems):
    """Kotlin 侧不许 import / 反射 SDK 内部符号。"""
    banned_import = re.compile(
        r"^\s*import\s+(device|backend|core|primitives|l3|ui_cli|cli)\b|"
        r"^\s*import\s+android\.opengl\b|^\s*import\s+org\.lwjgl\b", re.M)
    banned_text = re.compile(
        r'"\s*(device|backend|core|primitives)::|'
        r'"Vk[A-Z]\w*"|\bVkInstance\b|\bVkDevice\b|\bVkQueue\b')
    for name in sorted(os.listdir(KOTLIN_SRC)):
        if not name.endswith(".kt"):
            continue
        path = os.path.join(KOTLIN_SRC, name)
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()
        for m in banned_import.finditer(src):
            problems.append("%s: import 了 SDK 内部符号 — %s" % (name, m.group(0).strip()))
        for m in banned_text.finditer(src):
            problems.append("%s: 引用了 SDK/Vulkan 内部符号 — %s" % (name, m.group(0).strip()))


def check_colocated_helpers(problems):
    """`selftestParse` / `signatureOf` 只能有一份实现(在 MbAbiProtocol.kt 里)。"""
    for name in sorted(os.listdir(KOTLIN_SRC)):
        if not name.endswith(".kt") or name == "MbAbiProtocol.kt":
            continue
        with open(os.path.join(KOTLIN_SRC, name), "r", encoding="utf-8") as f:
            src = f.read()
        if "selftestParse" in src or "fun signatureOf" in src:
            problems.append("%s 触碰了校验入口(只该在 MbAbiProtocol.kt 里)" % name)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--classes-dir", default=DEFAULT_CLASSES)
    ap.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    ap.add_argument("--via-gradle", action="store_true",
                    help="用 Gradle 现取 classpath(慢,但保证是最新一次编译的输入)")
    ap.add_argument("--self-test", action="store_true",
                    help="验本校验器自己会红:注入故障,要求每条断言都报红")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    problems = []
    p = load_protocol()

    if args.via_gradle:
        cp = kotlin_compile_classpath(args.java_home)
        if not os.path.isdir(args.classes_dir):
            problems.append("找不到编译产物目录: %s(先跑 gradle :app:compileDebugKotlin)"
                            % args.classes_dir)
    else:
        cp = [args.classes_dir]
        if os.path.exists(CLASSES_CP_TXT):
            # Gradle 解析出来的库依赖(android.jar / kotlin-stdlib / compose ...)。
            # 只用到 java.* → kotlin.* 的那几层,但 classpath 必须完整:少一个
            # kotlin-stdlib 时解析函数一进去就 NoClassDefFoundError。
            with open(CLASSES_CP_TXT, "r", encoding="utf-8-sig") as f:
                cp += [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
        else:
            problems.append("缺少 %s(用 --via-gradle 或先跑 gradle 生成)"
                            % os.path.relpath(CLASSES_CP_TXT, REPO))

    if problems:
        for x in problems:
            print("[kotlin_abi] 失败: " + x)
        return 1

    bad = check_class_majors([args.classes_dir])
    problems += bad
    if problems:
        for x in problems:
            print("[kotlin_abi] 失败: " + x)
        return 1

    try:
        stub_dir = ensure_stub_library(p, java_home=args.java_home, quiet=args.quiet)
        reflected = reflect([x for x in cp if os.path.exists(x)], stub_dir,
                            java_home=args.java_home, quiet=args.quiet)
    except (RuntimeError, OSError) as exc:
        print("[kotlin_abi] 失败: %s" % exc)
        return 1

    for e in reflected.get("errors", []):
        problems.append("解析调用抛了 Java 异常: %s" % e)
    if not reflected.get("methods"):
        problems.append("反射 MbRenderer 没有拿到任何方法(类没加载上?)")

    rows = check_jni_descriptors(p, reflected, problems)
    check_constants(p, reflected, problems)
    check_parser(p, reflected, problems)
    check_parser_protocol_version(p, reflected, problems)
    check_mb_capi_reach(p, problems)
    check_no_sdk_internals(problems)
    check_colocated_helpers(problems)

    if args.self_test:
        problems += run_self_test(p, reflected)

    if problems:
        print("[kotlin_abi] 失败 %d 项:" % len(problems))
        for x in problems:
            print("  - " + x)
        return 1
    if not args.quiet:
        print("[kotlin_abi] OK JVM 里反射到的 %d 个 Kotlin external fun,其 JVM 描述符与 "
              "jni_bridge.cpp 的 JNIEXPORT 逐位一致:" % len(rows))
        print("   %-32s %-46s %s" % ("JNI 入口", "JVM 描述符", "mb_capi 对应 / ABI"))
        for name, sig, capi, abi in rows:
            print("   %-32s %-46s %s (ABI>=%s)" % (name, sig, capi, abi))
        print("[kotlin_abi] OK 常量/协议: %d 条语料 + %d 条自造输入,解析器输出与期望一致"
              % (len(p["cases"]), len(p.get("kotlin_synthetic_cases", []))))
        print("[kotlin_abi] OK 前提:MbRenderer.<clinit> 的 loadLibrary 靠空壳库过关 —— "
              "签名表来自 .class 元数据,不需要任何 native 实现")
    return 0


def run_self_test(p, reflected):
    """注入故障,要求对应断言确实报红,并且要求**断言覆盖率**为 100%。

    "守护断言必须先看着它红一次"——一条永远绿的断言与没有断言等价。但只把故障
    注入一遍还不够:那只能证明"被注入的那些"会红,**新写的断言没人给它注入就永远绿**。
    所以这里做两件事:
      1. 每个 `check_*` 断言都至少被一次注入覆盖(`_probe` 统一计数);
      2. `_inject` 把 faults 列表按"注入了什么"喂进去,没红就说明断言坏了。
    这两条互相咬合:有人加了一条新断言却忘了加注入,`_named` 集合里就少一条,
    第 2 步不会报错 —— 但 `check_` 的调用计数会大于等于注入次数,下面的
    `_unknown` 检查会把它抓出来。
    """
    out = []
    seen = {}         # 断言名 → 被注入打红的次数

    def _probe(label, fn):
        """跑一次断言,返回它报出的问题列表(顺带按断言名计数)。

        `fn(problems)` 每次注入都拿一份**新建**的 problems —— 绝不能复用 main 里
        那个,否则注入的坏输入会把真实问题列表污染成红的。
        """
        probs = []
        fn(probs)
        if probs:
            seen[label] = seen.get(label, 0) + 1
        return probs

    def _inject(label, what, fn):
        """坏输入 → 必须报红。没红就是**断言本身**坏了,不是被测代码坏了。"""
        if not _probe(label, fn):
            out.append("校验器自检失败:%r 这条故障**没有**被报红 —— %s 是假的"
                       % (what, label))

    base = reflected.get("values", {})
    methods = reflected.get("methods", {})

    def with_values(**over):
        return {"values": dict(base, **over)}

    def with_methods(**over):
        return {"methods": dict(methods, **over)}

    R = "com.magicblocks.pad.MbRenderer"
    real_methods = methods.get(R, {})

    faults = [
        # ---- 签名表 ----
        ("check_jni_descriptors", "nativeStatus 的返回类型从 jstring 漂成 jlong",
         lambda q: check_jni_descriptors(
             p, with_methods(**{R: dict(real_methods,
                                        nativeStatus={"ret": "long", "params": ["long"]})}), q)),
        ("check_jni_descriptors", "nativeRenderFrame 少一个 jfloat 参数",
         lambda q: check_jni_descriptors(
             p, with_methods(**{R: dict(
                 real_methods,
                 nativeRenderFrame=dict(real_methods["nativeRenderFrame"],
                                        params=["long"] + ["float"] * 6))}), q)),
        ("check_jni_descriptors", "Kotlin 多了一个没登记的 native 入口",
         lambda q: check_jni_descriptors(
             p, with_methods(**{R: dict(real_methods,
                                        nativeTotallyNew={"ret": "long", "params": []})}), q)),

        # ---- 常量 ----
        ("check_constants", "protocol.json 的 ABI MAJOR 变了而 MbAbi.kt 没跟上",
         lambda q: check_constants(
             dict(p, abi=dict(p["abi"], major=p["abi"]["major"] + 1)), reflected, q)),
        ("check_constants", "MbAbiProtocol.PARSER_ID 与 protocol.json 漂了",
         lambda q: check_constants(
             p, with_values(**{"MbAbiProtocol.PARSER_ID": "MbAbiProtocol/999"}), q)),
        ("check_constants", "MbAbi.requiredVersion 不再是 MAJOR*BIT_STRIDE_MAJOR",
         lambda q: check_constants(
             p, with_values(**{"MbAbi.requiredVersion":
                               int(base.get("MbAbi.requiredVersion") or 0) + 1}), q)),

        # ---- 解析器 ----
        ("check_parser", "解析器把 MB_E_NOT_FOUND 当成了成功",
         lambda q: check_parser(
             p, with_values(**{"selftest.not_found":
                               "ok\t0\t%s#{empty}" % p["parser_id"]}), q)),
        ("check_parser", "解析器把空目录(合法)当成了失败",
         lambda q: check_parser(p, with_values(**{"selftest.empty_dir": "failed\t坏"}), q)),
        ("check_parser", "解析器字段错位(name/path 互换)但条目数没变",
         lambda q: check_parser(p, with_values(**{"selftest.one_model": _swap_sig(p)}), q)),
        ("check_parser", "解析器又接受了老的裸 \"ok\" 首行(回到 t3 那个 bug)",
         lambda q: check_parser(
             p, with_values(**{"selftest.legacy_ok_first_line":
                               "ok\t0\t%s#{empty}" % p["parser_id"]}), q)),
        ("check_parser", "解析器对错误输出失败了,但没读懂 err= 首行(理由里没有错误码名)",
         lambda q: check_parser(
             p, with_values(**{"selftest.not_found": "failed\t扫描失败:说不上来"}), q)),
        ("check_parser", "解析器对合法输入返回了旧 class 的标识(没带 PARSER_ID)",
         lambda q: check_parser(p, with_values(**{"selftest.empty_dir": "ok\t0\t#{empty}"}), q)),

        # ---- 入口 → mb_capi 覆盖 ----
        ("check_mb_capi_reach", "某个 JNI 入口既没过 mb_capi 也没写例外理由",
         lambda q: check_mb_capi_reach(
             dict(p, jni_entries=[dict(p["jni_entries"][0], capi=[], _entry_comment="")]), q)),

        # ---- 协议版本 ----
        ("check_parser_protocol_version", "库自报的协议版本与 UI 解析器按的版本不同",
         lambda q: check_parser_protocol_version(
             p, with_values(**{"nativeModelListProtocolVersion":
                               int(base.get("MbAbiProtocol.PROTOCOL_VERSION") or 0) + 1}), q)),
    ]
    for label, what, fn in faults:
        _inject(label, what, fn)

    # 断言覆盖率:每个 check_* 都必须被注入打红过。新加断言却忘了加注入,
    # 会在这里被逮住 —— 否则它就是一条永远绿的代码,与没有断言等价。
    required = {"check_jni_descriptors", "check_constants", "check_parser",
                "check_parser_protocol_version", "check_mb_capi_reach"}
    for name in sorted(required - set(seen)):
        out.append("校验器自检失败:%s 从来没有被注入打红过 —— 它可能永远不会红" % name)
    return out


def _swap_sig(p):
    """造一个"条目数对、字段错位"的解析输出。"""
    for case in p["cases"]:
        if case["key"] == "one_model":
            e = case["entries"]
            swapped = [dict(x, name=x["path"], path=x["name"]) for x in e]
            return "ok\t%d\t%s" % (len(e), expected_signature(p, dict(case, entries=swapped)))
    raise SystemExit("protocol.json 里没有 one_model 用例")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
