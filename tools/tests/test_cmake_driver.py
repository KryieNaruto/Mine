import io
import os
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
from deps_lib import cmake_driver
from deps_lib.manifest import LibSpec


class TestConfigureCommand(unittest.TestCase):
    def test_command_shape(self):
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1", options=["FMT_TEST=OFF"])
        cmd = cmake_driver.configure_command("/root", lib, "release")
        self.assertEqual(cmd[0], "cmake")
        self.assertEqual(cmd[1], "-S")
        self.assertIn("-DCMAKE_BUILD_TYPE=release", cmd)
        self.assertIn("-DCMAKE_INSTALL_PREFIX=", " ".join(cmd))
        self.assertIn("-DFMT_TEST=OFF", cmd)
        self.assertIn("-G", cmd)
        self.assertIn("Ninja", cmd)


class TestPrefixPathInjection(unittest.TestCase):
    def _make_pool(self, built_release, built_debug):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = tmp.name
        for name in built_release:
            d = os.path.join(root, "third_party", "_install", name, "release")
            os.makedirs(d)
            with open(os.path.join(d, ".built"), "w") as f:
                f.write("")
        for name in built_debug:
            d = os.path.join(root, "third_party", "_install", name, "debug")
            os.makedirs(d)
            with open(os.path.join(d, ".built"), "w") as f:
                f.write("")
        return root

    def test_injects_only_built_release_prefixes(self):
        root = self._make_pool(
            built_release=["abseil-cpp-20260817.0", "glm-1.0.1"],
            built_debug=["abseil-cpp-20260817.0"],
        )
        lib = LibSpec(name="ink-stroke-modeler", repo="r", tag="main")
        cmd = cmake_driver.configure_command(root, lib, "release")
        joined = " ".join(cmd)
        self.assertIn("-DCMAKE_PREFIX_PATH=", joined)
        # 含已 build 的 release 前缀
        self.assertIn(
            os.path.join(root, "third_party", "_install", "abseil-cpp-20260817.0", "release"),
            joined,
        )
        self.assertIn(
            os.path.join(root, "third_party", "_install", "glm-1.0.1", "release"),
            joined,
        )
        # 不含未 build 的 debug 前缀(debug 前缀绝不进 release 的 PREFIX_PATH)
        self.assertNotIn(os.path.join(root, "third_party", "_install", "glm-1.0.1", "debug"), joined)

    def test_no_prefix_flag_when_nothing_built(self):
        root = self._make_pool(built_release=[], built_debug=[])
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        cmd = cmake_driver.configure_command(root, lib, "release")
        self.assertNotIn("-DCMAKE_PREFIX_PATH=", " ".join(cmd))


class _FakeProc:
    def __init__(self, lines, rc):
        self._lines = list(lines)
        self._rc = rc
        self.stdout = _FakeIter(self._lines)

    def wait(self):
        return self._rc


class _FakeIter:
    def __init__(self, lines):
        self._lines = lines
        self._i = 0

    def __iter__(self):
        return self

    def __next__(self):
        if self._i >= len(self._lines):
            raise StopIteration
        line = self._lines[self._i]
        self._i += 1
        return line

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _GbkConsole:
    """模拟 Windows GBK 控制台(stdout.encoding='gbk',mintty 下原生 Windows Python 常如此)。

    修复前:编码错误处理默认 strict,子进程输出含 U+FFFD(text=True errors="replace"
    解码产物)时 print 回 GBK 抛 UnicodeEncodeError → _stream 崩、编译中断。
    修复后:_make_output_safe 把错误处理降为 replace,U+FFFD 替换成 `?`,不崩。
    """

    encoding = "gbk"

    def __init__(self):
        self.lines = []
        self._replace = False

    def reconfigure(self, *, errors=None, **kw):
        self._replace = errors == "replace"

    def write(self, s):
        if self._replace:
            self.lines.append(s.encode("gbk", "replace").decode("gbk"))
        else:
            s.encode("gbk")  # 未修复时 U+FFFD 在此抛 UnicodeEncodeError
        return len(s)

    def flush(self):
        pass


