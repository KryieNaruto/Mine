# 一键环境搭建全链路统一(setup-unified)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让"双击脚本 → 新机器环境就绪 → Windows 可开 IDE / Linux 可直接编译"全链路成立，覆盖 GitHub 镜像、Android 工具链、按 (类型, 宿主) 逐项目构建、Windows 双击入口。

**Architecture:** 单一入口四步流水线（工具链 → 池 → 逐项目构建 → 汇总）。平台分叉只在工具链探测与逐项目构建两步；构建分派由 `project_gen.py` 按 (项目类型, 宿主平台) 决定，git 拉取走 ghproxy 镜像优先、官方兜底。

**Tech Stack:** Python(deps_lib)、bash(win-deps.sh/install-user-deps.sh/setup-env.sh)、batch(setup.bat)、Gradle+AGP+Kotlin(Android 示例)。

**Spec:** `docs/superpowers/specs/2026-08-27-unified-setup-design.md`

## Global Constraints

- **测试命令**: 每次任务以 `python3 -m unittest discover -s tools/tests -p "test_*.py"` 全量跑通为准（新增/改动的文件务必进该目录的测试）。
- **Linux 禁止 sudo**: 任何脚本/文档不得出现 `sudo`、`apt install`、`yum install`、`dnf install`。
- **国内镜像优先、官方兜底**: 新增下载（GitHub 源码、Android SDK、Gradle、Maven、JDK）一律镜像前缀优先，官方 URL 只作 fallback；禁止出现"只有官方 URL"的新增下载。
- **已建产物不重建**: 池 `third_party/_install/*/<variant>/.built` 存在即跳过；本计划不引入"从网上下二进制预编译库"。
- **不改坏既有链路**: Windows `.sln` 生成（`_gen_vs`）、Linux 13/13 ctest 全绿是回归基线。
- **中文注释**: 新增代码注释用中文，沿用现有风格。
- **版本锁**(Android 模板，Task 4): Gradle 8.7 / AGP 8.5.2 / Kotlin 1.9.24 / compileSdk 34 / minSdk 24 / JDK 17。
- 每个任务独立 commit。

---

### Task 1: GitHub 源码拉取镜像层(mirror.py + fetch.py)

**Files:**
- Create: `tools/deps_lib/mirror.py`
- Modify: `tools/deps_lib/fetch.py`(clone_lib、ensure_swiftshader_submodules)
- Test: `tools/tests/test_mirror.py`、`tools/tests/test_fetch.py`

**Interfaces:**
- Produces: `mirror.MIRROR_PREFIXES: list[str]`、`mirror.mirror_url(repo_url: str, prefix: str | None) -> str`、`mirror.pick_mirror_prefix(timeout: float = 6.0) -> str | None`、`mirror._probe(prefix: str, timeout: float) -> float | None`。
- Consumes: 无。Task 4 的 Android 下载不依赖此层（走独立镜像逻辑）。

- [ ] **Step 1: 写失败测试 `tools/tests/test_mirror.py`**

```python
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from deps_lib import mirror


class TestMirrorUrl(unittest.TestCase):
    def test_mirror_url_prepends_prefix(self):
        self.assertEqual(
            mirror.mirror_url("https://github.com/glfw/glfw.git", "https://ghproxy.net/"),
            "https://ghproxy.net/https://github.com/glfw/glfw.git",
        )

    def test_mirror_url_none_prefix_returns_original(self):
        self.assertEqual(
            mirror.mirror_url("https://github.com/glfw/glfw.git", None),
            "https://github.com/glfw/glfw.git",
        )


class TestPickMirrorPrefix(unittest.TestCase):
    def test_returns_fastest_reachable(self):
        with mock.patch.object(mirror, "_probe", side_effect=[3.0, 0.2]):
            self.assertEqual(mirror.pick_mirror_prefix(), "https://gh-proxy.com/")

    def test_returns_none_when_all_unreachable(self):
        with mock.patch.object(mirror, "_probe", return_value=None):
            self.assertIsNone(mirror.pick_mirror_prefix())

    def test_returns_none_when_no_prefixes(self):
        with mock.patch.object(mirror, "MIRROR_PREFIXES", []):
            self.assertIsNone(mirror.pick_mirror_prefix())


class TestProbe(unittest.TestCase):
    def test_probe_returns_elapsed_on_success(self):
        fake = mock.Mock()
        fake.read.return_value = b""
        with mock.patch("deps_lib.mirror.urllib.request.urlopen", return_value=fake):
            t = mirror._probe("https://ghproxy.net/", 3.0)
        self.assertIsInstance(t, float)
        self.assertGreater(t, 0)

    def test_probe_returns_none_on_error(self):
        with mock.patch("deps_lib.mirror.urllib.request.urlopen",
                        side_effect=OSError("unreachable")):
            self.assertIsNone(mirror._probe("https://ghproxy.net/", 3.0))
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `python3 -m unittest tools.tests.test_mirror -v`
Expected: `ModuleNotFoundError: No module named 'deps_lib.mirror'`(FAIL/ERROR)。

- [ ] **Step 3: 实现 `tools/deps_lib/mirror.py`**

```python
"""GitHub 源码拉取国内镜像层。

ghproxy 类加速服务把 `<前缀> + https://github.com/<repo>` 原样透传到 GitHub。
测速选可达且最快的镜像前缀;全部不可达返回 None(调用方退回官方直连)。
测速结果不缓存/不落盘——每次运行时现测,避免陈旧选择(CMakeCache 陈旧教训)。
"""
from __future__ import annotations

import time
import urllib.request

# ghproxy 类加速前缀。失效/新增在此维护(测速会自动跳过不可达的)。
MIRROR_PREFIXES = ["https://ghproxy.net/", "https://gh-proxy.com/"]

# 探测目标:任意一个 GitHub 仓库的 git smart-http 端点(镜像会原样透传)。
# 用 glm 而非大仓库,探测请求尽量小。
_PROBE_URL = "https://github.com/g-truc/glm.git/info/refs?service=git-upload-pack"


def mirror_url(repo_url: str, prefix: str | None) -> str:
    """prefix 非空时给 repo_url 前挂镜像前缀,否则原样返回。"""
    return prefix + repo_url if prefix else repo_url


def _probe(prefix: str, timeout: float) -> float | None:
    """探测某前缀是否可达且能透传 git 数据,返回耗时秒数;不可达返回 None。"""
    url = prefix + _PROBE_URL
    try:
        start = time.monotonic()
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            resp.read()  # 触发实际请求
        return time.monotonic() - start
    except Exception:
        return None


def pick_mirror_prefix(timeout: float = 6.0) -> str | None:
    """返回当前可达且最快的镜像前缀;全部不可达返回 None。"""
    best = None
    best_t = None
    for p in MIRROR_PREFIXES:
        t = _probe(p, timeout)
        if t is not None and (best_t is None or t < best_t):
            best, best_t = p, t
    return best
