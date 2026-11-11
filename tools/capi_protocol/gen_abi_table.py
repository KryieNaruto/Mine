#!/usr/bin/env python3
"""mb_capi 侧的协议/ABI 语料生成器(**唯一事实来源**)。

为什么需要这个文件
==================
t3 之后 JNI 层已经只经 `mb_capi_*` 工作,但 UI ⇄ SDK 之间还剩**一层文本协议**:
`nativeListModels` 返回的那个多行字符串。它曾经漂移过一次,而且漂移得毫无声息:

  - `jni_bridge.cpp` 把首行从 `"ok"` 改成了 `"err=MB_E_*"`;
  - `MbRenderer.kt` 仍在比 `lines[0] != "ok"` → 每次扫描**无条件**走 Failed;
  - `removePrefix("err\\t")` 去不掉 `err=` 前缀 → 界面显示字面量 `err=MB_OK`;
  - 桌面全绿:冒烟只打到 `mb_capi` 层,没有任何一层在验 Kotlin 侧解析。

这正是 t2 的 `extern "C"` 事故的同一个病根:**两侧各写一份格式约定,没有任何东西约束
它们一致**。所以修法不是"把两边都改成对的样子"(下次照样能各自漂),而是
**把协议变成一份机器可读的规约 + 生成的语料**,再让两侧都只能吃这一份:

    tools/capi_protocol/protocol.json     ← 唯一事实来源(人写)
              │
              ├─(读)─> MagicBlocks/src/android/jni_bridge.cpp        写:首行/字段/换行都从协议结构体取
              ├─(读)─> MagicBlocks/android/.../MbAbiProtocol.kt     解:同上
              └─(生成)─> tools/capi_protocol/golden/*.txt            C++ 断言它产出的字节与之一致
                        │
                        └─(引用)─> MagicBlocks/tests/capi_protocol/golden_cases.h
                                   ↑ 同一批文本还编进桌面程序,同时驱动 C++ 解析桩与
                                     真实 Kotlin 编译产物(JVM 反射调用),三方差一字节就红。

`--check` 模式做三件事(CI/agent 可直接跑):
  1. 重新生成 golden,与仓库里的逐字节比对(没有"生成完忘了提交"这种漂移);
  2. **只读地**扫描 Kotlin 源码,列出所有"看起来在解析协议"的硬编码字面量,
     与 protocol.json 的 `kotlin_literal_whitelist` 比对 —— 新出现的一律判红;
  3. 从 `jni_bridge.cpp` 的 `JNIEXPORT` 抽取 JNI 符号,与 protocol.json 的
     `jni_entries` 表比对(名字/参数表都按位比),漏一个或多一个都红。

用法:
  python tools/capi_protocol/gen_abi_table.py            # 重新生成 golden
  python tools/capi_protocol/gen_abi_table.py --check     # 校验(CI)
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PROTOCOL_JSON = os.path.join(HERE, "protocol.json")
GOLDEN_DIR = os.path.join(HERE, "golden")

JNI_BRIDGE = os.path.join(REPO, "MagicBlocks", "src", "android", "jni_bridge.cpp")
KOTLIN_DIRS = [
    os.path.join(REPO, "MagicBlocks", "android", "app", "src", "main", "java"),
    os.path.join(REPO, "MagicBlocks", "android", "app", "src", "debug", "java"),
]


# --------------------------------------------------------------------------- IO
def load_protocol():
    with open(PROTOCOL_JSON, "r", encoding="utf-8") as f:
        return json.load(f)


def write_text(path, text, check, problems):
    """写文件;`check` 为真时只比对,不一致就记一条问题。"""
    norm = text.replace("\r\n", "\n")
    if check:
        if not os.path.exists(path):
            problems.append("缺少生成产物: %s" % rel(path))
            return
        with open(path, "r", encoding="utf-8", newline="") as f:
            have = f.read().replace("\r\n", "\n")
        if have != norm:
            problems.append("生成产物与协议不一致(未重新生成?): %s" % rel(path))
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # newline="\n" 固定行尾:golden 是被逐字节断言的,不能随机器变 CRLF/LF。
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(norm)


def rel(path):
    return os.path.relpath(path, REPO).replace(os.sep, "/")


# ------------------------------------------------------------------- 语料构造
def build_cases(p):
    """把 protocol.json 里的情形实例化成具体文本。

    每个情形给出**完整的多行文本**;`\t` / `\n` 由 Python 转义写进 golden 文件,
    所以 golden 里看到的是 `name\tpath` 这种可见的转义,而不是真制表符
    —— 后者在 diff 里完全看不出来,正是 t3 那个 bug 藏身的地方。
    """
    sep = p["field_sep"]
    eol = p["line_sep"]
    out = []
    for case in p["cases"]:
        lines = []
        if case["kind"] == "ok":
            lines.append(p["status_prefix"] + p["err_ok"])
            for e in case.get("entries", []):
                lines += [e["name"], e["path"], str(e["bytes"]),
                          "1" if e["ok"] else "0", e["message"]]
        elif case["kind"] == "err":
            lines.append(p["status_prefix"] + case["err"])
        else:
            raise SystemExit("protocol.json: 未知 kind %r" % case["kind"])
        text = eol.join(lines) + eol          # 末尾一定带一个换行(最后一条记录也成行)
        out.append({"key": case["key"], "kind": case["kind"], "text": text})
    return out


def escape_c(text):
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace(
        '"', '\\"')


def emit_golden_files(p, cases, check, problems):
    for c in cases:
        # 每个情形一份文件:内容是**可见转义**,逐字节可比。
        body = escape_c(c["text"]) + "\n"
        write_text(os.path.join(GOLDEN_DIR, c["key"] + ".txt"), body, check, problems)


def emit_c_header(p, cases, check, problems):
    n = p["newline_sanitized"]
    lines = []
    lines.append("// 由 tools/capi_protocol/gen_abi_table.py 从 tools/capi_protocol/protocol.json 生成。")
    lines.append("// **不要手改** —— 改协议就改 protocol.json 再跑生成器,否则 --check 会红。")
    lines.append("//")
    lines.append("// 这份头文件是 C++ 与 JVM **共用的同一批字节**:")
    lines.append("//   * C++ 侧(mb_capi_protocol_wire.cpp)拿它断言真实产出一致;")
    lines.append("//   * 桌面程序(tests/capi_modellist/)再把同一批字节喂给**真实 Kotlin 编译产物**,")
    lines.append("//     用 JVM 反射调用 MbRenderer.kt 里的解析函数。")
    lines.append("// 两侧吃的是同一批文本,所以协议漂移不可能只漂一边。")
    lines.append("#ifndef MAGICBLOCKS_TESTS_CAPI_PROTOCOL_GOLDEN_CASES_H")
    lines.append("#define MAGICBLOCKS_TESTS_CAPI_PROTOCOL_GOLDEN_CASES_H")
    lines.append("")
    lines.append("namespace mb_protocol_golden {")
    lines.append("")
    lines.append("// ---- 协议常量(与 protocol.json / MbAbiProtocol.kt 同源) ----")
    lines.append('#define MB_PROTO_FIELD_SEP "%s"' % escape_c(p["field_sep"]))
    lines.append("#define MB_PROTO_FIELD_SEP_CH '%s'" % escape_c(p["field_sep"]))
    lines.append('#define MB_PROTO_LINE_SEP_STR "%s"' % escape_c(p["line_sep"]))
    lines.append("#define MB_PROTO_LINE_SEP_CH '%s'" % escape_c(p["line_sep"]))
    lines.append('#define MB_PROTO_STATUS_PREFIX "%s"' % escape_c(p["status_prefix"]))
    lines.append('#define MB_PROTO_ERR_OK "%s"' % escape_c(p["err_ok"]))
    lines.append("#define MB_PROTO_ERR_PREFIX_LEN %d" % len(p["status_prefix"]))
    lines.append("#define MB_PROTO_TOKEN_COUNT %d" % len(p["fields"]))
    lines.append("#define MB_PROTO_PROTOCOL_VERSION %d" % int(p["protocol_version"]))
    lines.append("")
    lines.append("// 字段里被替换成空格的那几个字符(见 protocol.json 的 newline_sanitized)。")
    lines.append("// 逐个字符给一条常量,而不是一整串:后者在生成物里换行后会变成非法字面量。")
    for i, ch in enumerate(n["chars"]):
        lines.append("#define MB_PROTO_SANITIZE_CHAR_%d '\\x%02X'  // %s" % (i, ord(ch), escape_c(ch)))
    lines.append("#define MB_PROTO_SANITIZE_CHAR_COUNT %d" % len(n["chars"]))
    lines.append("#define MB_PROTO_SANITIZE_CHAR '%s'" % escape_c(n["char"]))
    lines.append("")
    lines.append("// ---- 字段顺序(索引即协议位置) ----")
    for i, f in enumerate(p["fields"]):
        lines.append("#define MB_PROTO_FIELD_%d_%s %d" % (i, f.upper(), i))
    lines.append("")
    lines.append("struct case_t {")
    lines.append("  const char* key;")
    lines.append("  int kind;  // 0 = ok, 1 = err")
    lines.append("  const char* text;  // 完整返回文本(含末尾换行)")
    lines.append("};")
    lines.append("")
    lines.append("constexpr case_t kCases[] = {")
    for c in cases:
        kind = 0 if c["kind"] == "ok" else 1
        lines.append('    {"%s", %d, "%s"},' % (c["key"], kind, escape_c(c["text"])))
    lines.append("};")
    lines.append("")
    lines.append("constexpr int kCaseCount = %d;" % len(cases))
    lines.append("")
    lines.append("}  // namespace mb_protocol_golden")
    lines.append("")
    lines.append("#endif  // MAGICBLOCKS_TESTS_CAPI_PROTOCOL_GOLDEN_CASES_H")
    lines.append("")
    write_text(os.path.join(REPO, "MagicBlocks", "tests", "capi_protocol", "golden_cases.h"),
               "\n".join(lines), check, problems)


# --------------------------------------------------- Kotlin 侧字面量硬编码扫描
def iter_kotlin_files():
    for base in KOTLIN_DIRS:
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if d not in {"build", ".git"})
            for name in sorted(filenames):
                if name.endswith(".kt"):
                    yield os.path.join(dirpath, name)


KOTLIN_STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def kotlin_string_literals(path):
    """去掉行注释/块注释后,逐个抽出字符串字面量,返回 [(lineno, 文本)]。

    只做"给人看的初步筛查":它不可能是完整的 Kotlin 词法器,所以规则定得很窄 ——
    只找**看起来像协议标记**的字面量,而不是收集所有字符串(那样噪声淹没信号,
    规则会被迫放宽到没意义)。
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            src = f.read()
    except OSError:
        return []
    # 去掉块注释(保留行数,报行号还要用)
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    out = []
    for lineno, line in enumerate(src.splitlines(), 1):
        code = line.split("//", 1)[0]
        for m in KOTLIN_STRING.finditer(code):
            out.append((lineno, m.group(1)))
    return out


