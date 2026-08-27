# 按项目类型自动生成 IDE 工程(project-type-generation)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让工作区工具按每个项目 `deps.yaml` 里的 `type:` 字段自动生成对应 IDE 工程——现在实现 `vs`(CMake VS generator 出 `.sln`),`as` 登记占位。

**Architecture:** 新增 `tools/deps_lib/project_gen.py` 作为可测试的核心库(项目扫描 + 类型注册表 + 生成器),`tools/gen-projects.py` 是薄 CLI 包装(参数解析 + 汇总打印),风格对齐现有 `tools/build-deps.py` / `tools/deps_lib/cmake_driver.py` 的库/CLI 分层。作为前置重构,把 `build-deps.py` 里内联的 MSVC vcvars 注入逻辑搬进新模块 `tools/deps_lib/msvc_env.py`,供 `gen-projects.py` 复用(configure VS 工程同样需要 `cl.exe` 在 PATH)。

**Tech Stack:** Python 3(标准库 `argparse`/`subprocess`/`os`/`re` + `PyYAML`),CMake(Visual Studio generator),Bash(`setup-env.sh`),`unittest` + `unittest.mock`。

**Spec:** `docs/superpowers/specs/2026-08-27-project-type-generation-design.md`

## Global Constraints

- 项目类型取值仅 `vs` | `as`;`deps.yaml` 缺 `type:` 字段时缺省按 `vs` 兜底。
- `vs` 生成器仅在 Windows(`pool.on_windows()` 为真)执行;Linux/非 Windows 返回"跳过",不算失败。
- `.sln` 只含 **Release** 配置(`-DCMAKE_CONFIGURATION_TYPES=Release`),指向池 `release` 变体——v1 明确不支持 Debug 配置。
- `vs` generator 版本运行时探测(`cmake --help` 里取年份最大的 "Visual Studio N YYYY"),不写死版本;`--generator` 可覆盖。
- `list_projects()` 排除:`tools/ third_party/ docs/ .claude/ .github/ .user-deps/ .superpowers/` 及任意以 `.` 开头的目录。
- CLI 遵循 `build-deps.py` 既有约定:`--all`/`--project <dir>` 互斥(`add_mutually_exclusive_group(required=True)`);单项目失败不中断其它项目;最终按 `[GENERATED]/[SKIPPED]/[TODO]/[FAILED]` 分类打印汇总;仅 `FAILED`(真失败,不含 `TODO` 占位/`SKIPPED` 平台跳过)计入非零退出码。
- `msvc_env.py` 搬移是纯重构:函数体逻辑不变,仅把隐式 `MINE_ROOT` 全局改为显式 `root` 参数;原有回归测试断言不变,只改指向的模块。

---

### Task 1: 抽取 `tools/deps_lib/msvc_env.py`(MSVC vcvars 注入,纯搬移)

**Files:**
- Create: `tools/deps_lib/msvc_env.py`
- Create: `tools/tests/test_msvc_env.py`
- Modify: `tools/build-deps.py:1-165`(删除内联的 `_vcvars_bat`/`_tail`/`_msys_linked`/`_ensure_msvc_env`,改为导入新模块)
- Modify: `tools/build-deps.py:255`(调用点改为 `msvc_env.ensure_msvc_env(MINE_ROOT)`)
- Modify: `tools/tests/test_build_deps.py:1-216`(删除 `TestEnsureMsvcEnv`/`TestVcvarsBat`/`_FakeRun` 及相关导入,只留 `TestTopoExpand`)

**Interfaces:**
- Produces(供 Task 4 使用):
  - `deps_lib.msvc_env.ensure_msvc_env(root: str) -> bool` —— Windows 上把 vcvars64 环境注入 `os.environ`;Linux 上直接返回 `True`(no-op);失败返回 `False` 并已打印 `[ERROR]`。
  - `deps_lib.msvc_env.find_vcvars_bat(root: str) -> str` —— 定位 vcvars64.bat,找不到返回空串。
  - `deps_lib.msvc_env.is_msys_linked_python() -> bool` —— 当前 Python 是否 MSYS 链接。

- [ ] **Step 1: 写新模块的失败测试(先写测试,后写实现)**

创建 `tools/tests/test_msvc_env.py`:

```python
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
from deps_lib import msvc_env


class _FakeRun:
    """mock subprocess.run:把 stdout 写到子进程的 stdout 文件对象,返回 {returncode}。

    ensure_msvc_env 用文件重定向(不是 capture_output),mock 须把内容写入
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
```

- [ ] **Step 2: 跑测试确认失败(模块还不存在)**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest tools.tests.test_msvc_env -v`
Expected: `ModuleNotFoundError: No module named 'deps_lib.msvc_env'`

- [ ] **Step 3: 创建 `tools/deps_lib/msvc_env.py`(纯搬移 + 参数化 root)**

```python
"""MSVC(vcvars64)环境探测与注入 —— 供 build-deps.py / gen-projects.py 复用。"""
from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

from . import pool