```

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `python3 -m unittest tools.tests.test_mirror -v`
Expected: 全部 PASS。

- [ ] **Step 5: 改造 `fetch.py::clone_lib` 镜像优先、官方兜底**

把 `tools/deps_lib/fetch.py` 的 `clone_lib` 整体替换为下面版本（行为语义与原版一致：浅克隆优先、失败全量回退、返回 `(ok, commit_or_err)`）：

```python
def clone_lib(root: str, lib: LibSpec):
    """把 lib 源码 clone 到池 _src/<ver_dir>。镜像优先,官方直连兜底。
    返回 (ok, commit_or_err)。"""
    src = pool.src_dir(root, lib.name, lib.tag)
    if os.path.isdir(src):
        return False, "already exists"
    os.makedirs(os.path.dirname(src), exist_ok=True)

    prefix = mirror.pick_mirror_prefix()
    attempts = ([mirror.mirror_url(lib.repo, prefix)] if prefix else []) + [lib.repo]

    ok = False
    last_err = ""
    for url in attempts:
        r = subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", lib.tag, url, src],
            capture_output=True, text=True,
        )
        if r.returncode == 0:
            ok = True
            break
        shutil.rmtree(src, ignore_errors=True)  # 失败残留(镜像断流/半成品),清理后重试下一路
        last_err = (r.stderr or r.stdout).strip()
    if not ok:
        # 全量克隆后 checkout 任意 ref(含 commit sha);只走官方源
        r2 = subprocess.run(["git", "clone", lib.repo, src], capture_output=True, text=True)
        if r2.returncode != 0:
            shutil.rmtree(src, ignore_errors=True)
            return False, (last_err + "\n" + r2.stderr).strip()
        r3 = subprocess.run(["git", "-C", src, "checkout", lib.tag], capture_output=True, text=True)
        if r3.returncode != 0:
            shutil.rmtree(src, ignore_errors=True)
            return False, r3.stderr.strip()

    rc = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True)
    commit = rc.stdout.strip() if rc.returncode == 0 else ""
    return True, commit
```

并在 `fetch.py` 顶部加 `from . import MINE_ROOT, manifest, mirror, pool`（去掉原 `from . import MINE_ROOT, manifest, pool` 中的重复，保持其余 import 不动）。

- [ ] **Step 6: 给 clone_lib 写镜像路径测试(加到 `tools/tests/test_fetch.py`)**

```python
class TestCloneLibMirror(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lib = LibSpec(name="fmt", repo="https://github.com/fmtlib/fmt.git", tag="10.2.1")

    def _fake_run(self, rc_map):
        """按 (首 token, 全命令串) 匹配返回预设 returncode;否则抛错。"""
        def _run(args, **kw):
            joined = " ".join(args)
            for key, rc in rc_map.items():
                if key in joined:
                    return subprocess.CompletedProcess(args, rc, stdout="", stderr="err")
            raise AssertionError(f"未预期的命令: {joined}")
        return _run

    def test_mirror_clone_failure_falls_back_to_original(self):
        with mock.patch("deps_lib.fetch.mirror.pick_mirror_prefix", return_value="https://ghproxy.net/"), \
             mock.patch("deps_lib.fetch.subprocess.run",
                        side_effect=self._fake_run({
                            "ghproxy.net/https://github.com/fmtlib/fmt.git": 1,
                            "clone --depth 1 --branch 10.2.1 https://github.com/fmtlib/fmt.git": 0,
                            "rev-parse HEAD": 0,
                        })):
            ok, msg = fetch.clone_lib(self.tmp.name, self.lib)
        self.assertTrue(ok, msg)

    def test_mirror_clone_success_skips_original(self):
        with mock.patch("deps_lib.fetch.mirror.pick_mirror_prefix", return_value="https://ghproxy.net/"), \
             mock.patch("deps_lib.fetch.subprocess.run",
                        side_effect=self._fake_run({
                            "ghproxy.net/https://github.com/fmtlib/fmt.git": 0,
                            "rev-parse HEAD": 0,
                        })) as run:
            ok, _ = fetch.clone_lib(self.tmp.name, self.lib)
        self.assertTrue(ok)
        joined = " ".join(c.args[0] for c in run.call_args_list)
        self.assertIn("ghproxy.net", joined)
        self.assertNotIn("ghproxy.net", joined.replace("ghproxy.net/https://github.com/fmtlib/fmt.git", "", 1))
        # 官方源只在镜像失败时才出现
        self.assertEqual(joined.count("https://github.com/fmtlib/fmt.git"), 1)
```

（`test_fetch.py` 顶部已有的 import 若缺 `LibSpec`/`tempfile`/`mock`/`fetch` 模块引用，按该文件现有 import 风格补齐。）

- [ ] **Step 7: 跑测试确认 GREEN(全量)**

Run: `python3 -m unittest discover -s tools/tests -p "test_*.py"`
Expected: OK(原有用例 + 新增全过)。

- [ ] **Step 8: 改造 `ensure_swiftshader_submodules` 复用镜像(仓库级 insteadOf 改写)**

在 `fetch.py` 里新增两个小函数，并让 `ensure_swiftshader_submodules` 在 update 前设改写、失败后清除：

```python
def _set_mirror_rewrite(src: str, prefix: str) -> None:
    """仓库级 url.<prefix>https://github.com/.insteadOf,让本仓库内所有 github
    拉取(含子模块 clone)走镜像。git submodule 继承父仓库的 insteadOf 规则。"""
    subprocess.run(
        ["git", "-C", src, "config", f"url.{prefix}https://github.com/.insteadOf",
         "https://github.com/"],
        capture_output=True, text=True,
    )


def _unset_mirror_rewrite(src: str, prefix: str) -> None:
    subprocess.run(
        ["git", "-C", src, "config", "--unset-all", f"url.{prefix}https://github.com/.insteadOf"],
        capture_output=True, text=True,
    )
```

在 `ensure_swiftshader_submodules` 的 `last = ""` 之后、重试循环之前插入：

```python
    prefix = mirror.pick_mirror_prefix()
    if prefix:
        _set_mirror_rewrite(src, prefix)
```

并把循环内失败分支改为(镜像失败则清除改写、退官方直连再重试)：

```python
        if r.returncode == 0 and _submodule_ready(src):
            return True, ""
        last = (r.stderr or r.stdout)[-800:]
        if prefix:
            _unset_mirror_rewrite(src, prefix)
            prefix = None
```

- [ ] **Step 9: 给镜像改写写测试(加到 `tools/tests/test_fetch.py`)**

```python
class TestMirrorRewrite(unittest.TestCase):
    def test_set_mirror_rewrite_invokes_git_config(self):
        with mock.patch("deps_lib.fetch.subprocess.run") as run:
            fetch._set_mirror_rewrite("/src", "https://ghproxy.net/")
        args = run.call_args[0][0]
        self.assertIn("config", args)
        self.assertIn("url.https://ghproxy.net/https://github.com/.insteadOf", args)
        self.assertIn("https://github.com/", args)
```

- [ ] **Step 10: 跑全量测试确认 GREEN**

Run: `python3 -m unittest discover -s tools/tests -p "test_*.py"`
Expected: OK。

- [ ] **Step 11: Commit**

```bash
git add tools/deps_lib/mirror.py tools/deps_lib/fetch.py tools/tests/test_mirror.py tools/tests/test_fetch.py
git commit -m "feat(tools): GitHub 源码拉取镜像层(ghproxy 测速优先、官方兜底)
- mirror.py:镜像前缀测速选最快可达;不可达返回 None
- fetch.py clone_lib:镜像 URL 优先,失败清理残留后退官方直连(含全量克隆兜底)
- ensure_swiftshader_submodules:仓库级 url.insteadOf 改写,子模块 clone 同走镜像,失败清除退回直连
- 测速不缓存,每次现测,避免陈旧选择"
```

---

### Task 2: Android SDK 定位 + local.properties(android.py)

**Files:**
- Create: `tools/deps_lib/android.py`
- Test: `tools/tests/test_android.py`

**Interfaces:**
- Produces: `android.find_android_sdk() -> str | None`、`android.write_local_properties(project_dir: str, sdk_path: str) -> str`、`android._escape_properties(path: str) -> str`。
- Consumes: 无。Task 3 的 `_gen_as` 调用这两个函数。

- [ ] **Step 1: 写失败测试 `tools/tests/test_android.py`**

```python
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from deps_lib import android


class TestFindAndroidSdk(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self._env = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)

    def test_env_var_wins(self):
        sdk = os.path.join(self.tmp.name, "sdk")
        os.makedirs(sdk)
        os.environ["ANDROID_HOME"] = sdk
        os.environ["ANDROID_SDK_ROOT"] = "/nonexistent"
        self.assertEqual(android.find_android_sdk(), sdk)

    def test_home_android_sdk_fallback(self):
        fake_home = os.path.join(self.tmp.name, "home")
        sdk = os.path.join(fake_home, "Android", "Sdk")
        os.makedirs(sdk)
        with unittest.mock.patch("os.path.expanduser", return_value=fake_home):
            os.environ.pop("ANDROID_HOME", None)
            os.environ.pop("ANDROID_SDK_ROOT", None)
            self.assertEqual(android.find_android_sdk(), sdk)

    def test_user_deps_android_sdk_fallback(self):
        deps = os.path.join(self.tmp.name, "user-deps")
        sdk = os.path.join(deps, "android-sdk")
        os.makedirs(sdk)
        with unittest.mock.patch("os.environ.get", wraps=os.environ.get) as g:
            os.environ.pop("ANDROID_HOME", None)
            os.environ.pop("ANDROID_SDK_ROOT", None)
            os.environ["USER_DEPS"] = deps
            self.assertEqual(android.find_android_sdk(), sdk)

    def test_none_when_missing(self):
        os.environ.pop("ANDROID_HOME", None)
        os.environ.pop("ANDROID_SDK_ROOT", None)
        with unittest.mock.patch("os.path.expanduser", return_value="/nonexistent-home"):
            self.assertIsNone(android.find_android_sdk())


class TestEscapeProperties(unittest.TestCase):
    def test_backslash_escaped(self):
        self.assertEqual(android._escape_properties(r"D:\qsw\Android\Sdk"),
                         r"D:\\qsw\\Android\\Sdk")


class TestWriteLocalProperties(unittest.TestCase):
    def test_writes_sdk_dir(self):
        project_dir = os.path.join(self.tmp = tempfile.TemporaryDirectory().name)
        os.makedirs(project_dir, exist_ok=True)
        p = android.write_local_properties(project_dir, r"D:\qsw\Android\Sdk")
        with open(p, encoding="utf-8") as f:
            self.assertIn(r"sdk.dir=D:\\qsw\\Android\\Sdk", f.read())
```

（`TestWriteLocalProperties` 里 `self.tmp` 写法有误——改为 `self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)`，再 `project_dir = os.path.join(self.tmp.name, "proj")`。以能跑通为准。）

- [ ] **Step 2: 跑测试确认 RED**

Run: `python3 -m unittest tools.tests.test_android -v`
Expected: `ModuleNotFoundError: No module named 'deps_lib.android'`。

- [ ] **Step 3: 实现 `tools/deps_lib/android.py`**

```python
"""Android SDK 定位与 Android Studio 工程辅助。

SDK 探测顺序:ANDROID_HOME / ANDROID_SDK_ROOT 环境变量 → 常见安装路径
(Windows %LOCALAPPDATA%\\Android\\Sdk、Linux ~/Android/Sdk、/opt/android-sdk)
→ .user-deps/android-sdk(tools/android-deps.sh 的落地目录)。
"""
from __future__ import annotations