def check_kotlin_literals(p, problems):
    """把 Kotlin 侧的协议字面量钉成封闭集合。

    判据:一个字符串字面量若**含有** `field_sep` / `line_sep` / `status_prefix`,
    或**等于** `err_ok` / `sanitize_chars` / 空串之外的某个错误码名,就认为是
    "在手动拼协议"。允许出现的必须在 protocol.json 的 `kotlin_literal_whitelist` 里,
    且写明理由 —— 新出现一个就红,逼下一个人去改 protocol.json 或改用 MbAbiProtocol。
    """
    markers = [p["field_sep"], p["line_sep"], p["status_prefix"]]
    err_names = set(p.get("err_names", []))
    allowed = p.get("kotlin_literal_whitelist", {})
    found = {}
    for path in iter_kotlin_files():
        for lineno, lit in kotlin_string_literals(path):
            hit = any(m and m in lit for m in markers) or lit in err_names
            if not hit:
                continue
            found.setdefault(lit, []).append("%s:%d" % (rel(path), lineno))
    for lit, spots in sorted(found.items()):
        if lit not in allowed:
            problems.append(
                "Kotlin 侧出现未登记的协议字面量 %r: %s\n"
                "        → 要么改用 MbAbiProtocol 里的常量,要么写进 "
                "tools/capi_protocol/protocol.json 的 kotlin_literal_whitelist 并说明理由"
                % (lit, ", ".join(spots)))
    for lit in sorted(allowed):
        if lit.startswith("_") or lit in ("$comment", "comment"):
            continue          # protocol.json 里的说明字段,不是豁免
        if lit not in found:
            problems.append(
                "kotlin_literal_whitelist 里的 %r 在 Kotlin 源码里已不存在 → 请从 "
                "protocol.json 删掉这条豁免(留着就是一条永远不会红的死规则)" % lit)


