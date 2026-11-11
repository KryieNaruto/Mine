#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""t6 门控的自测:证明 `check_ui_isolation.py` 的门**真的会红**。

为什么必须有这个文件
-----------------------------------------------------------------------------
t8/G3 那个洞活了很久,原因只有一个:那条规则**永远匹配不上**,于是门控永远绿。
一个永远绿的门控与没有门控在 CI 里长得一模一样。所以这里对每一条门控都要求
"给出一个坏样本 → 必须红","给出一个好样本 → 必须绿"。

覆盖:
  N1 导出面:混进内部头 → GATE_UI_EXPORT 必须红;只有 mb_capi.h → 绿
  N2 负向 include:受限包含面下 include 内部头 → 必须红(C1083)
  N3 依赖扫描:注入违规 TU → 必须红;干净 TU → 必须绿
  N4 JNI 契约:Kotlin 少声明 / 多声明 / 参数类型改一位 / 回调名改掉 → 必须红
  N5 CMake 导出面:PUBLIC 给整棵 include/ → 必须红;给子目录 → 绿
  N6 消费者:缺 extern "C" → 链接期必须红(LNK2019/LNK2001)
  N7 版本断言:消费者写错版本 → 运行期必须红
  N8 符号表:26 个 MB_API 必须裸名存在(依赖真实 DLL;无 DLL 时如实 SKIP)

⚠ 需要 MSVC(编译类刺探)与一个 DLL(链接/运行类刺探)。缺任一项那几条如实 SKIP
  并打印原因,绝不悄悄算绿。

用法:
  python MagicBlocks/tools/tests/test_check_ui_isolation.py
  python MagicBlocks/tools/tests/test_check_ui_isolation.py -v
  python MagicBlocks/tools/tests/test_check_ui_isolation.py --dll <magicblocks.lib|dll>