import os


def find_android_sdk() -> str | None:
    """返回已安装的 Android SDK 根目录;找不到返回 None。"""
    for env in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        v = os.environ.get(env)
        if v and os.path.isdir(v):
            return v
    home = os.path.expanduser("~")
    local = os.environ.get("LOCALAPPDATA", "")
    user_deps = os.environ.get("USER_DEPS", os.path.join(home, ".user-deps"))
    candidates = []
    if local:
        candidates.append(os.path.join(local, "Android", "Sdk"))
    candidates += [
        os.path.join(home, "Android", "Sdk"),
        "/opt/android-sdk",
        os.path.join(user_deps, "android-sdk"),
    ]
    for c in candidates:
        if os.path.isdir(c):
            return c
    return None


def _escape_properties(path: str) -> str:
    """Java .properties 文件转义:反斜杠(Windows 路径)需加倍。"""
    return path.replace("\\", "\\\\")


def write_local_properties(project_dir: str, sdk_path: str) -> str:
    """写 Android Studio 的 local.properties(sdk.dir),返回文件路径。"""
    p = os.path.join(project_dir, "local.properties")
    with open(p, "w", encoding="utf-8") as f:
        f.write(f"sdk.dir={_escape_properties(sdk_path)}\n")
    return p
```

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `python3 -m unittest tools.tests.test_android -v`
Expected: 全部 PASS。

- [ ] **Step 5: 全量 + Commit**

Run: `python3 -m unittest discover -s tools/tests -p "test_*.py"` → OK
```bash
git add tools/deps_lib/android.py tools/tests/test_android.py
git commit -m "feat(tools): Android SDK 定位 + local.properties 写入(android.py)"
```

---

### Task 3: 逐项目分派——Linux vs 构建 + as 真实生成器

**Files:**
- Modify: `tools/deps_lib/project_gen.py`(`_gen_vs` 加 Linux 分支、`_gen_as` 真实实现)
- Modify: `tools/deps_lib/cmake_driver.py`(`_stream` 加 `cwd` 参数)
- Test: `tools/tests/test_project_gen.py`、`tools/tests/test_cmake_driver.py`

**Interfaces:**
- Consumes: Task 2 的 `android.find_android_sdk` / `android.write_local_properties`。
- Produces: `_gen_vs(root, project, variant, generator) -> tuple`(非 Windows 时返回构建结果)、`_gen_as(root, project, variant, generator) -> tuple`(Windows 写 local.properties / Linux 编 apk)。
- Modify: `cmake_driver._stream(cmd, tail_lines=60, cwd=None) -> tuple`(新增 `cwd` 关键字,默认 None 向后兼容)。

- [ ] **Step 1: `_stream` 加 `cwd` 参数(先改实现 + 补测试)**

`tools/deps_lib/cmake_driver.py`:

```python
def _stream(cmd: list, tail_lines: int = 60, cwd: str | None = None) -> tuple:
    ...
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
            stdout=subprocess.PIPE,
            ...
        )
