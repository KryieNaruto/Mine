import os
import sys
import tempfile
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


class TestStream(unittest.TestCase):
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

    def test_linux_does_not_force_cl(self):
        root = self._make_pool()
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        with mock.patch.object(cmake_driver.pool, "on_windows", return_value=False):
            cmd = cmake_driver.configure_command(root, lib, "release")
        joined = " ".join(cmd)
        self.assertNotIn("-DCMAKE_C_COMPILER=cl", joined)
        self.assertNotIn("-DCMAKE_CXX_COMPILER=cl", joined)


if __name__ == "__main__":
    unittest.main()