# ------------------------------------------------------ JNI 符号抽取 + 比对
JNIEXPORT_RE = re.compile(
    r"JNIEXPORT\s+(\w+)\s+JNICALL\s+Java_com_magicblocks_pad_MbRenderer_(\w+)\s*\(([^)]*)\)",
    re.S)


def parse_jni_entries(path):
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
            t = re.sub(r"\s*\*+\s*", "*", t)        # `JNIEnv * env` -> `JNIEnv*env`
            t = re.sub(r"\s+", " ", t)
            types.append(t)
        out[name] = {"ret": ret, "params": types}
    return out


def check_jni_entries(p, problems):
    """**名字与参数表按位比对**。

    这是 t4 里"桌面能跑起来的签名校验"的一半:名字/参数一旦漂移,真机上是
    运行期 UnsatisfiedLinkError,桌面上完全测不出来 —— 所以在这里提前钉死。
    另一半(Kotlin `external fun` 的真实 JVM 描述符)由
    tests/capi_modellist 的 JVM 反射路径提供,那条路径吃的是编译器自己生成的
    @Metadata,不是这里的文本。
    """
    actual = parse_jni_entries(JNI_BRIDGE)
    declared = {e["name"]: e for e in p["jni_entries"]}
    for name in sorted(set(actual) - set(declared)):
        problems.append("jni_bridge.cpp 里有未登记的 JNI 入口 %s → 请补进 protocol.json" % name)
    for name in sorted(set(declared) - set(actual)):
        problems.append("protocol.json 登记了 %s,但 jni_bridge.cpp 里没有这个 JNIEXPORT" % name)
    for name in sorted(set(actual) & set(declared)):
        a, d = actual[name], declared[name]
        if a["ret"] != d["ret"]:
            problems.append("JNI 入口 %s 返回类型: jni_bridge.cpp=%s, protocol.json=%s"
                            % (name, a["ret"], d["ret"]))
        if a["params"] != d["params"]:
            problems.append("JNI 入口 %s 参数表不一致:\n        jni_bridge.cpp = %s\n"
                            "        protocol.json  = %s" % (name, a["params"], d["params"]))