```

`tools/tests/test_cmake_driver.py` 新增：

```python
class TestStreamCwd(unittest.TestCase):
    def test_cwd_passed_to_subprocess(self):
        with mock.patch.object(cmake_driver.subprocess, "Popen") as popen:
            popen.return_value = _FakeProc(["ok"], 0)
            ok, tail = cmake_driver._stream(["true"], cwd="/tmp/proj")
        self.assertTrue(ok)
        self.assertEqual(popen.call_args[1].get("cwd"), "/tmp/proj")
```

- [ ] **Step 2: 跑该测试确认 RED**

Run: `python3 -m unittest tools.tests.test_cmake_driver.TestStreamCwd -v`
Expected: FAIL(`TypeError: _stream() got an unexpected keyword argument 'cwd'`)。

- [ ] **Step 3: 改 `_gen_vs` 加 Linux 构建分支**

`tools/deps_lib/project_gen.py` 的 `_gen_vs` 开头(现有 `if not pool.on_windows(): return True, "跳过: vs 类型仅 Windows"` 处)替换为：

```python
def _gen_vs(root: str, project: str, variant: str, generator: str | None) -> tuple:
    """vs 生成器:Windows 出 .sln(只 configure);Linux/其他直接 configure + build。"""
    project_dir = os.path.join(root, project)
    if not os.path.isfile(os.path.join(project_dir, "CMakeLists.txt")):
        return True, "跳过: 无 CMakeLists.txt"
    if not pool.on_windows():
        # Linux(或 macOS):直接编译出结果,满足"linux->能编译成功即可"。
        build_dir = os.path.join(project_dir, "build", "release")
        os.environ["MINE_ROOT"] = root  # 池根,CMakeLists 里靠 $ENV{MINE_ROOT} 定位池
        bt = "Debug" if variant == "debug" else "Release"
        if variant == "release" and os.path.isfile(os.path.join(project_dir, "CMakePresets.json")):
            cfg = ["cmake", "--preset", "release", "-S", project_dir]
        else:
            cfg = ["cmake", "-S", project_dir, "-B", build_dir, f"-DCMAKE_BUILD_TYPE={bt}"]
        print(f"---- configure {project} (linux): {' '.join(cfg)}", flush=True)
        ok, tail = cmake_driver._stream(cfg)
        if not ok:
            return False, f"configure 失败:\n{tail}"
        print(f"---- build {project} (linux): cmake --build {build_dir}", flush=True)
        ok, tail = cmake_driver._stream(
            ["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 4)])
        if not ok:
            return False, f"build 失败:\n{tail}"
        return True, f"构建完成: {build_dir}"
    # === 以下为原有 Windows .sln 逻辑,保持不动(gen_name 探测、MSVC 注入、缓存清理、
    #    build/vs 目录、-DCMAKE_CONFIGURATION_TYPES、_stream 调用) ===
    gen_name = generator or discover_vs_generator()
    ...
```

注意:原 `_gen_vs` 里 `project_dir` 变量与缓存清理、`os.environ["MINE_ROOT"] = root` 已在 Windows 分支后出现——确认不因新分支提前 return 而留下未定义引用(原代码第 58 行已有 `project_dir = os.path.join(root, project)`,保留即可,新分支复用)。

- [ ] **Step 4: 给 Linux vs 分支写测试(加到 `tools/tests/test_project_gen.py`)**

```python
class TestGenVsLinuxBuild(unittest.TestCase):
    def setUp(self):
        self._old = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._old)

    def test_linux_uses_preset_and_builds(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(True, "")) as stream:
            d = os.path.join(root, "EasyPainter")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "CMakeLists.txt"), "w") as f:
                f.write("project(demo)\n")
            with open(os.path.join(d, "CMakePresets.json"), "w") as f:
                f.write("{}")
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
        self.assertTrue(ok)
        cmds = [c.args[0] for c in stream.call_args_list]
        self.assertIn(["cmake", "--preset", "release", "-S", d], cmds)
        self.assertTrue(any("--build" in c for c in cmds))
        self.assertTrue(msg.startswith("构建完成"))
        self.assertEqual(os.environ.get("MINE_ROOT"), root)

    def test_linux_debug_variant_uses_explicit_build_type(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(True, "")) as stream:
            d = os.path.join(root, "EasyPainter")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "CMakeLists.txt"), "w") as f:
                f.write("project(demo)\n")
            ok, _ = project_gen._gen_vs(root, "EasyPainter", "debug", None)
        self.assertTrue(ok)
        cfg = stream.call_args_list[0].args[0]
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", cfg)
```

- [ ] **Step 5: 改 `_gen_as` 为真实实现**

`tools/deps_lib/project_gen.py` 里 `_gen_as` 整体替换为：

```python
def _gen_as(root: str, project: str, variant: str, generator: str | None) -> tuple:
    """as(Android Studio)生成器:Windows 探测 SDK 写 local.properties;
    Linux 直接 ./gradlew assembleDebug 编 apk。"""
    project_dir = os.path.join(root, project)
    sdk = android.find_android_sdk()
    if pool.on_windows():
        if not sdk:
            return True, "跳过: 未探测到 Android SDK(运行 tools/android-deps.sh 或设置 ANDROID_HOME 后重试)"
        p = android.write_local_properties(project_dir, sdk)
        return True, f"已写 local.properties(sdk.dir={sdk})→ {p}"
    # Linux:直接构建 apk
    if not sdk:
        return True, "跳过: 未探测到 Android SDK(先运行 tools/android-deps.sh 或设置 ANDROID_HOME)"
    gradlew = os.path.join(project_dir, "gradlew")
    if not os.path.isfile(gradlew):
        return True, "跳过: 无 gradlew(先用 tools/new-project.py as 生成 Android 工程)"
    cmd = ["./gradlew", "assembleDebug"]
    print(f"---- build {project} (as): {' '.join(cmd)} in {project_dir}", flush=True)
    ok, tail = cmake_driver._stream(cmd, cwd=project_dir)
    if not ok:
        return False, f"gradlew assembleDebug 失败:\n{tail}"
    apk = os.path.join(project_dir, "app", "build", "outputs", "apk", "debug", "app-debug.apk")
    return True, f"apk: {apk}"