def find_vcvars_bat(root: str) -> str:
    """定位 vcvars64.bat 的 Windows 路径;找不到返回空。

    优先读 <root>/.user-deps/vcvars.sh(win-deps.sh 生成,记录 VC_VARS_BAT,MSYS 风格路径;
    win-deps 经 msvc_locate 已选到正确 VS 根,如 18/Insiders);
    缺则回退扫描标准 VS 安装根,优先含 VC/Tools/MSVC(真实 C++ 工具集)的实例。
    返回 Windows 风格路径(带盘符反斜杠),供 cmd 调用(开关按运行时选 /c 或 //c)。
    """
    # 1) win-deps.sh 已写的 vcvars.sh —— 最可靠,含 msvc_locate 选中的根
    vs = os.path.join(root, ".user-deps", "vcvars.sh")
    if os.path.isfile(vs):
        try:
            with open(vs, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.match(r'^\s*export\s+VC_VARS_BAT="?(.+?)"?\s*$', line)
                    if m:
                        p = m.group(1).strip()
                        if p:
                            # vcvars.sh 存的是 MSYS 风格(/c/...),cmd 只认盘符反斜杠路径;
                            # 不转换就原样给 cmd,cmd 会剥引号按空格切,执行 '/Program' → rc=1。
                            if re.match(r"^/[a-zA-Z]/", p):
                                p = p[1].upper() + ":\\" + p[3:].replace("/", "\\")  # /c/... → C:\...
                            return p
        except OSError:
            pass
    # 2) 磁盘扫描标准 VS 根(仅当 vcvars.sh 缺失)。
    #    VS 版本目录命名不统一(v18 与 2022 并存,18 实际比 2022 新),不能按字典序选;
    #    因此只要求"存在 VC/Tools/MSVC(证明装了 C++ 工具集)"即采用,否则取第一个可用。
    bases = (r"C:\Program Files\Microsoft Visual Studio",
             r"C:\Program Files (x86)\Microsoft Visual Studio")
    plain = []
    for base in bases:
        for bat in glob.glob(os.path.join(base, "*", "*", "VC", "Auxiliary", "Build", "vcvars64.bat")):
            vs_root = bat[: bat.find("VC\\Auxiliary\\Build")]
            if os.path.isdir(os.path.join(vs_root, "VC", "Tools", "MSVC")):
                return bat
            plain.append(bat)
    return plain[0] if plain else ""


def _tail(path: str, n: int = 800) -> str:
    """读文件尾部 n 字节(失败回退空串),用于 vcvars 导出失败的报错展示。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = f.read()
        return data[-n:]
    except OSError:
        return ""


def is_msys_linked_python() -> bool:
    """当前 Python 进程是否加载了 MSYS2 运行时(msys-2.0.dll)。

    MSYS 链接的 python 里 subprocess 参数会被其运行时做路径转换(`/c` → `C:\\`),
    所以 cmd 开关要写 `//c` 防转换;原生 Windows python(本仓库工具链装的
    mingw-w64-x86_64-python)参数原样传递,必须写 `/c`——`//c` 会让 cmd 打开交互
    shell 等 stdin,卡死到 timeout(本机已复现交互 banner)。GetModuleHandleW 查
    DLL 是否已加载来判定;Linux 上恒 False(无 MSVC 需求)。
    """
    if not pool.on_windows():
        return False
    try:
        import ctypes
        # 句柄是 64 位指针,默认 restype=c_int 会截断致误判;显式定 c_void_p + argtypes。
        _gmw = ctypes.windll.kernel32.GetModuleHandleW
        _gmw.restype = ctypes.c_void_p
        _gmw.argtypes = [ctypes.c_wchar_p]
        return bool(_gmw("msys-2.0.dll"))
    except Exception:
        return False


def ensure_msvc_env(root: str) -> bool:
    """Windows 上把 MSVC(vcvars64)环境注入 os.environ,确保用 cl 编译。

    根因:PATH 里没有 cl(MSYS2 只有 g++),CMake 自动选 MinGW,SwiftShader 的
    __nop()(MSVC-only)直接崩。vcvars64.bat 只在 cmd 进程内改环境,因此用
    `cmd /c "<vcvars> && set"`(开关按运行时是否 MSYS 链接选 /c 或 //c,见
    is_msys_linked_python)捕获全部 KEY=VALUE 再 apply 到父进程。
    找不到 vcvars/导出失败 → 打印清晰报错返回 False(调用方停止,别静默走 MinGW)。
    """
    if not pool.on_windows():
        return True  # Linux 无 MSVC 需求
    if os.environ.get("VCINSTALLDIR") and shutil.which("cl"):
        return True  # 已在 MSVC 环境
    vcvars = find_vcvars_bat(root)
    if not vcvars:
        print("[ERROR] 未找到 vcvars64.bat。请先运行 tools/install-user-deps.sh(win-deps.sh 会定位/装 Build Tools 并写 .user-deps/vcvars.sh)。",
              file=sys.stderr)
        return False
    # 用文件重定向而非管道(capture_output)读 vcvars 的 `set` 输出:
    # cmd 的子进程链(vcvars 会拉起更多 bat)会持有 stdout 管道,capture_output 等 EOF
    # 永远等不到 → 静默卡死。文件上无 EOF 可等,不会死锁。用二进制写避免
    # Windows 文本模式换行/编码坑。
    # 卡死根因一:`//c` 在原生 Windows python 下原样进 cmd,cmd 不认该开关,打开交互
    # shell 等 stdin → 卡到 timeout,vcvars 根本没跑。开关按运行时是否 MSYS 链接
    # 选 `/c` 或 `//c`(见 is_msys_linked_python)。
    # 卡死根因二(开关改对后暴露):list2cmdline 会把命令内引号转义成 `\"`,cmd /c 剥
    # 首尾引号后留下 `\"...\"` → 执行 `\"C:\...vcvars64.bat\"` → 'not recognized'。
    # 因此不把带引号的命令塞给 cmd /c,改写临时 .cmd 包装 `call vcvars && set`,cmd
    # 只跑无空格无引号的裸文件名,不触发任何转义;batch 语法里 call + 引号是合法的。
    tmp = tempfile.gettempdir()
    env_txt = os.path.join(tmp, f"vcvars_{os.getpid()}.txt")
    bat_name = f"vcvars_{os.getpid()}.cmd"
    out = None
    print(f"[INFO] 注入 MSVC 环境(vcvars64: {vcvars}) …", flush=True)
    cmd_switch = "//c" if is_msys_linked_python() else "/c"
    try:
        with open(os.path.join(tmp, bat_name), "w", encoding="utf-8") as _b:
            _b.write("@echo off\r\n")
            _b.write(f'call "{vcvars}"\r\n')
            _b.write("set\r\n")
        with open(env_txt, "wb") as _f:
            _f.write(b"")
            _f.flush()
            out = subprocess.run(
                ["cmd", cmd_switch, bat_name],
                cwd=tmp, stdout=_f, timeout=120,
            )
    except (OSError, subprocess.TimeoutExpired) as e:
        print(f"[ERROR] 无法执行 vcvars64.bat: {e}", file=sys.stderr)
        return False
    finally:
        try:
            os.remove(os.path.join(tmp, bat_name))
        except OSError:
            pass
    if out.returncode != 0:
        print(f"[ERROR] vcvars64.bat 执行失败(rc={out.returncode}):\n{_tail(env_txt, 800)}", file=sys.stderr)
        return False
    applied = 0
    with open(env_txt, "r", encoding="utf-8", errors="replace") as _f:
        out_text = _f.read()
    for line in out_text.splitlines():
        # vcvars 的 set 输出形如 "PATH=C:\...;..."(首行可能是提示/空行,按 = 切首个)
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        if not key or any(c in key for c in " \t\r\n"):
            continue
        os.environ[key] = val.strip("\r")
        applied += 1
    if not shutil.which("cl"):
        print("[ERROR] 已导出 vcvars 环境但 PATH 里仍无 cl.exe,MSVC 工具链不可用。", file=sys.stderr)
        return False
    print(f"[INFO] MSVC 环境已注入(cl: {shutil.which('cl')}),将用 MSVC 编译", flush=True)
    return True
```

- [ ] **Step 4: 跑测试确认通过**

Run: `python3 -m unittest tools.tests.test_msvc_env -v`
Expected: 全部 PASS

- [ ] **Step 5: 改 `tools/build-deps.py` 指向新模块**

删除 `tools/build-deps.py:17-165`(`_vcvars_bat`/`_tail`/`_msys_linked`/`_ensure_msvc_env` 四个函数整段),把导入行(`tools/build-deps.py:13`)改为:

```python
from deps_lib import MINE_ROOT, cmake_driver, fetch, manifest, msvc_env, pool
```

把调用点(原 `tools/build-deps.py:255`)改为:

```python
    # Windows:池依赖必须用 MSVC(cl)编译;先注入 vcvars 环境,失败即停(绝不静默走 MinGW)
    if not msvc_env.ensure_msvc_env(MINE_ROOT):
        return 3
```

- [ ] **Step 6: 清理 `tools/tests/test_build_deps.py`,只留 `TestTopoExpand`**

把文件替换为:

```python
import importlib.util
import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)  # tools/ 便于 deps_lib 导入
_spec = importlib.util.spec_from_file_location(
    "build_deps_mod", os.path.join(_TOOLS, "build-deps.py"))
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
topo_expand = _mod.topo_expand

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


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 7: 跑全部相关测试确认通过**

Run: `python3 -m unittest tools.tests.test_build_deps tools.tests.test_msvc_env -v`
Expected: 全部 PASS(`TestTopoExpand` 5 个 + `test_msvc_env.py` 全部)

- [ ] **Step 8: Commit**

```bash
git add tools/deps_lib/msvc_env.py tools/tests/test_msvc_env.py tools/build-deps.py tools/tests/test_build_deps.py
git commit -m "refactor(tools): 抽取 MSVC vcvars 注入进 deps_lib/msvc_env.py,供 gen-projects 复用"
```

---

### Task 2: `tools/deps_lib/project_gen.py` —— 项目扫描 + 类型解析

**Files:**
- Create: `tools/deps_lib/project_gen.py`
- Create: `tools/tests/test_project_gen.py`

**Interfaces:**
- Consumes: `deps_lib.manifest._load_yaml(path: str) -> dict`(已存在,`tools/deps_lib/manifest.py:31-34`)
- Produces(供 Task 3-6 使用):
  - `deps_lib.project_gen.list_projects(root: str) -> list` —— 返回 `[(项目目录名, deps.yaml 绝对路径), ...]`,按目录名排序。
  - `deps_lib.project_gen.project_type(deps_yaml_path: str) -> str` —— 读 `type:` 字段,缺省 `"vs"`。

- [ ] **Step 1: 写失败测试**

创建 `tools/tests/test_project_gen.py`:

```python
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
from deps_lib import project_gen


def _write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


class TestListProjects(unittest.TestCase):
    def test_finds_project_dirs_with_deps_yaml(self):
        with tempfile.TemporaryDirectory() as root:
            _write(os.path.join(root, "EasyPainter", "deps.yaml"), "use: []\n")
            _write(os.path.join(root, "StickyNotes", "deps.yaml"), "use: []\n")
            found = project_gen.list_projects(root)
            self.assertEqual([name for name, _ in found], ["EasyPainter", "StickyNotes"])

    def test_excludes_tooling_and_pool_dirs(self):
        with tempfile.TemporaryDirectory() as root:
            _write(os.path.join(root, "EasyPainter", "deps.yaml"), "use: []\n")
            # third_party/deps.yaml 是全局清单(libs:),不是项目清单,必须排除
            _write(os.path.join(root, "third_party", "deps.yaml"), "libs: {}\n")
            _write(os.path.join(root, "tools", "deps.yaml"), "use: []\n")
            os.makedirs(os.path.join(root, "docs"), exist_ok=True)
            os.makedirs(os.path.join(root, ".github"), exist_ok=True)
            found = project_gen.list_projects(root)
            self.assertEqual([name for name, _ in found], ["EasyPainter"])

    def test_dir_without_deps_yaml_excluded(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "NotAProject"), exist_ok=True)
            found = project_gen.list_projects(root)
            self.assertEqual(found, [])


class TestProjectType(unittest.TestCase):
    def test_reads_declared_type(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "deps.yaml")
            _write(path, "type: as\nuse: []\n")
            self.assertEqual(project_gen.project_type(path), "as")

    def test_defaults_to_vs_when_missing(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "deps.yaml")
            _write(path, "use: [abseil-cpp]\n")
            self.assertEqual(project_gen.project_type(path), "vs")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `python3 -m unittest tools.tests.test_project_gen -v`
Expected: `ModuleNotFoundError: No module named 'deps_lib.project_gen'`

- [ ] **Step 3: 创建 `tools/deps_lib/project_gen.py`(先写扫描 + 类型解析部分)**

```python
"""按项目 deps.yaml 的 type: 字段生成对应 IDE 工程 —— 扫描 + 类型注册表。"""
from __future__ import annotations

import os

from . import manifest

# 根目录下不是"项目"的目录:工具/池/文档/CI/用户级依赖/超能力笔记/VCS
_EXCLUDE_DIRS = {"tools", "third_party", "docs", ".claude", ".github", ".user-deps", ".superpowers", ".git"}


def list_projects(root: str) -> list:
    """返回 [(项目目录名, deps.yaml 绝对路径)],排除工具/池/文档目录。"""
    out = []
    for name in sorted(os.listdir(root)):
        if name in _EXCLUDE_DIRS or name.startswith("."):
            continue
        project_dir = os.path.join(root, name)
        if not os.path.isdir(project_dir):
            continue
        deps_yaml = os.path.join(project_dir, "deps.yaml")
        if os.path.isfile(deps_yaml):
            out.append((name, deps_yaml))
    return out


def project_type(deps_yaml_path: str) -> str:
    """读 deps.yaml 的 type: 字段,缺省 'vs'。"""
    data = manifest._load_yaml(deps_yaml_path)
    return data.get("type") or "vs"
```

- [ ] **Step 4: 跑测试确认通过**

Run: `python3 -m unittest tools.tests.test_project_gen -v`
Expected: 全部 PASS

- [ ] **Step 5: Commit**

```bash
git add tools/deps_lib/project_gen.py tools/tests/test_project_gen.py
git commit -m "feat(tools): project_gen 扫描项目目录 + 解析 deps.yaml type 字段"
```

---

### Task 3: `discover_vs_generator()` —— 运行时探测 VS generator

**Files:**
- Modify: `tools/deps_lib/project_gen.py`(追加函数,`import re`/`import subprocess`)
- Modify: `tools/tests/test_project_gen.py`(追加测试)

**Interfaces:**
- Produces(供 Task 4 使用):`deps_lib.project_gen.discover_vs_generator() -> str` —— 返回 `cmake --help` 里年份最大的 "Visual Studio N YYYY" 生成器全名;找不到返回空串。

- [ ] **Step 1: 追加失败测试**

在 `tools/tests/test_project_gen.py` 顶部导入区加入 `from unittest import mock`,并追加类:

```python
class TestDiscoverVsGenerator(unittest.TestCase):
    _HELP_OUTPUT = """
Generators

The following generators are available on this platform (* marks default):
* Ninja                        = Generates build.ninja files.
  Visual Studio 16 2019        = Generates Visual Studio 2019 project files.
                                  Use -A option to specify architecture.
  Visual Studio 17 2022        = Generates Visual Studio 2022 project files.
                                  Use -A option to specify architecture.
"""

    def test_picks_newest_year(self):
        with mock.patch.object(
            project_gen.subprocess, "run",
            return_value=mock.Mock(stdout=self._HELP_OUTPUT),
        ):
            self.assertEqual(project_gen.discover_vs_generator(), "Visual Studio 17 2022")

    def test_no_vs_generator_returns_empty(self):
        with mock.patch.object(
            project_gen.subprocess, "run",
            return_value=mock.Mock(stdout="* Ninja = Generates build.ninja files.\n"),
        ):
            self.assertEqual(project_gen.discover_vs_generator(), "")

    def test_cmake_missing_returns_empty(self):
        with mock.patch.object(project_gen.subprocess, "run", side_effect=OSError("no cmake")):
            self.assertEqual(project_gen.discover_vs_generator(), "")
```

- [ ] **Step 2: 跑测试确认失败**

Run: `python3 -m unittest tools.tests.test_project_gen.TestDiscoverVsGenerator -v`
Expected: `AttributeError: module 'deps_lib.project_gen' has no attribute 'discover_vs_generator'`

- [ ] **Step 3: 实现 `discover_vs_generator()`**

在 `tools/deps_lib/project_gen.py` 顶部 `import os` 旁加 `import re` 和 `import subprocess`,并在文件末尾追加:

```python
def discover_vs_generator() -> str:
    """返回 cmake --help 里可用的最新 Visual Studio 生成器名;无则空串。"""
    try:
        out = subprocess.run(["cmake", "--help"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""
    best_year = -1
    best_name = ""
    for line in (out or "").splitlines():
        m = re.search(r"(Visual Studio \d+ (\d{4}))", line)
        if m:
            year = int(m.group(2))
            if year > best_year:
                best_year = year
                best_name = m.group(1)
    return best_name
```

- [ ] **Step 4: 跑测试确认通过**

Run: `python3 -m unittest tools.tests.test_project_gen -v`
Expected: 全部 PASS

- [ ] **Step 5: Commit**

```bash
git add tools/deps_lib/project_gen.py tools/tests/test_project_gen.py
git commit -m "feat(tools): project_gen 运行时探测最新 Visual Studio generator"
```

---

### Task 4: `_gen_vs` —— vs 生成器实现

**Files:**
- Modify: `tools/deps_lib/project_gen.py`(追加 `_gen_vs`,`from . import cmake_driver, msvc_env, pool`)
- Modify: `tools/tests/test_project_gen.py`(追加测试)

**Interfaces:**
- Consumes:
  - `deps_lib.pool.on_windows() -> bool`(`tools/deps_lib/pool.py:12`)
  - `deps_lib.msvc_env.ensure_msvc_env(root: str) -> bool`(Task 1)
  - `deps_lib.cmake_driver._built_prefixes(root: str, variant: str) -> list`(`tools/deps_lib/cmake_driver.py:14`)
  - `deps_lib.cmake_driver._stream(cmd: list, tail_lines: int = 60) -> tuple`(`tools/deps_lib/cmake_driver.py:74`,逐行透传输出 + 返回 `(ok, tail_log)`,风格同 `build_lib`)
  - `deps_lib.project_gen.discover_vs_generator() -> str`(Task 3)
- Produces(供 Task 5/6 使用):`deps_lib.project_gen._gen_vs(root: str, project: str, variant: str, generator: str | None) -> tuple` —— 返回 `(ok: bool, msg: str)`;`ok=True` 且 `msg` 以 `"跳过"` 开头表示平台/非 CMake 跳过,否则 `msg` 是生成的 `.sln` 路径;`ok=False` 时 `msg` 是失败原因。

- [ ] **Step 1: 追加失败测试**

在 `tools/tests/test_project_gen.py` 追加类:

```python
class TestGenVs(unittest.TestCase):
    def _make_project(self, root, name="EasyPainter", with_cmake=True):
        d = os.path.join(root, name)
        os.makedirs(d, exist_ok=True)
        if with_cmake:
            _write(os.path.join(d, "CMakeLists.txt"), "project(demo)\n")
        return d

    def test_skips_on_non_windows(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False):
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertTrue(ok)
            self.assertTrue(msg.startswith("跳过"))

    def test_skips_when_no_cmakelists(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True):
            self._make_project(root, with_cmake=False)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertTrue(ok)
            self.assertTrue(msg.startswith("跳过"))

    def test_fails_when_no_vs_generator_found(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value=""):
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertFalse(ok)
            self.assertIn("Visual Studio", msg)

    def test_fails_when_msvc_env_injection_fails(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=False):
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertFalse(ok)

    def test_command_shape_and_success(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes",
                                return_value=[os.path.join(root, "third_party/_install/abseil-cpp-1/release")]), \
             mock.patch.object(project_gen.cmake_driver, "_stream", return_value=(True, "")) as stream:
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertTrue(ok)
            self.assertTrue(msg.endswith("EasyPainter.sln"))
            cmd = stream.call_args[0][0]
            self.assertEqual(cmd[0], "cmake")
            self.assertIn("-G", cmd)
            self.assertIn("Visual Studio 17 2022", cmd)
            self.assertIn("-A", cmd)
            self.assertIn("x64", cmd)
            self.assertIn("-DCMAKE_CONFIGURATION_TYPES=Release", cmd)
            joined = " ".join(cmd)
            self.assertIn("-DCMAKE_PREFIX_PATH=", joined)
            self.assertIn("abseil-cpp-1", joined)
            self.assertEqual(os.environ.get("MINE_ROOT"), root)

    def test_generator_override_skips_discovery(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator") as discover, \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes", return_value=[]), \
             mock.patch.object(project_gen.cmake_driver, "_stream", return_value=(True, "")) as stream:
            self._make_project(root)
            ok, _ = project_gen._gen_vs(root, "EasyPainter", "release", "Visual Studio 16 2019")
            self.assertTrue(ok)
            discover.assert_not_called()
            self.assertIn("Visual Studio 16 2019", stream.call_args[0][0])

    def test_configure_failure_reports_tail_log(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes", return_value=[]), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(False, "CMake Error: find_package(absl) 失败")):
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertFalse(ok)
            self.assertIn("find_package(absl)", msg)
```

- [ ] **Step 2: 跑测试确认失败**

Run: `python3 -m unittest tools.tests.test_project_gen.TestGenVs -v`
Expected: `AttributeError: module 'deps_lib.project_gen' has no attribute '_gen_vs'`

- [ ] **Step 3: 实现 `_gen_vs`**

在 `tools/deps_lib/project_gen.py` 顶部导入区改为:

```python
"""按项目 deps.yaml 的 type: 字段生成对应 IDE 工程 —— 扫描 + 类型注册表。"""
from __future__ import annotations

import os
import re
import subprocess

from . import cmake_driver, manifest, msvc_env, pool
```

文件末尾追加:

```python
def _gen_vs(root: str, project: str, variant: str, generator: str | None) -> tuple:
    """Windows 上用 CMake VS generator 为 project 生成 .sln;只 configure 不编译。"""
    if not pool.on_windows():
        return True, "跳过: vs 类型仅 Windows"
    project_dir = os.path.join(root, project)
    if not os.path.isfile(os.path.join(project_dir, "CMakeLists.txt")):
        return True, "跳过: 无 CMakeLists.txt"
    gen_name = generator or discover_vs_generator()
    if not gen_name:
        return False, "未探测到可用的 Visual Studio 生成器(cmake --help 无输出);请安装 VS Build Tools"
    if not msvc_env.ensure_msvc_env(root):
        return False, "MSVC 环境注入失败(vcvars),无法 configure"
    build_dir = os.path.join(project_dir, "build", "vs")
    # EasyPainter 等靠 $ENV{MINE_ROOT} 定位池,必须在 configure 进程环境里注入
    os.environ["MINE_ROOT"] = root
    prefixes = cmake_driver._built_prefixes(root, variant)
    cmd = [
        "cmake", "-S", project_dir, "-B", build_dir,
        "-G", gen_name, "-A", "x64",
        "-DCMAKE_CONFIGURATION_TYPES=Release",
    ]
    if prefixes:
        cmd.append("-DCMAKE_PREFIX_PATH=" + ";".join(prefixes))
    print(f"---- configure {project} (vs): {' '.join(cmd)}", flush=True)
    ok, tail = cmake_driver._stream(cmd)
    if not ok:
        return False, f"configure 失败:\n{tail}"
    return True, os.path.join(build_dir, f"{project}.sln")
```

- [ ] **Step 4: 跑测试确认通过**

Run: `python3 -m unittest tools.tests.test_project_gen -v`
Expected: 全部 PASS

- [ ] **Step 5: Commit**

```bash
git add tools/deps_lib/project_gen.py tools/tests/test_project_gen.py
git commit -m "feat(tools): project_gen._gen_vs 用 CMake VS generator 生成 .sln"
```

---

### Task 5: `_gen_as` 占位 + `GENERATORS` 注册表 + `generate()` 分发

**Files:**
- Modify: `tools/deps_lib/project_gen.py`(追加 `_gen_as`/`GENERATORS`/`generate`)
- Modify: `tools/tests/test_project_gen.py`(追加测试)

**Interfaces:**
- Produces(供 Task 6 使用):
  - `deps_lib.project_gen.GENERATORS: dict[str, callable]` —— `{"vs": _gen_vs, "as": _gen_as}`。
  - `deps_lib.project_gen.generate(root: str, project: str, type_name: str, variant: str, generator: str | None) -> tuple` —— 返回 `(ok, msg)`;`type_name` 未注册时 `ok=False` 且 `msg` 形如 `"未知项目类型: xxx"`;`as` 占位固定返回 `ok=False` 且 `msg` 以 `"未实现"` 开头(CLI 侧据此归入 `TODO` 桶,不计入失败退出码——见 Task 6)。

- [ ] **Step 1: 追加失败测试**

```python
class TestGenAsPlaceholder(unittest.TestCase):
    def test_returns_not_implemented(self):
        ok, msg = project_gen._gen_as("/root", "SomeAndroidApp", "release", None)
        self.assertFalse(ok)
        self.assertTrue(msg.startswith("未实现"))


class TestGenerateDispatch(unittest.TestCase):
    def test_unknown_type_fails(self):
        ok, msg = project_gen.generate("/root", "X", "bogus", "release", None)
        self.assertFalse(ok)
        self.assertIn("bogus", msg)

    def test_dispatches_to_registered_generator(self):
        with mock.patch.dict(project_gen.GENERATORS, {"vs": mock.Mock(return_value=(True, "ok"))}):
            ok, msg = project_gen.generate("/root", "EasyPainter", "vs", "release", None)
            self.assertTrue(ok)
            self.assertEqual(msg, "ok")
            project_gen.GENERATORS["vs"].assert_called_once_with("/root", "EasyPainter", "release", None)

    def test_as_is_registered_but_not_implemented(self):
        ok, msg = project_gen.generate("/root", "SomeAndroidApp", "as", "release", None)
        self.assertFalse(ok)
        self.assertTrue(msg.startswith("未实现"))
```

- [ ] **Step 2: 跑测试确认失败**

Run: `python3 -m unittest tools.tests.test_project_gen.TestGenAsPlaceholder tools.tests.test_project_gen.TestGenerateDispatch -v`
Expected: `AttributeError: module 'deps_lib.project_gen' has no attribute '_gen_as'`

- [ ] **Step 3: 实现 `_gen_as` / `GENERATORS` / `generate`**

在 `tools/deps_lib/project_gen.py` 文件末尾追加:

```python
def _gen_as(root: str, project: str, variant: str, generator: str | None) -> tuple:
    """as(Android Studio)生成器占位:已登记类型,真实 Android 工程出现前不实现。"""
    return False, "未实现: as(Android Studio)生成器待有真实 Android 工程后实现"


GENERATORS = {
    "vs": _gen_vs,
    "as": _gen_as,
}


def generate(root: str, project: str, type_name: str, variant: str, generator: str | None) -> tuple:
    """按 type_name 分派到对应生成器;未知类型直接失败。"""
    fn = GENERATORS.get(type_name)
    if fn is None:
        return False, f"未知项目类型: {type_name}"
    return fn(root, project, variant, generator)
```

- [ ] **Step 4: 跑测试确认通过**

Run: `python3 -m unittest tools.tests.test_project_gen -v`
Expected: 全部 PASS

- [ ] **Step 5: Commit**

```bash
git add tools/deps_lib/project_gen.py tools/tests/test_project_gen.py
git commit -m "feat(tools): project_gen 加 as 占位生成器 + GENERATORS 注册表 + generate() 分发"
```

---

### Task 6: `tools/gen-projects.py` CLI 工具

**Files:**
- Create: `tools/gen-projects.py`

**Interfaces:**
- Consumes:
  - `deps_lib.project_gen.list_projects(root) -> list`(Task 2)
  - `deps_lib.project_gen.project_type(deps_yaml_path) -> str`(Task 2)
  - `deps_lib.project_gen.generate(root, project, type_name, variant, generator) -> tuple`(Task 5)
  - `deps_lib.cmake_driver._make_output_safe() -> None`(`tools/deps_lib/cmake_driver.py:60`,Windows GBK 控制台防崩)
- Produces:CLI `python3 tools/gen-projects.py --all|--project <dir> [--variant release|debug] [--generator <名>]`;`main(argv=None) -> int`,`0` 全部非失败、`1` 有 `FAILED` 项。

无独立单元测试文件——`main()` 只做参数解析 + 编排,核心逻辑已在 Task 2-5 的 `project_gen` 测试里覆盖(与 `build-deps.py` 的 `main()` 同例,不额外测 CLI 层)。本任务用真实命令跑一遍作为验证步骤(Step 3)。

- [ ] **Step 1: 创建 `tools/gen-projects.py`**

```python
#!/usr/bin/env python3
"""按项目 deps.yaml 的 type: 字段生成对应 IDE 工程(当前仅 vs → .sln)。"""
from __future__ import annotations

import argparse
import os
import sys

from deps_lib import MINE_ROOT, cmake_driver, project_gen


def _run(root: str, projects: list, variant: str, generator: str | None) -> int:
    summary = {"generated": [], "skipped": [], "todo": [], "failed": []}
    for name, deps_yaml in projects:
        type_name = project_gen.project_type(deps_yaml)
        ok, msg = project_gen.generate(root, name, type_name, variant, generator)
        if ok and msg.startswith("跳过"):
            summary["skipped"].append(f"{name} ({type_name}) {msg}")
        elif ok:
            summary["generated"].append(f"{name} ({type_name}) → {msg}")
        elif msg.startswith("未实现"):
            summary["todo"].append(f"{name} ({type_name}) {msg}")
        else:
            summary["failed"].append(f"{name} ({type_name}) {msg}")
            print(f"  失败: {msg}", file=sys.stderr, flush=True)

    for key, label in (("generated", "GENERATED"), ("skipped", "SKIPPED"),
                        ("todo", "TODO"), ("failed", "FAILED")):
        for item in summary[key]:
            print(f"[{label}] {item}", flush=True)
    print(
        f"汇总: 生成 {len(summary['generated'])} / 跳过 {len(summary['skipped'])} / "
        f"未实现 {len(summary['todo'])} / 失败 {len(summary['failed'])}",
        flush=True,
    )
    return 1 if summary["failed"] else 0


def main(argv=None) -> int:
    cmake_driver._make_output_safe()
    p = argparse.ArgumentParser(description="按项目类型生成 IDE 工程")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--project", metavar="DIR")
    g.add_argument("--all", action="store_true")
    p.add_argument("--variant", default="release", help="指向池的哪个变体(默认 release)")
    p.add_argument("--generator", default=None, help="覆盖 VS generator 名(调试用)")
    args = p.parse_args(argv)

    if args.project:
        deps_yaml = os.path.join(MINE_ROOT, args.project, "deps.yaml")
        if not os.path.isfile(deps_yaml):
            p.error(f"项目清单不存在: {deps_yaml}")
        projects = [(args.project, deps_yaml)]
    else:
        projects = project_gen.list_projects(MINE_ROOT)
        if not projects:
            print("未发现任何项目。")
            return 0

    return _run(MINE_ROOT, projects, args.variant, args.generator)


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: 语法/导入自检**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -c "import sys; sys.path.insert(0, 'tools'); import importlib.util; spec = importlib.util.spec_from_file_location('m', 'tools/gen-projects.py'); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); print('ok')"`
Expected: 打印 `ok`,无异常

- [ ] **Step 3: 真实跑一遍(Linux 上验证 SKIPPED 路径)**

Run: `python3 tools/gen-projects.py --all`
Expected: 输出两行 `[SKIPPED] EasyPainter (vs) 跳过: vs 类型仅 Windows` / `[SKIPPED] StickyNotes (vs) 跳过: vs 类型仅 Windows`(此时两项目 `deps.yaml` 还没加 `type: vs`,会先按缺省 `vs` 处理,同样落到 SKIPPED),末尾 `汇总: ... 失败 0`,退出码 `0`。

> 注:此步骤在 Task 7 补 `type: vs` 字段之前跑,验证的是"缺省按 vs 处理"路径;Task 7 之后重跑一次验证"显式声明 vs"路径,行为应一致。

- [ ] **Step 4: Commit**

```bash
git add tools/gen-projects.py
git commit -m "feat(tools): 新增 gen-projects.py CLI,按项目类型生成 IDE 工程"
```

---

### Task 7: 项目侧改动 —— EasyPainter `_variant` 兜底 + 两项目 `type: vs`

**Files:**
- Modify: `EasyPainter/CMakeLists.txt:13-16`
- Modify: `EasyPainter/deps.yaml`
- Modify: `StickyNotes/deps.yaml`
- Modify: `tools/tests/test_project_gen.py`(追加真实仓库回归测试)

**Interfaces:**
- Consumes: `deps_lib.project_gen.project_type(deps_yaml_path) -> str`(Task 2)

- [ ] **Step 1: 追加失败测试(锁定两个真实项目声明 `type: vs`)**

在 `tools/tests/test_project_gen.py` 追加:

```python
class TestRealProjectsDeclareVsType(unittest.TestCase):
    def test_easypainter_and_stickynotes_declare_vs(self):
        mine_root = os.path.dirname(_TOOLS)  # tools/tests/../.. = tools/,再上一层是 Mine 根
        mine_root = os.path.dirname(mine_root)
        for name in ("EasyPainter", "StickyNotes"):
            deps_yaml = os.path.join(mine_root, name, "deps.yaml")
            self.assertTrue(os.path.isfile(deps_yaml), f"{deps_yaml} 不存在")
            self.assertEqual(project_gen.project_type(deps_yaml), "vs")
```

同时在文件顶部导入区补上 `_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))`(与 `test_build_deps.py`/`test_new_project.py` 一致的写法),紧跟在 `sys.path.insert(...)` 那行之前定义好复用。

- [ ] **Step 2: 跑测试确认失败**

Run: `python3 -m unittest tools.tests.test_project_gen.TestRealProjectsDeclareVsType -v`
Expected: `AssertionError: 'vs' != 'vs'` 不会发生;实际会是 `project_type` 返回缺省 `'vs'`(因为字段还没显式加)——**这条测试此刻会通过而非失败**,因为 `project_type` 缺省即 `vs`。为让它真正驱动"必须显式声明"这件事,改成先断言字段存在:

```python
class TestRealProjectsDeclareVsType(unittest.TestCase):
    def test_easypainter_and_stickynotes_declare_vs(self):
        mine_root = os.path.dirname(os.path.dirname(_TOOLS))
        for name in ("EasyPainter", "StickyNotes"):
            deps_path = os.path.join(mine_root, name, "deps.yaml")
            self.assertTrue(os.path.isfile(deps_path), f"{deps_path} 不存在")
            with open(deps_path, encoding="utf-8") as f:
                content = f.read()
            self.assertIn("type: vs", content, f"{deps_path} 缺少显式 type: vs 声明")
```

Run: `python3 -m unittest tools.tests.test_project_gen.TestRealProjectsDeclareVsType -v`
Expected: `AssertionError: 'type: vs' not found in ...`(两个 `deps.yaml` 目前都没有这行)

- [ ] **Step 3: EasyPainter/deps.yaml 加 `type: vs`**

当前内容:
```yaml
use: [abseil-cpp, ink-stroke-modeler, glfw, glm, googletest, swiftshader]
```
改为:
```yaml
type: vs
use: [abseil-cpp, ink-stroke-modeler, glfw, glm, googletest, swiftshader]
```

- [ ] **Step 4: StickyNotes/deps.yaml 加 `type: vs`**

当前内容:
```yaml
use: []
# StickyNotes 无第三方 C++ 依赖：Qt 6.4.2 经 tools/install-user-deps.sh 部署到 .user-deps，
# Noto CJK 字体来自 EasyPainter/assets/fonts。
```
改为:
```yaml
type: vs
use: []
# StickyNotes 无第三方 C++ 依赖：Qt 6.4.2 经 tools/install-user-deps.sh 部署到 .user-deps，
# Noto CJK 字体来自 EasyPainter/assets/fonts。
```

- [ ] **Step 5: 修 `EasyPainter/CMakeLists.txt` 的 `_variant` 空值兜底**

`EasyPainter/CMakeLists.txt:13-16` 现状:
```cmake
# 变体解析:CMAKE_BUILD_TYPE(Release/Debug)→ 池产物子目录(release/debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})
```
改为:
```cmake
# 变体解析:CMAKE_BUILD_TYPE(Release/Debug)→ 池产物子目录(release/debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
if(NOT _variant)
  set(_variant "release")          # 多配置生成器(VS)下 CMAKE_BUILD_TYPE 恒空
endif()
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})
```

- [ ] **Step 6: 跑测试确认通过 + 全量回归**

Run: `python3 -m unittest discover -s tools/tests -v`
Expected: 全部 PASS(含新增的 `TestRealProjectsDeclareVsType`)

- [ ] **Step 7: 重跑一遍 gen-projects.py,确认显式 `type: vs` 路径行为一致**

Run: `python3 tools/gen-projects.py --all`
Expected: 与 Task 6 Step 3 相同的两行 `[SKIPPED] ... (vs) 跳过: vs 类型仅 Windows`,`汇总: ... 失败 0`,退出码 `0`

- [ ] **Step 8: Commit**

```bash
git add EasyPainter/CMakeLists.txt EasyPainter/deps.yaml StickyNotes/deps.yaml tools/tests/test_project_gen.py
git commit -m "fix(EasyPainter): 多配置生成器下 _variant 兜底 release;两项目声明 type: vs"
```

> **备注(留给 Windows 端集成验证,非本任务自动化范围)**:本任务在 Linux 上只能验证 `_variant` 兜底改动不破坏现有 Ninja 构建(靠 CI/本地 `--variant` 相关的既有测试 + 手工 `cmake --preset release` 检查 configure 不报错)。真正验证 `.sln` 能在 VS 里编译链接池内库,需按 spec §10"集成验证"在 Windows 机器上跑 `setup-env.sh` 后打开 `EasyPainter/build/vs/EasyPainter.sln`。

---

### Task 8: `new-project.py` 支持 `--type vs|as`

**Files:**
- Modify: `tools/new-project.py:62-92`
- Modify: `tools/templates/cpp/deps.yaml.tmpl`
- Modify: `tools/tests/test_new_project.py`

**Interfaces:**
- Consumes: `render_template(src, dst, ctx) -> None`(已存在,`tools/new-project.py:17-47`)
- Produces: CLI 新增 `--type vs|as`(默认 `vs`,`argparse choices` 校验;仅 `cpp` 生效,`python`/`web` 忽略该参数值但仍接受该 flag)。

- [ ] **Step 1: 写失败测试**

在 `tools/tests/test_new_project.py` 顶部追加 `import shutil` 和 `from unittest import mock`(与现有 `import unittest` 等并列),文件末尾(`if __name__ ==` 之前)追加:

```python
class TestTypeFlag(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        os.makedirs(os.path.join(self.root, "third_party"), exist_ok=True)
        with open(os.path.join(self.root, "third_party", "deps.yaml"), "w", encoding="utf-8") as f:
            f.write("libs: {}\n")
        # 复用真实模板渲染,不重复维护模板内容
        shutil.copytree(
            os.path.join(_TOOLS, "templates"),
            os.path.join(self.root, "tools", "templates"),
        )

    def test_explicit_type_written_into_deps_yaml(self):
        with mock.patch.object(np_mod, "MINE_ROOT", self.root):
            rc = np_mod.main(["cpp", "demo", "--type", "as"])
        self.assertEqual(rc, 0)
        with open(os.path.join(self.root, "demo", "deps.yaml"), encoding="utf-8") as f:
            self.assertIn("type: as", f.read())

    def test_default_type_is_vs(self):
        with mock.patch.object(np_mod, "MINE_ROOT", self.root):
            rc = np_mod.main(["cpp", "demo2"])
        self.assertEqual(rc, 0)
        with open(os.path.join(self.root, "demo2", "deps.yaml"), encoding="utf-8") as f:
            self.assertIn("type: vs", f.read())

    def test_invalid_type_exits_nonzero(self):
        with self.assertRaises(SystemExit) as cm:
            np_mod.main(["cpp", "demo3", "--type", "bogus"])
        self.assertNotEqual(cm.exception.code, 0)
```

- [ ] **Step 2: 跑测试确认失败**

Run: `python3 -m unittest tools.tests.test_new_project.TestTypeFlag -v`
Expected: `test_explicit_type_written_into_deps_yaml`/`test_default_type_is_vs` 失败于 `AssertionError: 'type: as' not found ...`(`ctx`/argparse 还没有 `--type`);`test_invalid_type_exits_nonzero` 也失败,因为当前 argparse 没有 `--type` 参数,`parse_args` 会报 `unrecognized arguments` 但那本身也是 `SystemExit`——先跑一遍确认前两个是真失败即可。

- [ ] **Step 3: 加 `--type` 参数 + 写入 ctx**

`tools/new-project.py:62-66` 现状:
```python
    p = argparse.ArgumentParser(description="新建项目脚手架")
    p.add_argument("lang", choices=sorted(LANGS), help="项目类型")
    p.add_argument("name", help="项目名(目录名)")
    p.add_argument("--libs", default="", help="逗号分隔库名,写入 deps.yaml use(默认空)")
    args = p.parse_args(argv)
```
改为:
```python
    p = argparse.ArgumentParser(description="新建项目脚手架")
    p.add_argument("lang", choices=sorted(LANGS), help="项目类型")
    p.add_argument("name", help="项目名(目录名)")
    p.add_argument("--libs", default="", help="逗号分隔库名,写入 deps.yaml use(默认空)")
    p.add_argument("--type", default="vs", choices=("vs", "as"),
                   help="IDE 工程类型,仅 cpp 生效(默认 vs)")
    args = p.parse_args(argv)
```

`tools/new-project.py:87-92` 现状:
```python
    ctx = {
        "PROJECT_NAME": args.name,
        "DEPS": ", ".join(use),
        "DEPS_FIND": find_frag,
        "DEPS_LINK": f"target_link_libraries({args.name} PRIVATE {link_frag})" if use else "",
    }
```
改为:
```python
    ctx = {
        "PROJECT_NAME": args.name,
        "DEPS": ", ".join(use),
        "DEPS_FIND": find_frag,
        "DEPS_LINK": f"target_link_libraries({args.name} PRIVATE {link_frag})" if use else "",
        "TYPE": args.type,
    }
```

（`--type` 取值已由 `argparse choices=("vs", "as")` 校验,非法值 `parse_args` 自动报错退出,无需手写校验逻辑;`python`/`web` 模板不含 `{{TYPE}}` 占位符,`ctx["TYPE"]` 对它们是死键,不产生任何文件内容变化。）

- [ ] **Step 4: `tools/templates/cpp/deps.yaml.tmpl` 加 `type:` 行**

当前内容:
```
use: [{{DEPS}}]
```
改为:
```
type: {{TYPE}}
use: [{{DEPS}}]
```

- [ ] **Step 5: 跑测试确认通过**

Run: `python3 -m unittest tools.tests.test_new_project -v`
Expected: 全部 PASS

- [ ] **Step 6: Commit**

```bash
git add tools/new-project.py tools/templates/cpp/deps.yaml.tmpl tools/tests/test_new_project.py
git commit -m "feat(tools): new-project.py 支持 --type vs|as,写进生成的 deps.yaml"
```

---

### Task 9: `setup-env.sh` 接线 —— 池就绪后自动生成 IDE 工程

**Files:**
- Modify: `tools/setup-env.sh:168-170`

**Interfaces:**
- Consumes: `tools/gen-projects.py --all`(Task 6 CLI,退出码 `0`/`1`)

- [ ] **Step 1: 定位插入点并确认现状**

`tools/setup-env.sh:161-172` 现状:
```bash
  if ! pool_built; then
    info "=== 拉取三方库源码 ==="
    python3 -u "$MINE_ROOT/tools/fetch-deps.py" --all
    info "=== 预编译三方库进池 ==="
    python3 -u "$MINE_ROOT/tools/build-deps.py" --all
  else
    info "=== 三方库池已就绪,跳过 fetch/build ==="
  fi

  # 4) 最终校验
  info "=== 最终校验 ==="
  probe
```

- [ ] **Step 2: 插入 gen-projects.py 调用(必须在 `fi` 之后、无条件执行)**

改为:
```bash
  if ! pool_built; then
    info "=== 拉取三方库源码 ==="
    python3 -u "$MINE_ROOT/tools/fetch-deps.py" --all
    info "=== 预编译三方库进池 ==="
    python3 -u "$MINE_ROOT/tools/build-deps.py" --all
  else
    info "=== 三方库池已就绪,跳过 fetch/build ==="
  fi

  info "=== 生成 IDE 工程(vs → .sln)==="
  python3 -u "$MINE_ROOT/tools/gen-projects.py" --all

  # 4) 最终校验
  info "=== 最终校验 ==="
  probe
```

- [ ] **Step 3: Bash 语法检查**

Run: `bash -n tools/setup-env.sh`
Expected: 无输出(exit 0)

- [ ] **Step 4: 确认插入位置在 `pool_built` if/else 块之后、`probe` 之前**

Run: `grep -n "pool_built\|gen-projects.py\|probe$" tools/setup-env.sh`
Expected: 看到 `gen-projects.py --all` 那行的行号,在 `if ! pool_built` 对应 `fi` 之后、`probe` 调用之前

- [ ] **Step 5: Commit**

```bash
git add tools/setup-env.sh
git commit -m "feat(tools): setup-env.sh 池就绪后自动跑 gen-projects.py --all 生成 IDE 工程"
```

> **备注(留给 Windows 端集成验证,非本任务自动化范围)**:本任务只能在 Linux 上做语法/位置检查。完整验证("池已就绪时跳过 fetch/build 分支也会走到 gen-projects"、`.sln` 实际生成)需按 spec §10 在 Windows 机器上完整跑一遍 `tools/setup-env.sh`。

---

## Self-Review 记录

- **Spec 覆盖**:§2 决策表(类型声明位置/取值/触发时机)→ Task 2/7/9;§3 类型模型 → Task 2/5;§4 CLI/扫描 → Task 2/6;§5 vs 生成器(平台门控/生成命令/generator 探测/幂等失败隔离)→ Task 3/4;§6 项目侧改动 → Task 7;§7 new-project.py → Task 8;§8 setup-env.sh → Task 9;§9 msvc_env 复用重构 → Task 1;§10 单测覆盖点(scan/type/vs 命令构造/as 占位/非 CMake 跳过/discover_vs_generator/new-project --type/msvc_env 搬移)→ 逐一对应 Task 2-5、Task 8、Task 1 的测试;§10 集成验证(Linux 全绿 + gen-projects 跳过、Windows .sln 生成)→ Task 6 Step 3、Task 7 Step 7、Task 9 备注。无遗漏项。
- **占位符扫描**:全文无 "TBD/implement later/add appropriate handling" 等占位;所有步骤含可直接运行的完整代码。
- **类型一致性**:`_gen_vs`/`_gen_as` 签名 `(root, project, variant, generator)` 在 Task 4/5/`GENERATORS`/`generate()` 中保持一致;`ensure_msvc_env(root)`/`find_vcvars_bat(root)`/`is_msys_linked_python()` 在 Task 1 定义、Task 4 消费,签名一致;`list_projects`/`project_type` 在 Task 2 定义、Task 6/7 消费,签名一致。