# ------------------------------------------------------------------ native 侧
def check_kotlin_abi_mirror(p, problems):
    """protocol.json 的 `abi` 段必须逐条出现在 MbAbi.kt 里。

    这不是重复校验:protocol.json 里那份是给"跨语言比对"用的声明,MbAbi.kt 里那份
    才是真被编译的代码。两者必须一模一样,多写/少写/写错版本都红。
    """
    abi = p.get("abi")
    if not abi:
        return
    path = os.path.join(REPO, "MagicBlocks", "android", "app", "src", "main", "java",
                        "com", "magicblocks", "pad", "MbAbi.kt")
    if not os.path.exists(path):
        problems.append("缺少 Kotlin 侧 ABI 镜像: %s" % rel(path))
        return
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    checks = [
        ("MAJOR", r"const val MAJOR\s*=\s*(\d+)", str(abi["major"])),
        ("MINOR", r"const val MINOR\s*=\s*(\d+)", str(abi["minor"])),
        ("PATCH", r"const val PATCH\s*=\s*(\d+)", str(abi["patch"])),
        ("BIT_STRIDE_MAJOR", r"const val BIT_STRIDE_MAJOR\s*=\s*(\d+)", str(abi["bit_stride"][0])),
        ("BIT_STRIDE_MINOR", r"const val BIT_STRIDE_MINOR\s*=\s*(\d+)", str(abi["bit_stride"][1])),
        # requiredVersion 必须是**字面量**(Kotlin 的 const val 不接受引用另一个 const val,
        # 编译器会报 CONST_VAL_WITH_NON_CONST_INITIALIZER)。所以这里钉的是字面量的值,
        # 并额外把"公式"钉一遍(见下面 pack() 那段) —— 两处都红才算真钉住。
        ("requiredVersion", r"const val requiredVersion: Int = (\d+)", str(abi["required"])),
    ]
    for label, pattern, expect in checks:
        m = re.search(pattern, src)
        if not m:
            problems.append("MbAbi.kt 里找不到 %s 的声明(协议要求它写成常量)" % label)
            continue
        if expect is not None and m.group(1) != expect:
            problems.append("MbAbi.kt 的 %s=%s,protocol.json 写的是 %s"
                            % (label, m.group(1), expect))
    # pack() 的公式本身也要是协议写的那个(否则常量都在、公式被改了同样出事)。
    # 接受两种写法:直接用字面量 stride,或用本文件里已被钉死的 BIT_STRIDE_* 常量
    # ——后者更强(常量在上面已经跟 protocol.json 对齐了)。
    bit = abi["bit_stride"]
    m = re.search(r"fun pack\(major: Int, minor: Int, patch: Int\): Int\s*=\s*(.+?)\n",
                  src, re.S)
    ident = re.escape(abi["stride_idents"][0]) if abi.get("stride_idents") else "NO_IDENT_CONFIGURED"
    if not m:
        problems.append("MbAbi.kt 里找不到 pack() 的实现")
    else:
        # 归一化:去掉空白,让多行续写与单行完全等价(否则"写成两行"会被误判成改了公式)。
        expr = re.sub(r"\s+", " ", m.group(1)).strip()
        ok_major = ident in expr or ("%d" % bit[0]) in expr
        ok_minor = ident.replace("MAJOR", "MINOR") in expr or ("%d" % bit[1]) in expr
        if not (ok_major and ok_minor) or "patch" not in expr:
            problems.append("MbAbi.kt 的 pack() 公式不是 MAJOR*%d + MINOR*%d + PATCH: %s"
                            % (bit[0], bit[1], expr))
    # majorOf / minorOf 必须真用到位宽常量,而不是另写一个魔数。
    for fn, need in (("majorOf", "BIT_STRIDE_MAJOR"), ("minorOf", "BIT_STRIDE_MINOR")):
        m = re.search(r"fun %s\(packed: Int\): Int\s*=\s*([^\n]+)" % fn, src)
        if not m:
            problems.append("MbAbi.kt 里找不到 %s()" % fn)
        elif need not in m.group(1):
            problems.append("MbAbi.kt 的 %s() 没有用 %s: %s" % (fn, need, m.group(1).strip()))
    # requiredVersion 的字面量必须**等于** pack(MAJOR, MINOR, PATCH=0) 的算术结果。
    want = abi["major"] * bit[0] + abi["minor"] * bit[1]
    if str(want) != str(abi["required"]):
        problems.append("protocol.json 自相矛盾: major*stride0 + minor*stride1 = %d, "
                        "但 required 写的是 %d" % (want, abi["required"]))
    m = re.search(r"const val requiredVersion: Int = (\d+)", src)
    if m and m.group(1) != str(want):
        problems.append("MbAbi.requiredVersion=%s,而 pack(%d,%d,0)=%d —— 打包公式与字面量对不上"
                        % (m.group(1), abi["major"], abi["minor"], want))