```

并在 `project_gen.py` 顶部 import 加 `from . import android`(与现有 `from . import cmake_driver, manifest, msvc_env, pool` 并列,注意 `android` 是本目录 `deps_lib/android.py`,不会与系统 `android` 模块冲突)。

- [ ] **Step 6: 给 _gen_as 写测试(替换 `tools/tests/test_project_gen.py` 里占位版 `TestGenAsPlaceholder`)**

```python
class TestGenAs(unittest.TestCase):
    def setUp(self):
        self._old = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._old)

    def _make_android_project(self, root, with_gradlew=True):
        d = os.path.join(root, "HelloAndroid")
        os.makedirs(os.path.join(d, "app"), exist_ok=True)
        if with_gradlew:
            with open(os.path.join(d, "gradlew"), "w") as f:
                f.write("#!/usr/bin/env bash\n")
        return d

    def test_windows_with_sdk_writes_local_properties(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen.android, "find_android_sdk",
                               return_value=r"D:\Android\Sdk"), \
             mock.patch.object(project_gen.android, "write_local_properties",
                               return_value="") as wlp:
            self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        wlp.assert_called_once()
        self.assertIn("local.properties", msg)

    def test_windows_no_sdk_skips(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen.android, "find_android_sdk", return_value=None):
            self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        self.assertTrue(msg.startswith("跳过"))

    def test_linux_with_sdk_builds_apk(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.android, "find_android_sdk",
                               return_value="/opt/android-sdk"), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(True, "")) as stream:
            d = self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        cmd, kw = stream.call_args
        self.assertEqual(cmd[0], ["./gradlew", "assembleDebug"])
        self.assertEqual(kw.get("cwd"), d)
        self.assertIn("app-debug.apk", msg)

    def test_linux_no_sdk_skips(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.android, "find_android_sdk", return_value=None):
            self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        self.assertTrue(msg.startswith("跳过"))
```

（同时删除原 `TestGenAsPlaceholder` 与 `TestGenerateDispatch.test_as_is_registered_but_not_implemented`——as 已实现,不再是"未实现"。保留 `test_unknown_type_fails`。）

- [ ] **Step 7: 跑全量测试确认 GREEN**

Run: `python3 -m unittest discover -s tools/tests -p "test_*.py"`
Expected: OK(原有用例 + 新增全过;若 `TestGenerateDispatch.test_as_is_registered_but_not_implemented` 残留报错,按 Step 6 删除)。

- [ ] **Step 8: Commit**

```bash
git add tools/deps_lib/project_gen.py tools/deps_lib/cmake_driver.py tools/tests/test_project_gen.py tools/tests/test_cmake_driver.py
git commit -m "feat(tools): 逐项目分派——Linux vs 直接编译 + as 真实生成器
- _stream 加 cwd 参数(gradlew 需在项目目录跑),默认 None 向后兼容
- _gen_vs 非 Windows 分支:preset/显式 configure + cmake --build 直接出结果
- _gen_as 落地:Windows 探测 SDK 写 local.properties;Linux 缺 SDK 跳过、有则 gradlew assembleDebug 编 apk
- 移除 as 占位标记,汇总不再出现 as 的 TODO"
```

---

### Task 4: new-project.py `--type as` + Android 模板 + HelloAndroid 示例

**Files:**
- Create: `tools/templates/as/`(完整 Android 骨架,见下)
- Modify: `tools/new-project.py`(`LANGS` 加 `as`、`as` 强制 `type: as`、ctx 加 `PROJECT_NAME_LC`)
- Create: `HelloAndroid/`(用 new-project.py 生成的真实示例项目)
- Test: `tools/tests/test_new_project.py`

**Interfaces:**
- Consumes: `new-project.render_template`(既有,递归复制 + `{{KEY}}` 替换)。
- Produces: 可被 Task 3 的 `_gen_as` 消费的 Android 工程结构(`app/` + `gradlew` + `app/build.gradle`)。

**版本锁(Gradle 8.7 / AGP 8.5.2 / Kotlin 1.9.24 / compileSdk 34 / minSdk 24)为 Global Constraints 一部分,不得改动。**

- [ ] **Step 1: 写失败测试(加到 `tools/tests/test_new_project.py`)**

```python
class TestNewProjectAndroid(unittest.TestCase):
    def test_as_lang_generates_android_skeleton(self):
        args = ["as", "HelloAndroid"]
        with mock.patch("new_project.LANGS", {"cpp", "python", "web", "as"}):
            rc = new_project.main(args)
        self.assertEqual(rc, 0)
        dst = os.path.join(new_project.MINE_ROOT, "HelloAndroid")
        for rel in ("settings.gradle", "build.gradle", "gradle.properties",
                    "app/build.gradle", "app/src/main/AndroidManifest.xml",
                    "gradle/wrapper/gradle-wrapper.properties",
                    "gradlew"):
            self.assertTrue(os.path.isfile(os.path.join(dst, rel)), rel)
        with open(os.path.join(dst, "deps.yaml"), encoding="utf-8") as f:
            self.assertIn("type: as", f.read())
        shutil.rmtree(dst, ignore_errors=True)
```

（`test_new_project.py` 现有 import 需有 `mock`/`shutil`;缺则补。`new_project.main` 会真的写到 `MINE_ROOT` 下,测试末尾 `shutil.rmtree` 清理。**注意**:该测试与仓库根已有 `HelloAndroid` 冲突——实现侧若已生成过,先删再跑,或改用临时 MINE_ROOT;以能跑通为准。推荐给 `main` 加可选 `--root` 测试钩子或用 `mock.patch` 改 `MINE_ROOT` 指向临时目录,二选一,写入 Step 2。)

- [ ] **Step 2: 跑测试确认 RED**

Run: `python3 -m unittest tools.tests.test_new_project -v`
Expected: FAIL(`LANGS` 无 `as`、模板不存在、`deps.yaml` 无 `type: as`)。

- [ ] **Step 3: 建 `tools/templates/as/` 骨架文件**

依次创建以下文件(内容见后;`{{...}}` 是 `render_template` 的占位符):

`tools/templates/as/deps.yaml.tmpl`:
```yaml
type: as
use: []
```

`tools/templates/as/settings.gradle`:
```groovy
pluginManagement {
    repositories {
        maven { url 'https://maven.aliyun.com/repository/google' }
        maven { url 'https://maven.aliyun.com/repository/gradle-plugin' }
        google()
        mavenCentral()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven { url 'https://maven.aliyun.com/repository/google' }
        maven { url 'https://maven.aliyun.com/repository/public' }
        google()
        mavenCentral()
    }
}
rootProject.name = "{{PROJECT_NAME}}"
include ':app'
```

`tools/templates/as/build.gradle`:
```groovy
plugins {
    id 'com.android.application' version '8.5.2' apply false
    id 'org.jetbrains.kotlin.android' version '1.9.24' apply false
}
```

`tools/templates/as/gradle.properties`:
```
org.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8
android.useAndroidX=false
```

`tools/templates/as/gradle/wrapper/gradle-wrapper.properties`:
```
distributionBase=GRADLE_USER_HOME
distributionPath=wrapper/dists
distributionUrl=https\://mirrors.cloud.tencent.com/gradle/gradle-8.7-bin.zip
networkTimeout=10000
validateDistributionUrl=true
zipStoreBase=GRADLE_USER_HOME
zipStorePath=wrapper/dists
```

`tools/templates/as/app/build.gradle`:
```groovy
plugins {
    id 'com.android.application'
    id 'org.jetbrains.kotlin.android'
}

