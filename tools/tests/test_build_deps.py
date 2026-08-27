import importlib.util
import os
import sys
import tempfile
import unittest
from unittest import mock

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)  # tools/ 便于 deps_lib 导入
_spec = importlib.util.spec_from_file_location(
    "build_deps_mod", os.path.join(_TOOLS, "build-deps.py"))
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
topo_expand = _mod.topo_expand
_ensure_msvc_env = _mod._ensure_msvc_env
_vcvars_bat = _mod._vcvars_bat
_msys_linked = _mod._msys_linked

from deps_lib.manifest import LibSpec


def _lib(name, deps=()):
    return LibSpec(name=name, repo=f"r/{name}", tag="v1", depends_on=deps)


class TestTopoExpand(unittest.TestCase):
    def test_dep_built_before_dependent(self):
        libs = [_lib("B", ("A",)), _lib("A")]
        order = topo_expand(libs, {})
        self.assertEqual([l.name for l in order], ["A", "B"])

    def test_no_duplicate_when_dep_listed(self):
        libs = [_lib("A"), _lib("B", ("A",))]
        order = topo_expand(libs, {})
        self.assertEqual([l.name for l in order], ["A", "B"])

    def test_transitive_deps_expanded(self):
        libs = [_lib("C", ("B",)), _lib("A")]
        gm = {"B": {"repo": "r/B", "tag": "v1", "depends_on": ["A"]}}
        order = topo_expand(libs, gm)
        self.assertEqual([l.name for l in order], ["A", "B", "C"])

    def test_cycle_raises(self):
        libs = [_lib("A", ("B",)), _lib("B", ("A",))]
        with self.assertRaises(RuntimeError):
            topo_expand(libs, {})

    def test_missing_dep_raises(self):
        libs = [_lib("B", ("nope",))]
        with self.assertRaises(RuntimeError):
            topo_expand(libs, {})


