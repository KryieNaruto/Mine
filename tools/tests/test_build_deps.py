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
    """mock subprocess.run:返回 {stdout, returncode} 假对象。"""
    def __init__(self, stdout="", rc=0):
        self.stdout = stdout
        self.returncode = rc
        self.stderr = ""


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
             mock.patch.object(_mod.subprocess, "run", return_value=fake), \
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
             mock.patch.object(_mod.subprocess, "run", return_value=fake):
            self.assertFalse(_ensure_msvc_env())

    def test_no_cl_after_inject_returns_false(self):
        fake = _FakeRun(stdout="PATH=C:\\vc\\bin;X\n", rc=0)
        with self._force_win(), \
             mock.patch.object(_mod, "_vcvars_bat", return_value=r"C:\vc\vcvars64.bat"), \
             mock.patch.object(_mod.subprocess, "run", return_value=fake), \
             mock.patch.object(_mod.shutil, "which", return_value=None):
            self.assertFalse(_ensure_msvc_env())


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