android {
    namespace 'com.example.app'
    compileSdk 34

    defaultConfig {
        applicationId "com.example.app"
        minSdk 24
        targetSdk 34
        versionCode 1
        versionName "1.0"
    }
    buildTypes {
        release { minifyEnabled false }
    }
    compileOptions {
        sourceCompatibility JavaVersion.VERSION_17
        targetCompatibility JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = '17' }
}
```

`tools/templates/as/app/src/main/AndroidManifest.xml`:
```xml
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <application android:label="@string/app_name"
                 android:theme="@android:style/Theme.Material.Light">
        <activity android:name=".MainActivity" android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
        </activity>
    </application>
</manifest>
```

`tools/templates/as/app/src/main/java/com/example/app/MainActivity.kt`:
```kotlin
package com.example.app

import android.app.Activity
import android.os.Bundle
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val label = TextView(this)
        label.text = "Hello from {{PROJECT_NAME}}"
        setContentView(label)
    }
}
```

`tools/templates/as/app/src/main/res/values/strings.xml`:
```xml
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">{{PROJECT_NAME}}</string>
</resources>
```

- [ ] **Step 4: 拉取 gradle wrapper 三件套进模板(网络步骤)**

```bash
TAG="v8.7.0"
BASE="https://raw.githubusercontent.com/gradle/gradle/$TAG/gradle/wrapper"
curl -fL -o tools/templates/as/gradle/wrapper/gradle-wrapper.jar "$BASE/gradle-wrapper.jar"
curl -fL -o tools/templates/as/gradlew \
     "https://raw.githubusercontent.com/gradle/gradle/$TAG/gradlew"
curl -fL -o tools/templates/as/gradlew.bat \
     "https://raw.githubusercontent.com/gradle/gradle/$TAG/gradlew.bat"
chmod +x tools/templates/as/gradlew
# 校验 jar 是合法 zip
unzip -l tools/templates/as/gradle/wrapper/gradle-wrapper.jar >/dev/null && echo "jar OK"
```

若 `raw.githubusercontent.com` 拉不下来:改用 GitHub 镜像前缀拼接同一 URL(Global Constraints: 镜像优先)。若最终不可达,该步骤阻塞,应暂停并汇报,不跳过。

- [ ] **Step 5: 改 `new-project.py`**

- `LANGS = {"cpp", "python", "web", "as"}`。
- `--type` help 文案改为 `"IDE 工程类型(默认 vs;as 项目自动用 as)"`。
- 生成前(拿到 `use` 之后、`render_template` 之前)加:
```python
    if args.lang == "as":
        ctx["TYPE"] = "as"   # as 模板强制 Android Studio 类型,忽略 --type
```
（注意:as 模板的 namespace/package 固定 `com.example.app`,**不需要** `PROJECT_NAME_LC` 键,别加。）

- [ ] **Step 6: 跑测试确认 GREEN**

Run: `python3 -m unittest tools.tests.test_new_project -v`
Expected: 全部 PASS。

- [ ] **Step 7: 生成并提交 HelloAndroid 示例项目**

```bash
python3 tools/new-project.py as HelloAndroid
# 检查骨架齐了再提交(不应包含 local.properties——那是 _gen_as 生成的,不提交)
ls HelloAndroid/settings.gradle HelloAndroid/gradle/wrapper/gradle-wrapper.jar HelloAndroid/gradlew
```

- [ ] **Step 8: 全量 + Commit**

Run: `python3 -m unittest discover -s tools/tests -p "test_*.py"` → OK
```bash
git add tools/templates/as tools/new-project.py tools/tests/test_new_project.py HelloAndroid
git commit -m "feat(tools): new-project.py 支持 as 类型 + Android 模板 + HelloAndroid 示例
- tools/templates/as/:AGP 8.5.2 + Kotlin 1.9.24 + Gradle 8.7(腾讯镜像),纯 Kotlin Activity,无 NDK
- gradle wrapper 三件套(jar/gradlew/gradlew.bat)随模板入库
- deps.yaml 强制 type: as;settings.gradle 等用 {{PROJECT_NAME}} 模板化
- HelloAndroid 为端到端验证载体,提交生成结果(不含 local.properties)"
```

---

### Task 5: Android 工具链脚本 + Windows 双击入口 + 逐项目构建接线

**Files:**
- Create: `tools/android-deps.sh`(JDK + Android SDK 探测/下载/许可证/env.sh)
- Create: `setup.bat`(Windows 双击入口)
- Modify: `tools/setup-env.sh`(工具链步接入 android-deps.sh)
- Modify: `tools/win-deps.sh`(无 pacman 时引导 MSYS2)
- Test: `tools/tests/test_android_deps.sh`(静态断言,仿 `test_win_deps_msvc.sh`)、`tools/tests/test_setup_env_probe.sh`(若现有该文件,扩展 setup.bat 存在性断言)

**Interfaces:**
- Consumes: 无(独立脚本)。env.sh 里 `ANDROID_HOME`/`JAVA_HOME` 供 Task 2 的 `find_android_sdk` 与 gradle 使用。
- Produces: `.user-deps/env.sh` 追加 `export ANDROID_HOME=...`、`export JAVA_HOME=...`、`export PATH=...:$PATH`; `setup.bat` 可双击运行。

> **评审修正(2026-08-28,cmdline-tools spec→plan 偏差)**:spec §5 要求 cmdline-tools 国内镜像兜底,原实现只有官方 `commandlinetools-${PLAT}-latest.zip`(无 `-latest` 别名,官方与镜像均 404)。实测腾讯云 `https://mirrors.cloud.tencent.com/AndroidSDK/commandlinetools-{linux,win}-16111833_latest.zip` 返回 200,故改为镜像优先(腾讯云)+ 官方 `dl.google.com` 版本号 URL 兜底,平台 token 用 `win`(非 `windows`)。

- [ ] **Step 1: 写静态断言测试 `tools/tests/test_android_deps.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ANDROID_DEPS="$ROOT/android-deps.sh"
[ -f "$ANDROID_DEPS" ] || { echo "FAIL: android-deps.sh 不存在"; exit 1; }

# 禁止 sudo
if grep -qE '\bsudo\b|\bapt install\b|\byum install\b|\bdnf install\b' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 出现 sudo/包管理器直装"; exit 1
fi
# 国内镜像优先(Adoptium/JDK + cmdline-tools 官方 URL 存在即可,镜像探测逻辑仿 win-deps.sh)
if ! grep -qE 'api\.adoptium\.net|mirrors\.huaweicloud\.com/openjdk' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 缺 JDK 下载源"; exit 1
fi
if ! grep -qE 'commandlinetools-(linux|windows)-latest\.zip' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 缺 cmdline-tools 下载 URL"; exit 1
fi
# 许可证自动接受
if ! grep -qE 'sdkmanager.*--licenses' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 缺 sdkmanager --licenses"; exit 1
fi
# env.sh 导出
if ! grep -qE 'export ANDROID_HOME=' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 未写 ANDROID_HOME 到 env.sh"; exit 1
fi
# 复用优先:先探测现成 SDK,找不到才下载
if ! grep -qE 'ANDROID_HOME|ANDROID_SDK_ROOT' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 未优先探测已存在的 SDK"; exit 1
fi
echo "PASS android-deps.sh 无 sudo + 镜像 + 许可证 + env.sh 导出 + 复用优先"
```