class _FakeRun:
    """mock subprocess.run:把 stdout 写到子进程的 stdout 文件对象,返回 {returncode}。

    现在 _ensure_msvc_env 用文件重定向(不再 capture_output),mock 须把内容写入
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
        # 清掉可能存在的 MSVC 环境,保证走注入分支
        for k in ("VCINSTALLDIR",):
            os.environ.pop(k, None)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._old)

    def _force_win(self):
        return mock.patch.object(_mod.pool, "on_windows", return_value=True)

    def test_linux_noop(self):
        with mock.patch.object(_mod.pool, "on_windows", return_value=False):
            self.assertTrue(_ensure_msvc_env())

    def test_injects_vcvars_env_into_os_environ(self):
        # Windows + 找到 vcvars + cmd 导出含 cl → 环境被注入
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\nINCLUDE=C:\\vc\\inc\n", rc=0)
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(_mod.subprocess, "run", side_effect=fake), \
             mock.patch.object(_mod.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(_ensure_msvc_env())
        self.assertEqual(os.environ.get("INCLUDE"), r"C:\vc\inc")

    def test_no_vcvars_returns_false(self):
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=""):
            self.assertFalse(_ensure_msvc_env())

    def test_vcvars_fails_returns_false(self):
        fake = _FakeRun(stdout="", rc=1)
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(_mod.subprocess, "run", side_effect=fake):
            self.assertFalse(_ensure_msvc_env())

    def test_no_cl_after_inject_returns_false(self):
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\n", rc=0)
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(_mod.subprocess, "run", side_effect=fake), \
             mock.patch.object(_mod.shutil, "which", return_value=None):
            self.assertFalse(_ensure_msvc_env())

    def test_uses_file_redirect_not_pipe_to_avoid_deadlock(self):
        # 回归:`cmd //c vcvars && set` 的子进程链会持有 stdout 管道 → capture_output
        # 等 EOF 永远等不到 → 屏幕只打印头部就卡死(本机已复现 capture_output 阻塞
        # 3s+ 等后台子进程释放管道,timeout 不触发)。改为文件重定向(capture_output=False)
        # 避免管道死锁;即便 cmd 链持有 stdout 也只是在文件上,无 EOF 可等。
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\nINCLUDE=C:\\vc\\inc\n", rc=0)
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(_mod.subprocess, "run", side_effect=fake) as run, \
             mock.patch.object(_mod.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(_ensure_msvc_env())
        _, kwargs = run.call_args
        self.assertFalse(kwargs.get("capture_output", False),
                         "capture_output 用管道会与 vcvars 子进程链死锁,必须改文件重定向")
        # stdout 必须是文件对象(重定向到文件),而非 capture_output 的管道
        self.assertTrue(hasattr(kwargs.get("stdout"), "write"),
                        "stdout 应为文件对象,避免管道 EOF 死锁")
        self.assertNotIn("stderr", kwargs)

    def test_uses_c_switch_for_native_python(self):
        # 回归(卡死根因):原生 Windows python 的 subprocess 参数不经 MSYS 转换,
        # `//c` 原样进 cmd → cmd 不认该开关,打开交互 shell 等 stdin → 卡到 timeout,
        # vcvars 根本没跑(本机已复现 `//c "echo OK"` 出交互 banner)。必须 `/c`。
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\nINCLUDE=C:\\vc\\inc\n", rc=0)
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(_mod, "_msys_linked", return_value=False), \
             mock.patch.object(_mod.subprocess, "run", side_effect=fake) as run, \
             mock.patch.object(_mod.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(_ensure_msvc_env())
        args, _ = run.call_args
        self.assertEqual(args[0][1], "/c",
                         "原生 python 下 cmd 开关必须 /c;`//c` 会让 cmd 开交互 shell 卡死")

    def test_uses_double_slash_for_msys_linked_python(self):
        # MSYS 链接的 python:其运行时会把 `/c` 当路径转成 C:\,必须 `//c` 防转换。
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\n", rc=0)
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(_mod, "_msys_linked", return_value=True), \
             mock.patch.object(_mod.subprocess, "run", side_effect=fake) as run, \
             mock.patch.object(_mod.shutil, "which", return_value=r"C:\vc\bin\cl.exe"):
            self.assertTrue(_ensure_msvc_env())
        args, _ = run.call_args
        self.assertEqual(args[0][1], "//c")

    def test_msys_linked_false_on_linux(self):
        with mock.patch.object(_mod.pool, "on_windows", return_value=False):
            self.assertFalse(_msys_linked())


class TestVcvarsBat(unittest.TestCase):
    """_vcvars_bat:vcvars.sh 里是 MSYS 风格路径,必须转 Windows 风格供 cmd 调用。"""

    def _write_vcvars_sh(self, root, value):
        d = os.path.join(root, ".user-deps")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "vcvars.sh"), "w", encoding="utf-8") as f:
            f.write(f'export VC_VARS_BAT="{value}"\n')
        return os.path.join(d, "vcvars.sh")

    def test_msys_path_converted_to_windows(self):
        # 回归:msvc.sh 写 /c/Program Files/... 的 MSYS 路径,若原样交给 cmd
        # 会被 cmd 剥引号并按空格切,执行 '/Program' → rc=1。必须转 C:\...。
        with tempfile.TemporaryDirectory() as td, \
             mock.patch.object(_mod, "MINE_ROOT", td):
            self._write_vcvars_sh(
                td,
                "/c/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat")
            self.assertEqual(
                _vcvars_bat(),
                r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
            )

    def test_windows_path_passthrough(self):
        # 已是 Windows 风格(如磁盘扫描回退)则原样返回。
        with tempfile.TemporaryDirectory() as td, \
             mock.patch.object(_mod, "MINE_ROOT", td):
            self._write_vcvars_sh(
                td,
                r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
            self.assertEqual(
                _vcvars_bat(),
                r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
            )


if __name__ == "__main__":
    unittest.main()