def check_kotlin_protocol_mirror(p, problems):
    """`MbAbiProtocol.kt` 里的协议常量必须与 protocol.json 逐条一致。

    为什么单独一个:这些常量是**解析侧**的唯一事实,而协议文本的来源在 C++ 侧。
    两侧各写一份常量就是 t3 那次 bug 的根:改了一边另一边没动,而界面上只表现为
    "模型列表永远是空的"。这里把它们逐条钉住,任何一边漂了 --check 就红。
    """
    path = os.path.join(REPO, "MagicBlocks", "android", "app", "src", "main", "java",
                        "com", "magicblocks", "pad", "MbAbiProtocol.kt")
    if not os.path.exists(path):
        problems.append("缺少 Kotlin 侧协议镜像: %s" % rel(path))
        return
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    def kotlin_esc(s):
        """Kotlin 源码里这个字符串字面量该长什么样(可见转义)。"""
        return (s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
                .replace("\r", "\\r").replace('"', '\\"'))

    def check_str(label, pattern, expect):
        m = re.search(pattern, src)
        if not m:
            problems.append("MbAbiProtocol.kt 里找不到 %s 的常量声明" % label)
            return
        if m.group(1) != kotlin_esc(expect):
            problems.append("MbAbiProtocol.%s = %r,protocol.json 写的是 %r"
                            % (label, m.group(1), kotlin_esc(expect)))

    def check_int(label, pattern, expect):
        m = re.search(pattern, src)
        if not m:
            problems.append("MbAbiProtocol.kt 里找不到 %s 的常量声明(协议要求它写成常量)" % label)
            return
        if str(m.group(1)) != str(expect):
            problems.append("MbAbiProtocol.%s = %s,protocol.json 写的是 %s"
                            % (label, m.group(1), expect))

    check_str("STATUS_PREFIX", r'const val STATUS_PREFIX\s*=\s*"((?:[^"\\]|\\.)*)"',
              p["status_prefix"])
    check_str("STATUS_OK", r'const val STATUS_OK\s*=\s*"((?:[^"\\]|\\.)*)"', p["err_ok"])
    check_str("LINE_SEP", r'const val LINE_SEP\s*=\s*"((?:[^"\\]|\\.)*)"', p["line_sep"])
    check_str("FIELD_SEP", r'const val FIELD_SEP\s*=\s*"((?:[^"\\]|\\.)*)"', p["field_sep"])
    check_str("PARSER_ID", r'const val PARSER_ID\s*=\s*"((?:[^"\\]|\\.)*)"', p["parser_id"])
    check_int("TOKENS_PER_MODEL", r"const val TOKENS_PER_MODEL\s*=\s*(\d+)",
              len(p["fields"]))
    check_int("PROTOCOL_VERSION", r"const val PROTOCOL_VERSION\s*=\s*(\d+)",
              p["protocol_version"])
    for i, f in enumerate(p["fields"]):
        check_int("FIELD_%s" % f.upper(),
                  r"const val FIELD_%s\s*=\s*(\d+)" % f.upper(), i)
    # PARSER_ID 的模板必须真的由 protocol_version 算出来 —— 否则 bump 了
    # protocol_version 而 PARSER_ID 还是手写的旧串,两条断言会一起绿(都等于旧值)。
    tpl = p.get("_parser_id_tpl", "")
    want_pid = tpl % p["protocol_version"] if tpl else p["parser_id"]
    if p["parser_id"] != want_pid:
        problems.append("protocol.json 自相矛盾: _parser_id_tpl %r 按 protocol_version=%d 展开是 "
                        "%r,但 parser_id 写的是 %r"
                        % (tpl, p["protocol_version"], want_pid, p["parser_id"]))