"""

import os
import re
import shutil
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
MB = os.path.dirname(TOOLS)
REPO = os.path.dirname(MB)
sys.path.insert(0, TOOLS)

import check_ui_isolation as C  # noqa: E402

VENT = os.path.join(MB, "build-uisplit")
VENV = os.path.join(VENT, "ui_isolation")
REAL_INCLUDE = os.path.join(MB, "include")

DLL_ARG = None
for i, a in enumerate(sys.argv):
    if a == "--dll" and i + 1 < len(sys.argv):
        DLL_ARG = sys.argv[i + 1]
        del sys.argv[i:i + 2]
        break


def make_env(export_include=None, lib=None, work=None, extra_args=None):
    class A(object):
        pass

    a = A()
    a.magicblocks = None
    a.build_dir = VENT
    a.export_include = export_include
    a.lib = lib
    a.ninja = None
    a.work_dir = work or os.path.join(VENV, "selftest_work")
    a.vcvars = None
    a.cl = None
    a.ndk_nm = None
    a.kotlin_path = None
    a.jni_bridge_path = None
    a.scripts_dir = None
    a.neg_headers = None
    a.mode = "all"
    for k, v in (extra_args or {}).items():
        setattr(a, k, v)
    env = C.Env(a)
    os.makedirs(env.work, exist_ok=True)
    return env


def find_lib():
    if DLL_ARG:
        return os.path.abspath(DLL_ARG)
    for cand in (os.path.join(VENV, "stub_dll", "magicblocks.lib"),
                 os.path.join(VENT, "magicblocks.lib")):
        if os.path.isfile(cand):
            return cand
    return None


def reset_gates():
    C.GATES.clear()
    C.ENV_RESULTS.clear()
    C.WARNINGS.clear()
    C.FAILURES.clear()


def copy_header_to_export(dst, extra=()):
    """建一个只有 mb_capi.h 的导出面;extra 是额外的 (相对路径, 内容) 列表。"""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(os.path.join(dst, "magicblocks"))
    shutil.copy2(os.path.join(REAL_INCLUDE, "magicblocks", "mb_capi.h"),
                 os.path.join(dst, "magicblocks", "mb_capi.h"))
    for rel, content in extra:
        p = os.path.join(dst, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(content)
    return dst


class N1ExportSurface(unittest.TestCase):
    """导出面:多一个内部头就必须红。"""

    def setUp(self):
        reset_gates()

    def test_clean_surface_is_green(self):
        inc = copy_header_to_export(os.path.join(VENV, "selftest_export_clean"))
        env = make_env(export_include=inc)
        self.assertTrue(C.check_export_surface(env))
        self.assertEqual(len(C.GATES), 1)
        self.assertTrue(C.GATES[0][1])

    def test_leaked_internal_header_is_red(self):
        inc = copy_header_to_export(
            os.path.join(VENV, "selftest_export_leak"),
            extra=[("magicblocks/scene/scene.h", "// pretend internal header\n")])
        env = make_env(export_include=inc)
        self.assertFalse(C.check_export_surface(env))
        self.assertTrue(any(not ok for _n, ok, _d in C.GATES))
        msg = " ".join(d for _n, ok, d in C.GATES if not ok)
        self.assertIn("scene/scene.h", msg)

    def test_bare_top_level_header_is_red(self):
        """裸露在导出面根目录的头也不许(demo 的 public/ 那种形状)。"""
        inc = copy_header_to_export(
            os.path.join(VENV, "selftest_export_bare"),
            extra=[("mb_extra.h", "// bare\n")])
        env = make_env(export_include=inc)
        self.assertFalse(C.check_export_surface(env))


class N2NegativeInclude(unittest.TestCase):
    """受限 include:只有导出面时,内部头必须编不过。"""

    def setUp(self):
        reset_gates()
        self.inc = copy_header_to_export(os.path.join(VENV, "selftest_neg_inc"))
        self.env = make_env(export_include=self.inc,
                            extra_args={"neg_headers": "scene/scene.h,ui/ui_app.h"})

    def test_internal_headers_are_unreachable(self):
        ok = C.check_negative_include(self.env)
        self.assertTrue(ok, "内部头在只有导出面的环境下居然编过了: %r" % (C.FAILURES,))
        self.assertTrue(any(n == "GATE_NEG_INCLUDE" and o for n, o, _d in C.GATES))

    def test_same_probe_compiles_against_real_tree(self):
        """元测试的反面:同样的探针指向真实内部头必须**能找得到**。

        否则"编不过"可能只是探针本身写错了 —— 那样的红什么都证明不了。

        ⚠ t5 之后内部头从 include/magicblocks/<sub>/ 物理搬到
          include/mb_internal/magicblocks/<sub>/。这里必须按**新位置**取证,
          否则探针无论怎么改都"找不到头",元测试会永远红(或者更糟:被人
          删掉,于是"探针写法正确"再也没人验)。
        """
        internal_root = os.path.join(REAL_INCLUDE, "mb_internal", "magicblocks")
        hdr = os.path.join(internal_root, "scene", "scene.h")
        if not os.path.isfile(hdr):
            self.skipTest("include/mb_internal/magicblocks/scene/scene.h 不存在(t5 布局又变了)")
        src = C.NEG_INCLUDE_PROBE % {"internal": "mb_internal/magicblocks/scene/scene.h"}
        d = os.path.join(self.env.work, "real_inc_probe")
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
        p = os.path.join(d, "neg.cpp")
        with open(p, "w", encoding="utf-8") as f:
            f.write(src)
        vc = self.env.msvc_env()
        rc, log = C._run([self.env.cl_path(), "/nologo", "/std:c++20", "/EHsc", "/c", p,
                          "/Fo:" + os.path.join(d, "neg.obj"), "/I" + REAL_INCLUDE], vc, cwd=d)
        # 只要求"不再是找不到头":内部头自身可能因为缺别的 -I 而报错,那与本自证无关。
        self.assertNotRegex(
            log, r"cannot open include file|C1083.*(?:cannot open|包括文件)",
            "真实内部头路径下仍然报'找不到头',说明探针名字写错了:\n" + log)


class N3DependencyScan(unittest.TestCase):
    """依赖扫描:注入违规 TU 必须红;干净 TU 必须绿。

    注意:CMake 侧的检查是**另一道**门(N5)。本组只验"TU 层面扫得对不对",
    所以用 CMakeAllGreen 把 CMake 那份判定换成空的,避免真实 CMakeLists 的状态
    (今天还在 `PUBLIC include`,t5 处理)污染 TU 层面的结论。
    """

    def setUp(self):
        reset_gates()
        self.env = make_env()
        self._orig = C.check_cmake_public_includes
        C.check_cmake_public_includes = lambda _env: []

    def tearDown(self):
        C.check_cmake_public_includes = self._orig

    def _tu(self, name, body):
        p = os.path.join(self.env.work, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write(body)
        return os.path.relpath(p, self.env.mb).replace(os.sep, "/")

    def test_clean_tu_is_green(self):
        rel = self._tu("clean.cpp", '#include "magicblocks/mb_capi.h"\nint x;\n')
        self.assertTrue(C.scan_ui_dependency_graph(self.env, tus=[rel]))

    def test_internal_include_is_red(self):
        rel = self._tu("viol.cpp", '#include "magicblocks/scene/scene.h"\nint x;\n')
        self.assertFalse(C.scan_ui_dependency_graph(self.env, tus=[rel]))
        self.assertTrue(any("scene/scene.h" in f for f in C.FAILURES))

    def test_relative_internal_include_is_red(self):
        rel = self._tu("viol2.cpp", '#include "core/device_ops.h"\nint x;\n')
        self.assertFalse(C.scan_ui_dependency_graph(self.env, tus=[rel]))

    def test_unregistered_header_is_red(self):
        """"没登记的头"也算红 —— 否则换个名字就能绕过白名单。"""
        rel = self._tu("viol3.cpp", '#include "mysneaky.h"\nint x;\n')
        self.assertFalse(C.scan_ui_dependency_graph(self.env, tus=[rel]))

    def test_platform_headers_are_green(self):
        """NDK 平台头(jni.h / ANativeWindow)是合法的,不能误报。"""
        rel = self._tu("plat.cpp", '#include <jni.h>\n#include <android/native_window.h>\n'
                                   '#include "magicblocks/mb_capi.h"\nint x;\n')
        self.assertTrue(C.scan_ui_dependency_graph(self.env, tus=[rel]),
                        "平台头被误判: %r" % (C.FAILURES,))

    def test_empty_tu_list_is_red_not_green(self):
        """空集扫描必须红:空的"全部通过"是最危险的假绿。"""
        self.assertFalse(C.scan_ui_dependency_graph(self.env, tus=[]))

    def test_ui_tus_actually_discovered(self):
        """保证扫的是真实 UI TU,而不是空集。

        ⚠ t5 之后"UI TU"的定义变了:桌面构建里不再有 jni_bridge.cpp
          (JNI 只在 ANDROID 分支),真正的 UI 侧消费者是
          tools/uisplit/probe_consumer.cpp。原来硬断言 jni_bridge 会永远红。
          现在只要求:发现到非空 TU 集合,且里面**至少有一个**是契约头的
          真实消费者(probe_consumer 或 jni_bridge,取决于平台)。
        """
        tus = C.ui_tus_from_build(self.env) or C.tus_from_cmake_lists(self.env)
        self.assertTrue(tus, "构建图和 CMakeLists 里都找不到 UI TU")
        self.assertTrue(any(("probe_consumer" in t) or ("jni_bridge" in t) for t in tus),
                        "UI TU 集合里既没有 probe_consumer 也没有 jni_bridge,"
                        "说明扫描扫到的不是 UI 侧文件: %r" % (tus,))


class N4JniContract(unittest.TestCase):
    """JNI 契约:Kotlin 少/多/改坏,都必须红。"""

    def setUp(self):
        reset_gates()
        self.env = make_env()
        self.kt_real, _ = C.parse_kotlin_externals(self.env)
        self.assertIsNotNone(self.kt_real, "真实 MbRenderer.kt 解析失败")
        self.kt_text = C.read_text(self._real_kt())

    def _real_kt(self):
        return os.path.join(MB, "android", "app", "src", "main", "java", "com", "magicblocks",
                            "pad", "MbRenderer.kt")

    def _mutated(self, name, mutate):
        text = mutate(self.kt_text)
        p = os.path.join(self.env.work, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        return p

    def test_real_contract_is_green(self):
        ok = C.check_jni_contract(self.env)
        self.assertTrue(ok, "真实 Kotlin/jni_bridge 契约判红: %r" % (C.FAILURES,))

    def test_missing_kotlin_decl_is_red(self):
        p = self._mutated("MbRenderer_missing.kt",
                          lambda t: t.replace("external fun nativeCreate(): Long", ""))
        reset_gates()
        self.assertFalse(C.check_jni_contract(make_env(extra_args={"kotlin_path": p})))
        self.assertTrue(any("nativeCreate" in f for f in C.FAILURES), C.FAILURES)

    def test_extra_kotlin_decl_is_red(self):
        p = self._mutated("MbRenderer_extra.kt",
                          lambda t: t.replace("external fun nativeDestroy(handle: Long)",
                                              "external fun nativeDestroy2(handle: Long)"))
        reset_gates()
        self.assertFalse(C.check_jni_contract(make_env(extra_args={"kotlin_path": p})))

    def test_changed_param_type_is_red(self):
        """`width: Int` → `width: Long`:真机是 UnsatisfiedLinkError,桌面必须变红。"""
        p = self._mutated("MbRenderer_type.kt",
                          lambda t: t.replace("window: Long, width: Int, height: Int",
                                              "window: Long, width: Long, height: Int"))
        reset_gates()
        self.assertFalse(C.check_jni_contract(make_env(extra_args={"kotlin_path": p})))
        self.assertTrue(any("nativeSurfaceCreated" in f for f in C.FAILURES), C.FAILURES)

    def test_changed_return_type_is_red(self):
        p = self._mutated("MbRenderer_ret.kt",
                          lambda t: t.replace("external fun nativeMsaaSamples(handle: Long): Int",
                                              "external fun nativeMsaaSamples(handle: Long): Long"))
        reset_gates()
        self.assertFalse(C.check_jni_contract(make_env(extra_args={"kotlin_path": p})))

    def test_renamed_log_callback_is_red(self):
        p = self._mutated("MbRenderer_log.kt",
                          lambda t: t.replace("fun logNative(msg: String)", "fun logNativeX(msg: String)"))
        reset_gates()
        self.assertFalse(C.check_jni_contract(make_env(extra_args={"kotlin_path": p})))
        self.assertTrue(any("logNative" in f for f in C.FAILURES), C.FAILURES)

    def test_parsed_count_matches_declarations(self):
        """数量必须与文件里 `external fun` 的字面出现次数一致。

        少解析出一个 = 那个入口从门控视野里消失 = 假绿的前身。
        这里不写死 13(入口数会随 t4 变),而是与文本里 `external fun` 的出现次数比对。
        同时要求与 C++ 侧数量相等 —— 双向一致才有意义。
        """
        expected = len(re.findall(r"external\s+fun\s+\w+\s*\(", self.kt_text))
        self.assertEqual(len(self.kt_real), expected,
                         "解析出 %d 个,文本里有 %d 个 external fun" % (len(self.kt_real), expected))
        bridge, _ = C.parse_jni_bridge_symbols(self.env)
        self.assertEqual(len(bridge), expected, "C++ 侧入口数与 Kotlin 不一致")


class N5CmakeExport(unittest.TestCase):
    """CMake 的 PUBLIC/INTERFACE include 面。"""

    def setUp(self):
        reset_gates()

    def _fake_magicblocks(self, cmake_text):
        d = os.path.join(VENV, "selftest_cmake_tree")
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
        with open(os.path.join(d, "CMakeLists.txt"), "w", encoding="utf-8") as f:
            f.write(cmake_text)
        return d

    def test_public_whole_include_dir_is_red(self):
        d = self._fake_magicblocks(
            "target_include_directories(magicblocks PUBLIC include PRIVATE src)\n")
        bad = C.check_cmake_public_includes(make_env(extra_args={"magicblocks": d}))
        self.assertTrue(bad, "PUBLIC include/ 没被抓住")
        self.assertTrue(any("include" in b for b in bad))

    def test_public_subdir_is_green(self):
        d = self._fake_magicblocks(
            "target_include_directories(magicblocks PUBLIC include/magicblocks PUBLIC include/mbc-serial "
            "PRIVATE src)\n")
        bad = C.check_cmake_public_includes(make_env(extra_args={"magicblocks": d}))
        self.assertFalse(bad, bad)

    def test_interface_generator_expression_is_red(self):
        d = self._fake_magicblocks(
            'target_include_directories(magicblocks INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")\n')
        bad = C.check_cmake_public_includes(make_env(extra_args={"magicblocks": d}))
        self.assertTrue(bad, "INTERFACE 整棵 include/ 没被抓住: %r" % (bad,))


@unittest.skipUnless(shutil.which("cl.exe") or os.name == "nt", "需要 MSVC")
class N6LinkAndRun(unittest.TestCase):
    """链接层 + 运行层:必须是"编→链→跑→查返回值"。"""

    def setUp(self):
        reset_gates()
        self.lib = find_lib()
        self.inc = copy_header_to_export(os.path.join(VENV, "selftest_link_inc"))

    def test_no_extern_c_fails_to_link(self):
        if not self.lib:
            self.skipTest("没有 DLL/导入库(--dll 或先跑 ui_isolation.ps1)")
        env = make_env(export_include=self.inc, lib=self.lib)
        src = ('#include <cstdint>\n'
               '// 模拟 mb_capi.h 忘了 extern "C":C++ 链接的声明 vs DLL 里的 C 符号\n'
               'std::uint32_t mb_abi_version();\n'
               'int main() { return (int)mb_abi_version(); }\n')
        res = C.build_and_run(env, src, [self.inc], "selftest_mangle")
        self.assertTrue(res["compiled"], res["log"][-2000:])
        self.assertFalse(res["linked"], "缺 extern \"C\" 竟然链接成功 —— 契约面不干净")
        self.assertRegex(res["log"], r"LNK2019|LNK2001|LNK1120")

    def test_wrong_version_assert_fails_at_runtime(self):
        if not self.lib:
            self.skipTest("没有 DLL/导入库")
        env = make_env(export_include=self.inc, lib=self.lib)
        src = ('#include <stdio.h>\n#include "magicblocks/mb_capi.h"\n'
               'int main() { unsigned v = mb_abi_version();\n'
               '  printf("runtime=%u header=%u\\n", v, (unsigned)MB_ABI_VERSION);\n'
               '  return v == MB_ABI_VERSION + 1 ? 0 : 9; }\n')
        res = C.build_and_run(env, src, [self.inc], "selftest_version")
        self.assertTrue(res["compiled"], res["log"][-2000:])
        self.assertTrue(res["linked"], res["log"][-2000:])
        self.assertEqual(res["exit"], 9, "版本不一致却没有让消费者失败: %r" % (res["out"],))

    def test_internal_header_unreachable_even_through_dll(self):
        if not self.lib:
            self.skipTest("没有 DLL/导入库")
        env = make_env(export_include=self.inc, lib=self.lib)
        src = ('#include "magicblocks/scene/scene.h"\n'
               '#include "magicblocks/mb_capi.h"\nint main() { return 0; }\n')
        res = C.build_and_run(env, src, [self.inc], "selftest_internal")
        self.assertFalse(res["compiled"], "内部头在导出面下仍然能 include")


@unittest.skipUnless(shutil.which("cl.exe") or os.name == "nt", "需要 MSVC")
class N7Symbols(unittest.TestCase):
    """导出表:26 个 MB_API 入口必须是裸名。"""

    def test_all_mb_api_symbols_exported(self):
        lib = find_lib()
        if not lib:
            self.skipTest("没有 DLL/导入库")
        reset_gates()
        env = make_env(lib=lib, export_include=copy_header_to_export(
            os.path.join(VENV, "selftest_sym_inc")))
        ok = C.check_symbols(env)
        # 允许"多出未登记的 mb_*"这一类判红——那说明 ABI 面比 26 个更宽,是要报出来的
        # 发现,不是本自测的失败;本自测只要求"26 个裸名都在"。
        missing = [f for f in C.FAILURES if "缺这几个裸名符号" in f]
        self.assertFalse(missing, missing)
        self.assertGreaterEqual(len(C.MB_API_SYMBOLS), 20)

    def test_symbol_list_matches_header(self):
        """硬编码的 MB_API_SYMBOLS 必须与头文件里标了 MB_API 的函数名一致。

        这条防的是"实现加了入口、头文件写了,但门控清单没加"—— 那样门控会
        对新增入口视而不见。
        """
        text = C.read_text(os.path.join(REAL_INCLUDE, "magicblocks", "mb_capi.h"))
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        found = set(re.findall(r"MB_API\s+[\w \*]+?\bMB_CALL\s+(mb_\w+)\s*\(", text))
        self.assertEqual(found, set(C.MB_API_SYMBOLS),
                         "头文件里的 MB_API 入口与门控清单不一致:头里有门控没列 %r,"
                         "门控列了头里没有 %r" % (sorted(found - set(C.MB_API_SYMBOLS)),
                                              sorted(set(C.MB_API_SYMBOLS) - found)))


class N8LinkEvidence(unittest.TestCase):
    """链接层证据本身必须能红 —— 否则"编→链→跑"里的第二层是个空转的门。

    captain 的要求很具体:编译产物、链接产物(含被解析的符号清单)、运行输出
    三者分开作证。这一组测的就是中间那一层的取证器:
      · 消费者 exe 的导入表里**必须**出现被测 DLL 名;
      · 消费者真正调用的 mb_* 必须是**裸名**出现在导入表里;
      · 那些裸名必须在 DLL 导出表里对得上。
    每条都要有"缺了就报红"的反例,不然又是一句空话。
    """

    def setUp(self):
        reset_gates()
        self.lib = find_lib()
        self.inc = copy_header_to_export(os.path.join(VENV, "selftest_linkev_inc"))

    def test_import_table_shows_expected_dll(self):
        """正例:消费者 exe 的导入表里确实有 magicblocks.dll。"""
        if not self.lib:
            self.skipTest("没有 DLL/导入库")
        env = make_env(export_include=self.inc, lib=self.lib)
        src = ('#include "magicblocks/mb_capi.h"\n'
               'int main() { return mb_abi_version() == MB_ABI_VERSION ? 0 : 1; }\n')
        res = C.build_and_run(env, src, [self.inc], "selftest_linkev_pos")
        self.assertTrue(res["compiled"] and res["linked"], res["log"][-2000:])
        exe = os.path.join(env.work, "consumer_selftest_linkev_pos", "consumer.exe")
        ev = C.consumer_import_evidence(env, exe)
        self.assertIn("magicblocks.dll", ev["dll_names"],
                      "exe 导入表里没有被测 DLL: %r" % (ev["dll_names"],))
        self.assertIn("mb_abi_version", ev["symbols"],
                      "exe 导入表里没有裸名 mb_abi_version: %r" % (sorted(ev["symbols"]),))

    def test_mangled_import_is_not_counted_as_resolved(self):
        """反例:去掉 extern "C" 后连不上,导入表里不该有"看起来对"的裸名。

        这正是 t2 的事故形态。自测要证明:链接失败时,取证器不会因为
        "名字凑巧出现过"就判绿。
        """
        if not self.lib:
            self.skipTest("没有 DLL/导入库")
        env = make_env(export_include=self.inc, lib=self.lib)
        src = ('#include <cstdint>\n'
               'std::uint32_t mb_abi_version();\n'   # 故意不带 extern "C"
               'int main() { return (int)mb_abi_version(); }\n')
        res = C.build_and_run(env, src, [self.inc], "selftest_linkev_mangle")
        self.assertTrue(res["compiled"], res["log"][-2000:])
        self.assertFalse(res["linked"], "缺 extern \"C\" 竟然链接成功")
        self.assertRegex(res["log"], r"LNK2019|LNK2001|LNK1120")

    def test_consumer_expected_symbols_come_from_source_not_handwritten(self):
        """CONSUMER_EXPECTED_SYMBOLS 必须是从 UI_CONSUMER 源码抽出来的。

        手抄清单会漂移:有人给消费者加了调用、清单没跟上,链接层就悄悄少检。
        这里反向验证:清单里的每个符号都必须真的出现在模板里。
        """
        self.assertTrue(C.CONSUMER_EXPECTED_SYMBOLS, "抽取结果为空 —— 抽取器坏了")
        for s in C.CONSUMER_EXPECTED_SYMBOLS:
            self.assertIn(s + "(", C.UI_CONSUMER,
                          "清单里的 %s 在 UI_CONSUMER 源码里找不到(清单漂移了)" % s)
        # 且真的覆盖了 UI 生命周期的四个关键入口
        for must in ("mb_session_create", "mb_session_attach_surface",
                     "mb_session_render", "mb_session_destroy"):
            self.assertIn(must, C.CONSUMER_EXPECTED_SYMBOLS)

    def test_missing_dll_in_import_table_is_red(self):
        """反例:导入表里没有被测 DLL 时,消费者门控必须报红而不是放行。"""
        if not self.lib:
            self.skipTest("没有 DLL/导入库")
        env = make_env(export_include=self.inc, lib=self.lib)
        # 一个**不引用任何 mb_* 符号**的消费者:编得过、链得过(不需要该 lib),
        # 但导入表里根本不会有 magicblocks.dll。门控必须识别出"链接证据不成立"。
        src = '#include <stdio.h>\nint main() { printf("no mb symbols\\n"); return 7; }\n'
        res = C.build_and_run(env, src, [self.inc], "selftest_linkev_nodll")
        self.assertTrue(res["compiled"] and res["linked"], res["log"][-2000:])
        exe = os.path.join(env.work, "consumer_selftest_linkev_nodll", "consumer.exe")
        ev = C.consumer_import_evidence(env, exe)
        self.assertNotIn("magicblocks.dll", ev["dll_names"],
                         "没有引用 mb_* 的 exe 竟然依赖了被测 DLL —— 取证器判断依据有问题")


if __name__ == "__main__":
    unittest.main(verbosity=2)