class TestStream(unittest.TestCase):
    def test_gbk_console_does_not_crash_on_unencodable(self):
        # 回归:Windows GBK 控制台下,子进程输出含 U+FFFD(errors="replace" 解码产物),
        # 修复前 print 回 GBK 抛 UnicodeEncodeError 崩掉长编译;修复后替换为 `?` 不崩,
        # 且失败日志仍保留原始内容便于排查。
        out = _GbkConsole()
        with mock.patch.object(sys, "stdout", out):
            ok, tail = cmake_driver._stream([sys.executable, "-u", "-c", "print('\\ufffd')"])
        self.assertTrue(ok)
        self.assertEqual("".join(out.lines), "?\n")  # 控制台侧 U+FFFD → `?`,不崩
        self.assertIn("�", tail)               # 日志侧保留原始内容

    def test_real_passthrough_and_tail_on_failure(self):
        # 真子进程:非零退出,输出被透传且尾部留作失败日志
        ok, tail = cmake_driver._stream(
            [sys.executable, "-c", "print('line-a'); print('line-b'); raise SystemExit(2)"]
        )
        self.assertFalse(ok)
        self.assertIn("line-a", tail)
        self.assertIn("line-b", tail)

    def test_real_success(self):
        ok, tail = cmake_driver._stream([sys.executable, "-c", "print('hello')"])
        self.assertTrue(ok)
        self.assertIn("hello", tail)

    def test_missing_command(self):
        ok, tail = cmake_driver._stream(["/nonexistent/cmd-xyz"])
        self.assertFalse(ok)
        self.assertIn("命令不存在", tail)

    def test_cr_progress_emitted_live(self):
        # 回归:Ninja/CMake 的 \r 进度行若被 _stream 攒到 \n 才打印,SwiftShader
        # 编译期屏幕零输出,形似卡死。此例用真实子进程写 \r 进度 + 停顿 + \n 收尾,
        # 断言 \r 进度在进程结束前就透传到 stdout(即「实时」而非攒在管道里)。
        code = ("import sys,time;"
                "sys.stdout.write('\\r[1/2] building...');sys.stdout.flush();"
                "time.sleep(0.5);"
                "sys.stdout.write('\\r[2/2] building...');sys.stdout.flush();"
                "sys.stdout.write('\\ndone\\n');sys.stdout.flush()")
        out = io.StringIO()
        with mock.patch.object(sys, "stdout", out):
            ok, tail = cmake_driver._stream([sys.executable, "-u", "-c", code])
        self.assertTrue(ok)
        self.assertIn("done", tail)
        self.assertIn("[2/2] building...", tail)
        # 进度行必须在进程退出前就出现在 stdout(实时);攒到退出才打则说明回归
        self.assertIn("[1/2] building...", out.getvalue())


class TestBuildLib(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name

    def test_success_writes_built(self):
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        with mock.patch.object(cmake_driver, "_post_install_copy", return_value=(True, "")), \
             mock.patch("deps_lib.cmake_driver.subprocess.Popen",
                        return_value=_FakeProc([], 0)):
            ok, err = cmake_driver.build_lib(self.root, lib, "release", jobs=2)
        self.assertTrue(ok, err)
        built = os.path.join(self.root, "third_party", "_install", "fmt-10.2.1", "release", ".built")
        self.assertTrue(os.path.isfile(built))

    def test_failure_no_built(self):
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        with mock.patch("deps_lib.cmake_driver.subprocess.Popen",
                        return_value=_FakeProc(["configure boom"], 3)):
            ok, err = cmake_driver.build_lib(self.root, lib, "release", jobs=2)
        self.assertFalse(ok)
        self.assertIn("configure boom", err)
        built = os.path.join(self.root, "third_party", "_install", "fmt-10.2.1", "release", ".built")
        self.assertFalse(os.path.exists(built))


class TestMsvcConfigureCommand(unittest.TestCase):
    def _make_pool(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = tmp.name
        d = os.path.join(root, "third_party", "_install", "abseil-cpp-20260817.0", "release")
        os.makedirs(d)
        with open(os.path.join(d, ".built"), "w") as f:
            f.write("")
        return root

    def test_no_mingw64_and_msvc_runtime(self):
        root = self._make_pool()
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        cmd = cmake_driver.configure_command(root, lib, "release")
        joined = " ".join(cmd)
        self.assertNotIn("/mingw64", joined)
        self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL", joined)
        # 注入已 build 的 MSVC 池前缀(abseil)
        self.assertIn(os.path.join(root, "third_party", "_install", "abseil-cpp-20260817.0", "release"), joined)

    def test_windows_forces_cl_compiler(self):
        # Windows:configure 必须显式指定 cl,否则 PATH 里残留 g++ 时 CMake 选错(MingW 编 SwiftShader 必崩)
        root = self._make_pool()
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        with mock.patch.object(cmake_driver.pool, "on_windows", return_value=True):
            cmd = cmake_driver.configure_command(root, lib, "release")
        joined = " ".join(cmd)
        self.assertIn("-DCMAKE_C_COMPILER=cl", joined)
        self.assertIn("-DCMAKE_CXX_COMPILER=cl", joined)
        # 回归:abseil 要求 CXX_STANDARD + CXX_STANDARD_REQUIRED 同时设,否则 MSVC 下
        # 走 check_cxx_source_compiles 探测失败报 "compiler defaults to C++ < 17"。
        self.assertIn("-DCMAKE_CXX_STANDARD=20", joined)
        self.assertIn("-DCMAKE_CXX_STANDARD_REQUIRED=ON", joined)

    def test_linux_does_not_force_cl(self):
        root = self._make_pool()
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        with mock.patch.object(cmake_driver.pool, "on_windows", return_value=False):
            cmd = cmake_driver.configure_command(root, lib, "release")
        joined = " ".join(cmd)
        self.assertNotIn("-DCMAKE_C_COMPILER=cl", joined)
        self.assertNotIn("-DCMAKE_CXX_COMPILER=cl", joined)
        # Linux 不强制 C++20:靠 abseil 自己的编译探测按真实编译器能力走(旧 g++ 可 C++17)
        self.assertNotIn("-DCMAKE_CXX_STANDARD_REQUIRED=ON", joined)


if __name__ == "__main__":
    unittest.main()
