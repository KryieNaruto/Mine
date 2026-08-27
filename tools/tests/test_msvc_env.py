import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
from deps_lib import msvc_env


class _FakeRun:
    """mock subprocess.run:把 stdout 写到子进程的 stdout 文件对象,返回 {returncode}。

    ensure_msvc_env 用文件重定向(不是 capture_output), mock 须把内容写入
    run 收到的 stdout 文件对象,代码才会从文件读回注入环境。
    """
    def __init__(self, stdout="", rc=0):
        self._stdout = stdout
        self.returncode = rc

    def __call__(self, cmd, **kwargs):
        f = kwargs.get("stdout")
        if f is not None:
            f.write(self._stdout.encode("utf-8"))
            f.flush()
            f.close()
        return self


class TestEnsureMsvcEnv(unittest.TestCase):
    def setUp(self):
        self._old = dict(os.environ)
        for k in ("VCINSTALLDIR",):
            os.environ.pop(k, None)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._old)

    def _force_win(self):
        return mock.patch.object(msvc_env.pool, "on_windows", return_value=True)

    def test_linux_noop(self):
        with mock.patch.object(msvc_env.pool, "on_windows", return_value=False):
            self.assertTrue(msvc_env.ensure_msvc_env("/any/root"))

    def test_injects_vcvars_env_into_os_environ(self):
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\nINCLUDE=C:\\vc\\inc\n", rc=0)
        with self._force_win(), \
             mock.patch.object(msvc_env, "find_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(msvc_env.subprocess, "run", side_effect=fake), \
             mock.patch.object(msvc_env.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(msvc_env.ensure_msvc_env("/any/root"))
        self.assertEqual(os.environ.get("INCLUDE"), r"C:\vc\inc")

    def test_no_vcvars_returns_false(self):
        with self._force_win(), \
             mock.patch.object(msvc_env, "find_vcvars_bat", return_value=""):
            self.assertFalse(msvc_env.ensure_msvc_env("/any/root"))

    def test_vcvars_fails_returns_false(self):
        fake = _FakeRun(stdout="", rc=1)
        with self._force_win(), \
             mock.patch.object(msvc_env, "find_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(msvc_env.subprocess, "run", side_effect=fake):
            self.assertFalse(msvc_env.ensure_msvc_env("/any/root"))

    def test_no_cl_after_inject_returns_false(self):
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\n", rc=0)
        with self._force_win(), \
             mock.patch.object(msvc_env, "find_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(msvc_env.subprocess, "run", side_effect=fake), \
             mock.patch.object(msvc_env.shutil, "which", return_value=None):
            self.assertFalse(msvc_env.ensure_msvc_env("/any/root"))

    def test_uses_file_redirect_not_pipe_to_avoid_deadlock(self):
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\nINCLUDE=C:\\vc\\inc\n", rc=0)
        with self._force_win(), \
             mock.patch.object(msvc_env, "find_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(msvc_env.subprocess, "run", side_effect=fake) as run, \
             mock.patch.object(msvc_env.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(msvc_env.ensure_msvc_env("/any/root"))
        _, kwargs = run.call_args
        self.assertFalse(kwargs.get("capture_output", False))
        self.assertTrue(hasattr(kwargs.get("stdout"), "write"))
        self.assertNotIn("stderr", kwargs)

    def test_uses_c_switch_for_native_python(self):
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\nINCLUDE=C:\\vc\\inc\n", rc=0)
        with self._force_win(), \
             mock.patch.object(msvc_env, "find_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(msvc_env, "is_msys_linked_python", return_value=False), \
             mock.patch.object(msvc_env.subprocess, "run", side_effect=fake) as run, \
             mock.patch.object(msvc_env.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(msvc_env.ensure_msvc_env("/any/root"))
        args, _ = run.call_args
        self.assertEqual(args[0][1], "/c")
        self.assertTrue(args[0][2].endswith(".cmd"))
        self.assertNotIn("vcvars64.bat", args[0][2])

    def test_uses_double_slash_for_msys_linked_python(self):
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\n", rc=0)
        with self._force_win(), \
             mock.patch.object(msvc_env, "find_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(msvc_env, "is_msys_linked_python", return_value=True), \
             mock.patch.object(msvc_env.subprocess, "run", side_effect=fake) as run, \
             mock.patch.object(msvc_env.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(msvc_env.ensure_msvc_env("/any/root"))
        args, _ = run.call_args
        self.assertEqual(args[0][1], "//c")

    def test_msys_linked_false_on_linux(self):
        with mock.patch.object(msvc_env.pool, "on_windows", return_value=False):
            self.assertFalse(msvc_env.is_msys_linked_python())


class TestFindVcvarsBat(unittest.TestCase):
    """find_vcvars_bat:vcvars.sh 里是 MSYS 风格路径,必须转 Windows 风格供 cmd 调用。"""

    def _write_vcvars_sh(self, root, value):
        d = os.path.join(root, ".user-deps")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "vcvars.sh"), "w", encoding="utf-8") as f:
            f.write(f'export VC_VARS_BAT="{value}"\n')
        return os.path.join(d, "vcvars.sh")

    def test_msys_path_converted_to_windows(self):
        with tempfile.TemporaryDirectory() as td:
            self._write_vcvars_sh(
                td,
                "/c/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat")
            self.assertEqual(
                msvc_env.find_vcvars_bat(td),
                r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
            )

    def test_windows_path_passthrough(self):
        with tempfile.TemporaryDirectory() as td:
            self._write_vcvars_sh(
                td,
                r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
            self.assertEqual(
                msvc_env.find_vcvars_bat(td),
                r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
            )


if __name__ == "__main__":
    unittest.main()