def check_cpp_protocol_mirror(p, problems):
    """`mb_capi.h` 里的 `MB_PROTOCOL_*` 常量必须与 protocol.json 逐条一致。

    为什么必须有这一条(2026-11-11 captain 追问,实测确认是真洞):
      `jni_bridge.cpp` 拼字符串时用的是 `MB_PROTOCOL_STATUS_PREFIX` 这类宏,**不是**
      字面量 —— 这一点是对的(否则就退化成"两侧各写一份")。但这些宏定义在
      `mb_capi.h` 里,而**有没有人校验它们等于 protocol.json,是另一件事**。

      实测:把 `mb_capi.h` 的 `#define MB_PROTOCOL_STATUS_PREFIX "err="` 手改成
      `"ERR:"`,`gen_abi_table.py --check` **全绿** —— 即 C++ 侧的真实现可以单方面
      漂走而无人察觉。这正是 t3 那个 bug 的形状:"检查器描述的是一份格式,被检的
      是另一份代码"。现在补上,并且用 `--check` 之外的独立注入证明它真能红。

    只认代码行:注释里的 `MB_PROTOCOL_*`(说明文字)不算。
    """
    path = os.path.join(REPO, "MagicBlocks", "include", "magicblocks", "mb_capi.h")
    if not os.path.exists(path):
        problems.append("缺少 C++ 侧协议镜像: %s" % rel(path))
        return
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()

    def c_esc(s):
        """C 源码里这个字符串该长什么样(可见转义)。"""
        return (s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
                .replace("\r", "\\r").replace('"', '\\"'))

    def find_str(label):
        m = re.search(r'^#\s*define\s+%s\s+"((?:[^"\\]|\\.)*)"' % label, raw, re.M)
        if not m:
            problems.append("mb_capi.h 里找不到 #define %s(协议要求它写成宏)" % label)
            return None
        return m.group(1)

    def find_chr(label):
        m = re.search(r"^#\s*define\s+%s\s+'(\\?.)'" % label, raw, re.M)
        if not m:
            problems.append("mb_capi.h 里找不到 #define %s(字符常量)" % label)
            return None
        return m.group(1)

    def find_int(label):
        m = re.search(r"^#\s*define\s+%s\s+(\d+)" % label, raw, re.M)
        if not m:
            problems.append("mb_capi.h 里找不到 #define %s(协议要求它写成宏)" % label)
            return None
        return m.group(1)

    def want_str(label, expect):
        got = find_str(label)
        if got is not None and got != c_esc(expect):
            problems.append("mb_capi.h 的 %s = \"%s\",protocol.json 写的是 \"%s\""
                            % (label, got, c_esc(expect)))

    def want_chr(label, expect):
        got = find_chr(label)
        if got is None:
            return
        # 解码可见转义后比对,避免 '\t' 与真 TAB 的写法差异被误报。
        dec = got.replace("\\\\", "\x00").replace("\\t", "\t").replace("\\r", "\r")
        dec = dec.replace("\\n", "\n").replace("\x00", "\\")
        if dec != expect:
            problems.append("mb_capi.h 的 %s = %r,protocol.json 写的是 %r"
                            % (label, dec, expect))

    want_str("MB_PROTOCOL_STATUS_PREFIX", p["status_prefix"])
    want_str("MB_PROTOCOL_ERR_OK", p["err_ok"])
    want_str("MB_PROTOCOL_LINE_SEP_STR", p["line_sep"])
    want_chr("MB_PROTOCOL_FIELD_SEP", p["field_sep"])
    want_str("MB_PROTOCOL_SANITIZE_CHARS", p["newline_sanitized"]["chars"])
    want_chr("MB_PROTOCOL_SANITIZE_CHAR", p["newline_sanitized"]["char"])

    got = find_int("MB_PROTOCOL_TOKENS_PER_MODEL")
    if got is not None and int(got) != len(p["fields"]):
        problems.append("mb_capi.h 的 MB_PROTOCOL_TOKENS_PER_MODEL = %s,protocol.json 有 %d 个字段"
                        % (got, len(p["fields"])))

    # 字段顺序枚举也必须逐项对上:它决定 jni_bridge.cpp 里 append 的顺序。
    for i, f in enumerate(p["fields"]):
        name = "MB_PROTOCOL_FIELD_%s" % f.upper()
        m = re.search(r"\b%s\s*=\s*(\d+)" % name, raw)
        if not m:
            problems.append("mb_capi.h 的 mb_protocol_field 里找不到 %s" % name)
        elif int(m.group(1)) != i:
            problems.append("mb_capi.h 的 %s = %s,protocol.json 里它排第 %d"
                            % (name, m.group(1), i))


# ------------------------------------------------------------------------ main
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="只校验:重新生成后与仓库比对 + Kotlin 字面量 + JNI 符号表")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    p = load_protocol()
    problems = []
    cases = build_cases(p)

    emit_golden_files(p, cases, args.check, problems)
    emit_c_header(p, cases, args.check, problems)
    check_kotlin_literals(p, problems)
    check_jni_entries(p, problems)
    check_kotlin_abi_mirror(p, problems)
    check_kotlin_protocol_mirror(p, problems)
    check_cpp_protocol_mirror(p, problems)

    if problems:
        print("[capi_protocol] 失败 %d 项:" % len(problems))
        for x in problems:
            print("  - " + x)
        return 1

    if not args.quiet:
        if args.check:
            print("[capi_protocol] OK 语料 %d 条与协议一致;Kotlin 协议字面量在册;"
                  "JNI 入口 %d 个与 jni_bridge.cpp 逐位一致;"
                  "MbAbi + MbAbiProtocol 与协议镜像一致"
                  % (len(cases), len(p["jni_entries"])))
        else:
            print("[capi_protocol] 已生成 %d 份 golden + tests/capi_protocol/golden_cases.h"
                  % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