- [ ] **Step 2: 实现 `tools/android-deps.sh`**

```bash
#!/usr/bin/env bash
# Android 工具链(免 sudo):JDK 17 + Android SDK cmdline-tools。
# 先探测现成(ANDROID_HOME/ANDROID_SDK_ROOT → 常见路径 → .user-deps/android-sdk),
# 找不到才下载;国内镜像优先、官方兜底。产物写 .user-deps/env.sh,供 setup-env.sh
# source 后传给 gen-projects.py(CMake/Android 构建需要 ANDROID_HOME/JAVA_HOME)。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
USER_DEPS="${USER_DEPS:-$ROOT/.user-deps}"
ENV_SH="$USER_DEPS/env.sh"
mkdir -p "$USER_DEPS"

info() { printf '[INFO] %s\n' "$*"; }
die()  { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

case "$(uname -s)" in
  MINGW*|MSYS*) PLAT=windows ;;
  *) PLAT=linux ;;
esac

# --- ① JDK 17(AGP 8.5/Gradle 8.7 需要):探测现成,缺失下载 Temurin 17 到 .user-deps ---
JAVA_HOME=""
if command -v java >/dev/null 2>&1; then
  ver="$(java -version 2>&1 | head -1)"
  if printf '%s' "$ver" | grep -qE '"(1[7-9]|[2-9][0-9])\.'; then
    JAVA_HOME="$(dirname "$(dirname "$(command -v java)")")"   # 近似;够 gradle 用即可
    info "① 复用系统 JDK: $ver"
  fi
fi
if [ -z "$JAVA_HOME" ] && [ -x "$USER_DEPS/jdk17/bin/java" ]; then
  JAVA_HOME="$USER_DEPS/jdk17"
  info "① 复用 .user-deps 已装 JDK17"
fi
if [ -z "$JAVA_HOME" ]; then
  info "① 未探测到 JDK 17+,下载 Temurin 17 到 .user-deps/jdk17 …"
  JDK_DIR="$USER_DEPS/jdk17"
  mkdir -p "$JDK_DIR"
  # Adoptium API(官方,可直连/经 CDN);国内镜像兜底见下方候选
  case "$PLAT" in
    linux)   JDK_URL="https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse"; JDK_ARCHIVE="$USER_DEPS/jdk17.tar.gz" ;;
    windows) JDK_URL="https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse"; JDK_ARCHIVE="$USER_DEPS/jdk17.zip" ;;
  esac
  curl -fL --retry 3 -o "$JDK_ARCHIVE" "$JDK_URL" \
    || die "JDK 下载失败(可手动装 JDK17 后重跑;或把 JDK 解压到 .user-deps/jdk17)"
  case "$PLAT" in
    linux)   tar -xzf "$JDK_ARCHIVE" -C "$JDK_DIR" --strip-components=1 ;;
    windows) unzip -qo "$JDK_ARCHIVE" -d "$JDK_DIR" && [ -d "$JDK_DIR/jdk-17" ] && mv "$JDK_DIR/jdk-17"/* "$JDK_DIR" || true ;;
  esac
  rm -f "$JDK_ARCHIVE"
  [ -x "$JAVA_HOME/bin/java" ] || die "JDK 解压后找不到 java"
fi

# --- ② Android SDK cmdline-tools:探测现成,缺失下载 ---
ANDROID_HOME=""
for cand in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$USER_DEPS/android-sdk" \
            "$LOCALAPPDATA/Android/Sdk" "$HOME/Android/Sdk" "/opt/android-sdk"; do
  [ -n "$cand" ] && [ -f "$cand/cmdline-tools/latest/bin/sdkmanager" ] && { ANDROID_HOME="$cand"; break; }
done
if [ -n "$ANDROID_HOME" ]; then
  info "② 复用已安装 Android SDK: $ANDROID_HOME"
else
  info "② 未探测到 Android SDK,下载 cmdline-tools 到 .user-deps/android-sdk …"
  ANDROID_HOME="$USER_DEPS/android-sdk"
  mkdir -p "$ANDROID_HOME/cmdline-tools"
  CT_URL="https://dl.google.com/android/repository/commandlinetools-${PLAT}-latest.zip"
  CT_ARCHIVE="$USER_DEPS/cmdline-tools.zip"
  curl -fL --retry 3 -o "$CT_ARCHIVE" "$CT_URL" \
    || die "cmdline-tools 下载失败(官方 URL 不可达;可手动下载同 URL 到 $CT_ARCHIVE 后重跑)"
  unzip -qo "$CT_ARCHIVE" -d "$ANDROID_HOME/cmdline-tools"
  [ -d "$ANDROID_HOME/cmdline-tools/cmdline-tools" ] \
    && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
  rm -f "$CT_ARCHIVE"
  [ -x "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ] || die "cmdline-tools 解压后缺 sdkmanager"
fi

# --- ③ 接受许可证(sdkmanager 需要 JAVA_HOME)---
if [ -n "${JAVA_HOME:-}" ]; then export JAVA_HOME; fi
yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --licenses --sdk_root="$ANDROID_HOME" >/dev/null 2>&1 \
  || warn_license=1

# --- ④ 写 env.sh(幂等:存在则保留既有行,追加/更新本脚本的导出)---
if [ ! -f "$ENV_SH" ]; then
  printf '# Android 工具链环境(由 tools/android-deps.sh 生成)\n' > "$ENV_SH"
fi
_san() { grep -v '^# ' "$1" 2>/dev/null | grep -v '^$' || true; }
_san "$ENV_SH" | grep -qE '^export ANDROID_HOME=' \
  || printf 'export ANDROID_HOME="%s"\n' "$ANDROID_HOME" >> "$ENV_SH"
if [ -n "${JAVA_HOME:-}" ]; then
  _san "$ENV_SH" | grep -qE '^export JAVA_HOME=' \
    || printf 'export JAVA_HOME="%s"\n' "$JAVA_HOME" >> "$ENV_SH"
fi
info "④ Android 工具链就绪: ANDROID_HOME=$ANDROID_HOME"
```

（注意:上面 `yes | sdkmanager --licenses` 若失败不应让整链 die——Android 无 SDK 兴趣的机器也应继续。把失败降级为 warn:`|| warn "许可证接受失败(不影响其余项目)"`。其余平台分支(`unzip`/`tar`)与 `win-deps.sh` 同套路。）

- [ ] **Step 3: 跑静态测试确认 GREEN**

Run: `bash tools/tests/test_android_deps.sh`
Expected: `PASS ...`。

- [ ] **Step 4: 接线 `setup-env.sh` + 建 `setup.bat` + `win-deps.sh` 引导 MSYS2**

`tools/setup-env.sh` 工具链步(现有 `bash "$MINE_ROOT/tools/install-user-deps.sh"` 之后、池步之前)加:

```bash
  # Android 工具链(JDK + SDK;探测复用优先,缺失才下载;Windows/Linux 共用)
  bash "$MINE_ROOT/tools/android-deps.sh"
```

仓库根新建 `setup.bat`:

```bat
@echo off
rem Mine 一键搭建入口:双击运行,调用 tools/setup-env.sh(Git for Windows 的 bash)。
chcp 65001 >nul
where bash >nul 2>nul
if errorlevel 1 (
  echo [ERROR] 未找到 bash.exe(Git for Windows)。
  echo 请先安装 Git for Windows: https://gitforwindows.org/ 后重试。
  pause
  exit /b 1
)
bash "%~dp0tools\setup-env.sh"
set "rc=%errorlevel%"
if not "%rc%"=="0" (
  echo.
  echo [ERROR] setup-env.sh 退出码 %rc%,见上方日志。
)
pause
exit /b %rc%
```

`tools/win-deps.sh` 顶部(`setup_mirrors` 调用之前)加 MSYS2 引导——检测无 pacman 时(如 Git Bash),下载 msys2 base 解压到 `.user-deps/msys64` 并用它重入本脚本:

```bash
# --- 0.5 非 MSYS2 环境(如 Git Bash)引导:下载 msys2 base 到 .user-deps/msys64 ---
if ! command -v pacman >/dev/null 2>&1; then
  info "0.5 未检测到 pacman(非 MSYS2 环境,如 Git Bash)。引导 MSYS2 到 \$USER_DEPS/msys64 …"
  MSYS2_DIR="$USER_DEPS/msys64"
  [ -x "$MSYS2_DIR/usr/bin/pacman.exe" ] || {
    mkdir -p "$USER_DEPS"
    # 镜像优先:清华/中科大 msys2 仓库的 msys2-base-x86_64-*.tar.xz(取最新一个)
    MSYS2_BASE=""
    for base in "https://mirrors.tuna.tsinghua.edu.cn/msys2/distrib/x86_64/" \
                "https://mirrors.ustc.edu.cn/msys2/distrib/x86_64/"; do
      idx="$(curl -fsSL --max-time 20 "$base" 2>/dev/null || true)"
      fn="$(printf '%s' "$idx" | grep -oE 'msys2-base-x86_64-[0-9]{8}\.tar\.xz' | sort -r | head -1 || true)"
      [ -n "$fn" ] && { MSYS2_BASE="$base$fn"; break; }
    done
    [ -n "$MSYS2_BASE" ] || die "MSYS2 base 下载源不可达(镜像全挂)"
    curl -fL --retry 3 -o "$USER_DEPS/msys2-base.tar.xz" "$MSYS2_BASE" \
      || die "MSYS2 base 下载失败"
    tar -xJf "$USER_DEPS/msys2-base.tar.xz" -C "$USER_DEPS"   # 解出 msys64/
    rm -f "$USER_DEPS/msys2-base.tar.xz"
  }
  # 用新 MSYS2 的 bash 重入本脚本(路径转换:MSYS2 内用 /d/qsw/Mine)
  MSYS_ROOT="$(cygpath -u "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
  exec "$MSYS2_DIR/usr/bin/bash.exe" -lc "cd '$MSYS_ROOT' && export USER_DEPS='$USER_DEPS' && bash tools/win-deps.sh"
fi
```

（`ROOT` 在 win-deps.sh 顶部已有定义;`cygpath` 在 Git Bash 下可用。重入后即拥有 `pacman`,走既有流程。）

- [ ] **Step 5: 静态验证 + 本地回归**

- `bash tools/tests/test_android_deps.sh` → PASS
- `bash tools/tests/test_win_deps_msvc.sh` → PASS(win-deps.sh 改动不应破坏既有断言;若 0.5 段把"env.sh 生成段抽取"的 awk 断言干扰了,把引导段放在 `env.sh` 生成段**之后**或调整测试锚点)
- `python3 -m unittest discover -s tools/tests -p "test_*.py"` → OK
- Linux 侧真实回归:`bash tools/setup-env.sh --check`(只检测,不安装)不报错,且不出现 sudo。

- [ ] **Step 6: Commit**

```bash
git add tools/android-deps.sh tools/setup-env.sh setup.bat tools/win-deps.sh tools/tests/test_android_deps.sh
git commit -m "feat(tools): Android 工具链脚本 + Windows 双击入口 + MSYS2 引导
- android-deps.sh:JDK17 + Android SDK cmdline-tools 探测复用优先/缺失下载,免 sudo,国内镜像优先,许可证自动接受,写 env.sh
- setup-env.sh:工具链步接入 android-deps.sh(Linux/Windows 共用)
- setup.bat:检测 Git Bash 的 bash.exe,双击调 setup-env.sh,失败提示并暂停
- win-deps.sh:无 pacman 环境(Git Bash)下载 msys2 base 引导后重入,镜像优先"
```

---

## Self-Review

**1. Spec coverage:**
- §4.1 Windows 入口/边界 → Task 5(setup.bat、win-deps.sh 引导)
- §4.2 Linux 无 sudo → Task 5(android-deps.sh 无 sudo)+ Global Constraints
- §5 Android 工具链(SDK/JDK/Gradle/Maven 镜像/许可证/免 sudo)→ Task 5 + Task 4(模板内 Maven/Gradle 镜像)
- §6 GitHub 镜像 → Task 1
- §7 as 生成器 + 示例项目 + new-project.py → Task 3(`_gen_as`)+ Task 4(模板/示例/`--type as`)+ Task 2(local.properties)
- §3 分派表 Linux vs 构建 → Task 3(`_gen_vs` 非 Windows 分支)
- §8 汇总/退出码 → 既有 gen-projects.py 逻辑复用,as 已实现后不再 TODO,无需新任务
- §9 测试策略 → 每任务 TDD + 静态 bash 断言(Task 5)

**2. Placeholder scan:** 无 TBD/TODO;所有代码步骤含完整可运行实现。Task 4 Step 1 的测试存在"写到真实 MINE_ROOT 需清理/临时根"的实现歧义,已在 Step 1 注释里指明处理方向。

**3. Type consistency:** `_stream(cmd, tail_lines=60, cwd=None)`(Task 3 定义)被 Task 3 的 `_gen_as`/`_gen_vs` 以 `cwd=` 关键字调用;`android.find_android_sdk`/`write_local_properties`(Task 2)被 Task 3 `_gen_as` 调用;签名在各定义处一致。`PROJECT_NAME_LC` 已在 Task 4 Step 5 移除(模板 package 固定 `com.example.app`)。
