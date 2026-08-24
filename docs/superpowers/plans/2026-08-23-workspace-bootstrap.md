# 工作空间脚手架(workspace-bootstrap)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `Mine/` 下落地一套「全局共享三方库池 + 清单 + 一键脚本 + 项目脚手架」的工作空间,实现一项目一文件夹、三方库只拉一次/只编一次、仓库可复现。

**Architecture:** `tools/` 集中全部逻辑(纯 Python 模块 `deps_lib/` + 三个 CLI 脚本 + 模板),`third_party/` 是全局共享池(`_src` 源码 / `_build` 中间 / `_install` 预编译产物,按 `<name>-<ver>/<variant>` 布局),项目只声明 `use` 依赖并靠 `find_package` 链接池产物。

**Tech Stack:** Python 3(标准库 + PyYAML)、CMake 3.22+、Ninja、git、unittest(测试,零额外依赖)。

**Spec:** `docs/superpowers/specs/2026-08-23-workspace-bootstrap-design.md`

## Global Constraints

- 根目录固定为 `Mine/`(即本仓库),所有脚本用 `deps_lib.MINE_ROOT` 推导,不写死绝对路径。
- Python 最低 3.8;依赖仅 PyYAML(已装 6.0.1);测试用标准库 `unittest`,命令统一 `python3 -m unittest discover -s tools/tests -v`。
- 目录命名 `<name>-<ver>`:`<ver>` 取清单 `tag`,`tag` 中非 `[A-Za-z0-9._-]` 字符替换为 `-`。
- 池产物变体 `[release, debug]`;预编译产物目录 `_install/<name>-<ver>/<variant>/`,以 `.built` 标记文件为已编译权威判定。
- git 忽略:`third_party/_src/`、`_build/`、`_install/`、`.pool.lock.json`、`build/`、`__pycache__/` 等;只提交 `tools/`、`third_party/deps.yaml`、项目骨架源码。
- 硬依赖版本下限:cmake ≥ 3.22、g++ ≥ 11、python3 ≥ 3.8;缺则 `sudo apt install` 补。
- 每个 task 结束独立可测、单独 commit。

---

### Task 1: 目录骨架 + .gitignore + manifest.py(清单解析)

**Files:**
- Create: `.gitignore`
- Create: `tools/deps_lib/__init__.py`
- Create: `tools/deps_lib/manifest.py`
- Create: `tools/tests/__init__.py`
- Create: `tools/tests/test_manifest.py`
- Create: `third_party/deps.yaml`

**Interfaces:**
- Consumes: 无(首个 task)
- Produces:
  - `deps_lib.MINE_ROOT`(str)—— Mine 根绝对路径,后续所有模块/脚本复用。
  - `deps_lib.manifest.LibSpec`(frozen dataclass: `name`, `repo`, `tag`, `build="cmake"`, `options=()`)。
  - `deps_lib.manifest.ver_dir(name, tag) -> str`。
  - `deps_lib.manifest.load_global_manifest(root) -> dict`。
  - `deps_lib.manifest.load_project_manifest(project_dir) -> list[str]`。
  - `deps_lib.manifest.resolve_libs(global_manifest, use) -> list[LibSpec]`。
  - `deps_lib.manifest.all_libs(global_manifest) -> list[LibSpec]`。
  - `deps_lib.manifest.variants(global_manifest) -> list[str]`。

- [ ] **Step 1: 写失败的测试** `tools/tests/test_manifest.py`

```python
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/

import unittest
from deps_lib import manifest


class TestVerDir(unittest.TestCase):
    def test_plain_tag(self):
        self.assertEqual(manifest.ver_dir("fmt", "10.2.1"), "fmt-10.2.1")

    def test_slash_tag_is_sanitized(self):
        self.assertEqual(manifest.ver_dir("spdlog", "v1/14"), "spdlog-v1-14")


class TestResolve(unittest.TestCase):
    def setUp(self):
        self.gm = {
            "variants": ["release", "debug"],
            "libs": {
                "fmt": {"repo": "https://example/fmt.git", "tag": "10.2.1",
                        "options": ["FMT_TEST=OFF"]},
                "glm": {"repo": "https://example/glm.git", "tag": "1.0.1"},
            },
        }

    def test_resolve_one(self):
        specs = manifest.resolve_libs(self.gm, ["fmt"])
        self.assertEqual(len(specs), 1)
        self.assertEqual(specs[0].name, "fmt")
        self.assertEqual(specs[0].options, ("FMT_TEST=OFF",))
        self.assertEqual(specs[0].build, "cmake")

    def test_resolve_missing_raises(self):
        with self.assertRaises(KeyError):
            manifest.resolve_libs(self.gm, ["nope"])

    def test_variants_default(self):
        self.assertEqual(manifest.variants({"libs": {}}), ["release", "debug"])

    def test_all_libs_order(self):
        specs = manifest.all_libs(self.gm)
        self.assertEqual([s.name for s in specs], ["fmt", "glm"])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: 失败 `ModuleNotFoundError: No module named 'deps_lib'`

- [ ] **Step 3: 建目录骨架 + .gitignore**

Create `.gitignore`:

```gitignore
# 第三方池:源码 / 中间构建 / 预编译产物 / 状态锁
third_party/_src/
third_party/_build/
third_party/_install/
third_party/.pool.lock.json

# 构建产物
build/
*.o
*.a
*.so
*.dylib

# Python
__pycache__/
*.pyc

# IDE
.idea/
.vscode/
```

Create `tools/deps_lib/__init__.py`:

```python
"""deps_lib —— 工作空间依赖管理共享模块。"""
import os

# Mine 根:tools/deps_lib/__init__.py -> tools/deps_lib -> tools -> Mine
MINE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

__all__ = ["MINE_ROOT"]
```

Create `tools/tests/__init__.py`(空文件)。

- [ ] **Step 4: 实现 manifest.py**

Create `tools/deps_lib/manifest.py`:

```python
"""deps.yaml 清单解析:全局定义 + 项目 use 引用合并。"""
from __future__ import annotations

import dataclasses
import os

import yaml


@dataclasses.dataclass(frozen=True)
class LibSpec:
    name: str
    repo: str
    tag: str
    build: str = "cmake"
    options: tuple = ()

    def __post_init__(self):
        object.__setattr__(self, "options", tuple(self.options or ()))


def ver_dir(name: str, tag: str) -> str:
    """由 name + tag 生成池目录名,非 [A-Za-z0-9._-] 字符替换为 '-'。"""
    safe = "".join(c if (c.isalnum() or c in "._-") else "-" for c in str(tag))
    return f"{name}-{safe}"


def _load_yaml(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data or {}


def load_global_manifest(root: str) -> dict:
    path = os.path.join(root, "third_party", "deps.yaml")
    if not os.path.exists(path):
        raise FileNotFoundError(f"全局清单不存在: {path}")
    return _load_yaml(path)


def load_project_manifest(project_dir: str) -> list:
    path = os.path.join(project_dir, "deps.yaml")
    if not os.path.exists(path):
        raise FileNotFoundError(f"项目清单不存在: {path}")
    return _load_yaml(path).get("use", []) or []


def resolve_libs(global_manifest: dict, use: list) -> list:
    libs = global_manifest.get("libs", {}) or {}
    missing = [n for n in use if n not in libs]
    if missing:
        raise KeyError(f"全局清单未定义这些库: {', '.join(sorted(missing))}")
    out = []
    for name in use:
        d = libs[name]
        out.append(LibSpec(
            name=name,
            repo=d["repo"],
            tag=str(d["tag"]),
            build=d.get("build", "cmake"),
            options=d.get("options", []) or [],
        ))
    return out


def all_libs(global_manifest: dict) -> list:
    return resolve_libs(global_manifest, list(global_manifest.get("libs", {}).keys()))


def variants(global_manifest: dict) -> list:
    return global_manifest.get("variants", ["release", "debug"]) or ["release", "debug"]
```

- [ ] **Step 5: 写全局清单样例** `third_party/deps.yaml`

```yaml
# 全局三方库清单 —— 唯一事实来源。
# 项目侧只通过 deps.yaml 的 `use` 列表引用这里的库名。
default_variant: release
variants: [release, debug]

libs:
  fmt:
    repo: https://github.com/fmtlib/fmt.git
    tag: "10.2.1"
    build: cmake
    options: [FMT_TEST=OFF]
  glm:
    repo: https://github.com/g-truc/glm.git
    tag: "1.0.1"
    build: cmake
```

- [ ] **Step 6: 跑测试确认通过**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: 全部 PASS(6 tests)

- [ ] **Step 7: Commit**

```bash
git add .gitignore tools/deps_lib tools/tests third_party/deps.yaml
git commit -m "feat: deps.yaml manifest parsing (deps_lib)"
```

---

### Task 2: pool.py(池路径与状态)

**Files:**
- Create: `tools/deps_lib/pool.py`
- Create: `tools/tests/test_pool.py`

**Interfaces:**
- Consumes: `deps_lib.manifest.ver_dir`(Task 1)
- Produces:
  - `pool.src_dir(root, name, tag)`, `pool.build_dir(root, name, tag, variant)`, `pool.install_dir(root, name, tag, variant)`, `pool.lock_path(root)`。
  - `pool.load_lock(root) -> dict`, `pool.save_lock(root, lock)`。
  - `pool.is_fetched(root, name, tag) -> bool`(以 `_src/<ver_dir>/` 目录存在为准)。
  - `pool.is_built(root, name, tag, variant) -> bool`(以 `.built` 文件存在为准)。

- [ ] **Step 1: 写失败的测试** `tools/tests/test_pool.py`

```python
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
from deps_lib import pool


class TestPool(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.addCleanup(self.tmp.cleanup)

    def test_paths(self):
        self.assertEqual(
            pool.src_dir(self.root, "fmt", "10.2.1"),
            os.path.join(self.root, "third_party", "_src", "fmt-10.2.1"),
        )
        self.assertEqual(
            pool.install_dir(self.root, "fmt", "10.2.1", "debug"),
            os.path.join(self.root, "third_party", "_install", "fmt-10.2.1", "debug"),
        )

    def test_lock_roundtrip(self):
        pool.save_lock(self.root, {"a": 1})
        self.assertEqual(pool.load_lock(self.root), {"a": 1})

    def test_load_lock_missing_is_empty(self):
        self.assertEqual(pool.load_lock(self.root), {})

    def test_is_fetched_and_built(self):
        os.makedirs(pool.src_dir(self.root, "fmt", "10.2.1"))
        self.assertTrue(pool.is_fetched(self.root, "fmt", "10.2.1"))
        self.assertFalse(pool.is_fetched(self.root, "glm", "1.0.1"))

        inst = pool.install_dir(self.root, "fmt", "10.2.1", "release")
        os.makedirs(inst)
        self.assertFalse(pool.is_built(self.root, "fmt", "10.2.1", "release"))
        with open(os.path.join(inst, ".built"), "w") as f:
            f.write("")
        self.assertTrue(pool.is_built(self.root, "fmt", "10.2.1", "release"))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: `ImportError`(`deps_lib.pool` 不存在)

- [ ] **Step 3: 实现 pool.py**

Create `tools/deps_lib/pool.py`:

```python
"""第三方库池的路径约定与状态判定。"""
from __future__ import annotations

import json
import os

from .manifest import ver_dir


def _tp(root: str) -> str:
    return os.path.join(root, "third_party")


def src_dir(root: str, name: str, tag: str) -> str:
    return os.path.join(_tp(root), "_src", ver_dir(name, tag))


def build_dir(root: str, name: str, tag: str, variant: str) -> str:
    return os.path.join(_tp(root), "_build", ver_dir(name, tag), variant)


def install_dir(root: str, name: str, tag: str, variant: str) -> str:
    return os.path.join(_tp(root), "_install", ver_dir(name, tag), variant)


def lock_path(root: str) -> str:
    return os.path.join(_tp(root), ".pool.lock.json")


def load_lock(root: str) -> dict:
    p = lock_path(root)
    if not os.path.exists(p):
        return {}
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def save_lock(root: str, lock: dict) -> None:
    p = lock_path(root)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(lock, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, p)


def is_fetched(root: str, name: str, tag: str) -> bool:
    return os.path.isdir(src_dir(root, name, tag))


def is_built(root: str, name: str, tag: str, variant: str) -> bool:
    return os.path.isfile(os.path.join(install_dir(root, name, tag, variant), ".built"))
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: 全部 PASS(6 + 5 = 11 tests)

- [ ] **Step 5: Commit**

```bash
git add tools/deps_lib/pool.py tools/tests/test_pool.py
git commit -m "feat: third-party pool path & state helpers"
```

---

### Task 3: fetch-deps.py(一键拉取源码)

**Files:**
- Create: `tools/deps_lib/fetch.py`
- Create: `tools/fetch-deps.py`
- Create: `tools/tests/test_fetch.py`

**Interfaces:**
- Consumes: `deps_lib.manifest`(load_global_manifest / load_project_manifest / resolve_libs / all_libs), `deps_lib.pool`(src_dir / is_fetched / load_lock / save_lock), `deps_lib.MINE_ROOT`。
- Produces:
  - `deps_lib.fetch.clone_lib(root, lib) -> tuple[bool, str]`(返回 `(ok, commit_or_err)`)。
  - `deps_lib.fetch.run(root, libs, jobs) -> dict`(返回汇总 `{"fetched": [...], "skipped": [...], "failed": [...]}`)。
  - `deps_lib.fetch.collect_libs(args) -> list`(按 `--project`/`--all` 收集需集)。
  - CLI `python3 tools/fetch-deps.py [--project DIR | --all] [--jobs N]`。

- [ ] **Step 1: 写失败的测试** `tools/tests/test_fetch.py`

```python
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
from deps_lib import fetch as fetch_mod
from deps_lib.manifest import LibSpec


def _make_local_repo(path: str, tag: str = "v1.0.0") -> None:
    os.makedirs(path)
    subprocess.run(["git", "init", "-q", path], check=True)
    with open(os.path.join(path, "README.md"), "w") as f:
        f.write("fixture\n")
    subprocess.run(["git", "-C", path, "add", "."], check=True)
    subprocess.run(["git", "-C", path, "commit", "-q", "-m", "init"], check=True)
    subprocess.run(["git", "-C", path, "tag", tag], check=True)


class TestCloneLib(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.repo = os.path.join(self.tmp.name, "fixture-repo")
        self.addCleanup(self.tmp.cleanup)

    def test_clone_and_skip(self):
        _make_local_repo(self.repo, "v1.0.0")
        lib = LibSpec(name="demo", repo=self.repo, tag="v1.0.0")

        ok, commit = fetch_mod.clone_lib(self.root, lib)
        self.assertTrue(ok, msg=commit)
        self.assertTrue(commit)
        # 已拉取则跳过:再次 clone 到已存在目录应报错
        ok2, _ = fetch_mod.clone_lib(self.root, lib)
        self.assertFalse(ok2)


if __name__ == "__main__":
    unittest.main()
```

> 注:`fetch.py` 需能被 `import fetch` 命中 —— 测试文件把 `tools/` 加入了 `sys.path`,而脚本在 `tools/` 下,故 `import fetch` 生效。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: `ImportError: No module named 'deps_lib.fetch'`

- [ ] **Step 3: 实现 deps_lib/fetch.py 与 fetch-deps.py**

Create `tools/deps_lib/fetch.py`:

```python
"""三方库源码拉取:clone 到池 + 状态汇总。"""
from __future__ import annotations

import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

from . import MINE_ROOT, manifest, pool
from .manifest import LibSpec, ver_dir


def clone_lib(root: str, lib: LibSpec):
    """把 lib 源码 clone 到池 _src/<ver_dir>。返回 (ok, commit_or_err)。"""
    src = pool.src_dir(root, lib.name, lib.tag)
    if os.path.isdir(src):
        return False, "already exists"
    os.makedirs(os.path.dirname(src), exist_ok=True)

    # 主路径:tag/branch 浅克隆
    r = subprocess.run(
        ["git", "clone", "--depth", "1", "--branch", lib.tag, lib.repo, src],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        # 回退:全量克隆后 checkout 任意 ref(含 commit sha)
        r2 = subprocess.run(["git", "clone", lib.repo, src], capture_output=True, text=True)
        if r2.returncode != 0:
            return False, (r.stderr + "\n" + r2.stderr).strip()
        r3 = subprocess.run(["git", "-C", src, "checkout", lib.tag], capture_output=True, text=True)
        if r3.returncode != 0:
            return False, r3.stderr.strip()

    rc = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True)
    commit = rc.stdout.strip() if rc.returncode == 0 else ""
    return True, commit


def run(root: str, libs: list, jobs: int) -> dict:
    summary = {"fetched": [], "skipped": [], "failed": []}
    lock = pool.load_lock(root)

    def _work(lib):
        key = ver_dir(lib.name, lib.tag)
        if pool.is_fetched(root, lib.name, lib.tag):
            return ("skipped", key)
        ok, msg = clone_lib(root, lib)
        if ok:
            lock[key] = {
                "repo": lib.repo,
                "requested_tag": lib.tag,
                "commit": msg,
                "fetched": True,
            }
            return ("fetched", key)
        return ("failed", f"{key}: {msg}")

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for kind, item in ex.map(_work, libs):
            summary[kind].append(item)

    pool.save_lock(root, lock)
    return summary


def collect_libs(args) -> list:
    gm = manifest.load_global_manifest(MINE_ROOT)
    if args.project:
        use = manifest.load_project_manifest(args.project)
        return manifest.resolve_libs(gm, use)
    if args.all:
        return manifest.all_libs(gm)
    raise SystemExit("必须指定 --project <dir> 或 --all")
```

Create `tools/fetch-deps.py`:

```python
#!/usr/bin/env python3
"""一键拉取三方库源码进池。只拉取,不编译。"""
from __future__ import annotations

import argparse
import sys

from deps_lib import MINE_ROOT, fetch


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="拉取三方库源码进共享池")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--project", metavar="DIR", help="按项目 deps.yaml 的 use 集拉取")
    g.add_argument("--all", action="store_true", help="拉取全局清单全部库")
    p.add_argument("--jobs", type=int, default=4, help="并行 clone 数(默认 4)")
    args = p.parse_args(argv)

    libs = fetch.collect_libs(args)
    if not libs:
        print("无需要拉取的库。")
        return 0

    summary = fetch.run(MINE_ROOT, libs, args.jobs)
    for k in ("fetched", "skipped", "failed"):
        for item in summary[k]:
            print(f"[{k.upper()}] {item}")
    print(f"汇总: 拉取 {len(summary['fetched'])} / 跳过 {len(summary['skipped'])} / 失败 {len(summary['failed'])}")
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: 全部 PASS(11 + 1 = 12 tests)

- [ ] **Step 5: Commit**

```bash
git add tools/fetch-deps.py tools/tests/test_fetch.py
git commit -m "feat: fetch-deps.py pulls third-party sources into shared pool"
```

---

### Task 4: cmake_driver.py + build-deps.py(一键预编译)

**Files:**
- Create: `tools/deps_lib/cmake_driver.py`
- Create: `tools/build-deps.py`
- Create: `tools/tests/test_cmake_driver.py`

**Interfaces:**
- Consumes: `deps_lib.pool`(src_dir / build_dir / install_dir / is_built), `deps_lib.manifest`, `deps_lib.MINE_ROOT`。
- Produces:
  - `cmake_driver.configure_command(root, lib, variant) -> list[str]`(纯函数,便于测试)。
  - `cmake_driver.build_lib(root, lib, variant, jobs) -> tuple[bool, str]`(执行 configure+build+install,写 `.built`)。
  - `build-deps.py` CLI:`[--project DIR | --all] [--variant release|debug|all] [--jobs N]`。

- [ ] **Step 1: 写失败的测试** `tools/tests/test_cmake_driver.py`

```python
import os
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: `ImportError: No module named 'deps_lib.cmake_driver'`

- [ ] **Step 3: 实现 cmake_driver.py**

Create `tools/deps_lib/cmake_driver.py`:

```python
"""CMake + Ninja 统一预编译驱动。"""
from __future__ import annotations

import os
import subprocess

from . import pool
from .manifest import LibSpec


def configure_command(root: str, lib: LibSpec, variant: str) -> list:
    src = pool.src_dir(root, lib.name, lib.tag)
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    cmd = [
        "cmake", "-S", src, "-B", bdir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=" + variant,
        "-DCMAKE_INSTALL_PREFIX=" + idir,
    ]
    for opt in lib.options:
        cmd.append("-D" + opt)
    return cmd


def build_lib(root: str, lib: LibSpec, variant: str, jobs: int) -> tuple:
    """configure + build + install,成功后写 .built。返回 (ok, err_log)。"""
    bdir = pool.build_dir(root, lib.name, lib.tag, variant)
    idir = pool.install_dir(root, lib.name, lib.tag, variant)
    os.makedirs(bdir, exist_ok=True)
    os.makedirs(idir, exist_ok=True)

    cmds = [
        configure_command(root, lib, variant),
        ["cmake", "--build", bdir, "-j", str(jobs)],
        ["cmake", "--install", bdir],
    ]
    for cmd in cmds:
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return False, "\n".join(x for x in (r.stdout, r.stderr) if x).strip()

    with open(os.path.join(idir, ".built"), "w", encoding="utf-8") as f:
        f.write(f"variant={variant}\n")
    return True, ""
```

- [ ] **Step 4: 实现 build-deps.py**

Create `tools/build-deps.py`:

```python
#!/usr/bin/env python3
"""一键预编译三方库进池(默认 release + debug 双变体)。"""
from __future__ import annotations

import argparse
import os
import sys

from deps_lib import MINE_ROOT, cmake_driver, fetch, manifest, pool


def _collect_libs(args) -> list:
    return fetch.collect_libs(args)


def _target_variants(gm: dict, arg: str) -> list:
    avail = manifest.variants(gm)
    if arg == "all":
        return avail
    if arg not in avail:
        raise SystemExit(f"variant '{arg}' 不在清单 variants 中: {avail}")
    return [arg]


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="预编译三方库进共享池")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--project", metavar="DIR")
    g.add_argument("--all", action="store_true")
    p.add_argument("--variant", default="all", help="release|debug|all(默认 all)")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="ninja -j(默认 CPU 核数)")
    args = p.parse_args(argv)

    gm = manifest.load_global_manifest(MINE_ROOT)
    libs = _collect_libs(args)
    variants = _target_variants(gm, args.variant)
    if not libs:
        print("无需要编译的库。")
        return 0

    summary = {"built": [], "skipped": [], "failed": []}
    lock = pool.load_lock(MINE_ROOT)

    for lib in libs:
        for v in variants:
            key = f"{manifest.ver_dir(lib.name, lib.tag)} [{v}]"
            if pool.is_built(MINE_ROOT, lib.name, lib.tag, v):
                summary["skipped"].append(key)
                continue
            print(f"编译 {manifest.ver_dir(lib.name, lib.tag)} [{v}] …")
            ok, err = cmake_driver.build_lib(MINE_ROOT, lib, v, args.jobs)
            if ok:
                summary["built"].append(key)
                lock.setdefault(manifest.ver_dir(lib.name, lib.tag), {})
                lock[manifest.ver_dir(lib.name, lib.tag)].setdefault("built", {})
                lock[manifest.ver_dir(lib.name, lib.tag)]["built"][v] = True
            else:
                summary["failed"].append(key)
                print(f"  失败日志(尾部):\n{err[-2000:]}\n", file=sys.stderr)

    pool.save_lock(MINE_ROOT, lock)
    for k in ("built", "skipped", "failed"):
        for item in summary[k]:
            print(f"[{k.upper()}] {item}")
    print(f"汇总: 已编 {len(summary['built'])} / 跳过 {len(summary['skipped'])} / 失败 {len(summary['failed'])}")
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: 全部 PASS(12 + 1 = 13 tests)

- [ ] **Step 6: Commit**

```bash
git add tools/deps_lib/cmake_driver.py tools/build-deps.py tools/tests/test_cmake_driver.py
git commit -m "feat: build-deps.py prebuilds libraries into shared pool (release+debug)"
```

---

### Task 5: setup-env.sh(系统工具检测/安装)

**Files:**
- Create: `tools/setup-env.sh`

**Interfaces:**
- Consumes: 无
- Produces: `tools/setup-env.sh [--check | --help]`(默认探测+apt 补硬依赖)。

- [ ] **Step 1: 实现 setup-env.sh**

Create `tools/setup-env.sh`:

```bash
#!/usr/bin/env bash
# 系统工具链检测/安装:cmake / ninja / g++ / pkg-config / git / python3。
# 只装系统依赖,不碰三方库(拉取/编译由 fetch-deps.py / build-deps.py 负责)。
set -euo pipefail

info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }
has()  { command -v "$1" >/dev/null 2>&1; }

extract_version() {
  local s="$1"
  if [[ "$s" =~ ([0-9]+(\.[0-9]+)+) ]]; then printf '%s' "${BASH_REMATCH[1]}"
  elif [[ "$s" =~ ([0-9]+) ]]; then printf '%s' "${BASH_REMATCH[1]}"; fi
}
ver_seg() { local v="$1" i="$2" s; s="$(printf '%s' "$v" | cut -d. -f"$((i+1))" 2>/dev/null || true)"; case "$s" in ''|*[!0-9]*) printf 0;; *) printf '%d' "$((10#$s))";; esac; }
ver_ge() { local a="$1" b="$2" i sa sb; for i in 0 1 2 3; do sa="$(ver_seg "$a" "$i")"; sb="$(ver_seg "$b" "$i")"; [ "$sa" -gt "$sb" ] && return 0; [ "$sa" -lt "$sb" ] && return 1; done; return 0; }

HARD_MISS=0
MISS_DETAILS=()

chk() { # name 最低版本 探测命令 详情
  local name="$1" min="$2" v vn ok=1
  if ! has "$(printf '%s' "$3" | awk '{print $1}')"; then ok=0; fi
  if [ "$ok" = 1 ]; then
    v="$($3 2>/dev/null | head -n1 || true)"
    vn="$(extract_version "$v")"
    if [ -n "$min" ] && [ -n "$vn" ] && ! ver_ge "$vn" "$min"; then ok=0; fi
  fi
  if [ "$ok" = 0 ]; then HARD_MISS=$((HARD_MISS+1)); MISS_DETAILS+=("$name(需 ${min:-任意})"); fi
  if [ "$ok" = 1 ]; then printf '[OK]   %s: %s\n' "$name" "${v:-已安装}"; else printf '[MISS] %s: 缺失或版本过低\n' "$name"; fi
}

probe() {
  info "=== 系统工具链探测 ==="
  HARD_MISS=0; MISS_DETAILS=()
  chk "cmake"      "3.22" "cmake --version"
  chk "ninja"      ""     "ninja --version"
  chk "g++"        "11"   "g++ --version"
  chk "pkg-config" ""     "pkg-config --version"
  chk "git"        ""     "git --version"
  chk "python3"    "3.8"  "python3 --version"
}

print_help() {
  cat <<'EOF'
用法: tools/setup-env.sh [--check] [--help]

  检测/安装系统工具链(cmake/ninja/g++/pkg-config/git/python3)。
  --check    只探测不安装;硬依赖缺失时非零退出。
  -h,--help  打印本帮助。
  默认       探测;硬依赖缺失且存在 apt-get 时 sudo 自动安装,否则打印指引。
EOF
}

attempt_apt() {
  if ! has apt-get; then
    warn "未检测到 apt-get,无法自动安装。请手动安装缺失项: ${MISS_DETAILS[*]}"
    return 1
  fi
  info "将 sudo apt-get 安装缺失硬依赖(需 root 权限,Ctrl-C 取消):"
  sudo apt-get update && sudo apt-get install -y \
    cmake ninja-build build-essential pkg-config git python3
}

main() {
  local mode="install"
  if [ "$#" -gt 1 ]; then err "参数过多: $*"; exit 2; fi
  if [ "$#" -eq 1 ]; then
    case "$1" in
      --check) mode="check" ;;
      -h|--help) print_help; exit 0 ;;
      *) err "未知参数: $1"; exit 2 ;;
    esac
  fi

  probe
  if [ "$HARD_MISS" -eq 0 ]; then
    info "硬依赖齐全。"
    exit 0
  fi

  if [ "$mode" = "check" ]; then
    err "硬依赖缺失 ${HARD_MISS} 项: ${MISS_DETAILS[*]}"
    exit 1
  fi

  if attempt_apt; then
    info "安装完成,重新探测:"
    probe
    [ "$HARD_MISS" -eq 0 ] && exit 0
    err "安装后仍有缺失: ${MISS_DETAILS[*]}"
    exit 1
  fi
  exit 1
}

main "$@"
```

- [ ] **Step 2: 加可执行权限 + 冒烟测试 --check**

Run: `chmod +x /home/qiansenwei/workspace/Mine/tools/setup-env.sh && /home/qiansenwei/workspace/Mine/tools/setup-env.sh --check`
Expected: 输出 `[OK]` 各项、`硬依赖齐全。`,exit 0(本机已装齐)。

- [ ] **Step 3: Commit**

```bash
git add tools/setup-env.sh
git commit -m "feat: setup-env.sh detects/installs system toolchain"
```

---

### Task 6: templates + new-project.py(项目脚手架)

**Files:**
- Create: `tools/templates/cpp/CMakeLists.txt`
- Create: `tools/templates/cpp/CMakePresets.json`
- Create: `tools/templates/cpp/deps.yaml.tmpl`
- Create: `tools/templates/cpp/.gitignore`
- Create: `tools/templates/cpp/README.md`
- Create: `tools/templates/cpp/src/main.cpp`
- Create: `tools/templates/cpp/scripts/fetch-deps.py`
- Create: `tools/templates/cpp/scripts/build-deps.py`
- Create: `tools/templates/python/pyproject.toml`
- Create: `tools/templates/python/deps.yaml.tmpl`
- Create: `tools/templates/python/.gitignore`
- Create: `tools/templates/python/README.md`
- Create: `tools/templates/python/src/__init__.py`
- Create: `tools/templates/web/package.json`
- Create: `tools/templates/web/deps.yaml.tmpl`
- Create: `tools/templates/web/.gitignore`
- Create: `tools/templates/web/README.md`
- Create: `tools/templates/web/src/index.html`
- Create: `tools/new-project.py`
- Create: `tools/tests/test_new_project.py`

**Interfaces:**
- Consumes: `deps_lib.MINE_ROOT`、`deps_lib.manifest`(resolve_libs 校验库名存在性)。
- Produces:
  - `new_project.render_template(src, dst, ctx) -> None`(递归复制+占位符替换)。
  - CLI `python3 tools/new-project.py <语言> <项目名> [--libs fmt,glm]`。

- [ ] **Step 1: 写失败的测试** `tools/tests/test_new_project.py`

```python
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/
import new_project as np_mod


class TestRenderTemplate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_render_replaces_placeholder(self):
        src = os.path.join(self.tmp.name, "src")
        dst = os.path.join(self.tmp.name, "dst")
        os.makedirs(src)
        with open(os.path.join(src, "main.txt"), "w") as f:
            f.write("hello {{NAME}}")
        np_mod.render_template(src, dst, {"NAME": "world"})
        with open(os.path.join(dst, "main.txt")) as f:
            self.assertEqual(f.read(), "hello world")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: `ImportError: No module named 'new_project'`

- [ ] **Step 3: 写 cpp 模板文件**

Create `tools/templates/cpp/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22)
project({{PROJECT_NAME}} CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 池 install 前缀(优先取缓存变量,默认按 MINE_ROOT 推导)
set(MINE_ROOT "$ENV{MINE_ROOT}" CACHE PATH "Mine workspace root")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
list(APPEND CMAKE_PREFIX_PATH "${MINE_ROOT}/third_party/_install")

{{DEPS_FIND}}

add_executable({{PROJECT_NAME}} src/main.cpp)
{{DEPS_LINK}}
```

Create `tools/templates/cpp/CMakePresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "debug",   "displayName": "Debug",   "binaryDir": "${sourceDir}/build/debug",   "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug",   "CMAKE_PREFIX_PATH": "${sourceDir}/../third_party/_install" } },
    { "name": "release", "displayName": "Release", "binaryDir": "${sourceDir}/build/release", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "CMAKE_PREFIX_PATH": "${sourceDir}/../third_party/_install" } }
  ],
  "buildPresets": [
    { "name": "debug",   "configurePreset": "debug" },
    { "name": "release", "configurePreset": "release" }
  ]
}
```

Create `tools/templates/cpp/deps.yaml.tmpl`:

```yaml
use: [{{DEPS}}]
```

Create `tools/templates/cpp/.gitignore`:

```gitignore
build/
*.o
*.a
*.so
```

Create `tools/templates/cpp/README.md`:

```markdown
# {{PROJECT_NAME}}

C/C++ 项目。

## 依赖
- 三方库通过 `deps.yaml` 的 `use` 列表声明(引用全局清单 `third_party/deps.yaml`)。
- 拉取/预编译:`python3 ../tools/fetch-deps.py --project .`、`python3 ../tools/build-deps.py --project .`

## 构建
    cmake --preset release
    cmake --build --preset release
```

Create `tools/templates/cpp/src/main.cpp`:

```cpp
#include <iostream>

int main() {
    std::cout << "hello from {{PROJECT_NAME}}" << std::endl;
    return 0;
}
```

Create `tools/templates/cpp/scripts/fetch-deps.py`:

```python
#!/usr/bin/env python3
"""薄封装:转发到 tools/fetch-deps.py,固定 --project 指向本项目。"""
import os
import shlex
import sys

_here = os.path.dirname(os.path.abspath(__file__))   # <proj>/scripts
_proj = os.path.dirname(_here)                       # <proj>
_root = os.path.dirname(_proj)                       # Mine
_tool = os.path.join(_root, "tools", "fetch-deps.py")
_args = [sys.executable, _tool, "--project", _proj] + sys.argv[1:]
sys.exit(os.system(" ".join(shlex.quote(a) for a in _args)))
```

Create `tools/templates/cpp/scripts/build-deps.py`:

```python
#!/usr/bin/env python3
"""薄封装:转发到 tools/build-deps.py,固定 --project 指向本项目。"""
import os
import shlex
import sys

_here = os.path.dirname(os.path.abspath(__file__))   # <proj>/scripts
_proj = os.path.dirname(_here)                       # <proj>
_root = os.path.dirname(_proj)                       # Mine
_tool = os.path.join(_root, "tools", "build-deps.py")
_args = [sys.executable, _tool, "--project", _proj] + sys.argv[1:]
sys.exit(os.system(" ".join(shlex.quote(a) for a in _args)))
```

- [ ] **Step 4: 写 python / web 模板文件**

Create `tools/templates/python/pyproject.toml`:

```toml
[project]
name = "{{PROJECT_NAME}}"
version = "0.1.0"
requires-python = ">=3.8"

[build-system]
requires = ["setuptools>=61.0"]
build-backend = "setuptools.build_meta"
```

Create `tools/templates/python/deps.yaml.tmpl`:

```yaml
use: [{{DEPS}}]
```

Create `tools/templates/python/.gitignore`:

```gitignore
__pycache__/
*.pyc
build/
dist/
*.egg-info/
```

Create `tools/templates/python/README.md`:

```markdown
# {{PROJECT_NAME}}

Python 项目。
```

Create `tools/templates/python/src/__init__.py`(空文件)。

Create `tools/templates/web/package.json`:

```json
{
  "name": "{{PROJECT_NAME}}",
  "version": "0.1.0",
  "private": true
}
```

Create `tools/templates/web/deps.yaml.tmpl`:

```yaml
use: [{{DEPS}}]
```

Create `tools/templates/web/.gitignore`:

```gitignore
node_modules/
dist/
```

Create `tools/templates/web/README.md`:

```markdown
# {{PROJECT_NAME}}

前端项目(占位模板)。
```

Create `tools/templates/web/src/index.html`:

```html
<!doctype html>
<html>
  <head><meta charset="utf-8"><title>{{PROJECT_NAME}}</title></head>
  <body>hello from {{PROJECT_NAME}}</body>
</html>
```

- [ ] **Step 5: 实现 new-project.py**

Create `tools/new-project.py`:

```python
#!/usr/bin/env python3
"""新建项目脚手架:从模板生成骨架 + 写 deps.yaml + 薄封装脚本。"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import sys

from deps_lib import MINE_ROOT, manifest

LANGS = {"cpp", "python", "web"}
_NAME_RE = re.compile(r"^[A-Za-z0-9_-]+$")


def render_template(src: str, dst: str, ctx: dict) -> None:
    """递归复制 src 到 dst,替换文本文件中的 {{KEY}} 占位符。"""
    os.makedirs(dst, exist_ok=True)
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        target_dir = os.path.join(dst, rel)
        os.makedirs(target_dir, exist_ok=True)
        for f in files:
            src_path = os.path.join(root, f)
            dst_path = os.path.join(target_dir, f)
            if f.endswith(".tmpl"):
                dst_path = dst_path[:-5]
                with open(src_path, "r", encoding="utf-8") as fh:
                    content = fh.read()
                for k, v in ctx.items():
                    content = content.replace("{{" + k + "}}", v)
                with open(dst_path, "w", encoding="utf-8") as fh:
                    fh.write(content)
            else:
                with open(src_path, "r", encoding="utf-8") as fh:
                    content = fh.read()
                for k, v in ctx.items():
                    content = content.replace("{{" + k + "}}", v)
                with open(dst_path, "w", encoding="utf-8") as fh:
                    fh.write(content)
    # 复制空目录(如 python/src 的包目录占位已含 __init__.py,此处兜底)
    for root, dirs, _files in os.walk(src):
        rel = os.path.relpath(root, src)
        target_dir = os.path.join(dst, rel)
        for d in dirs:
            os.makedirs(os.path.join(target_dir, d), exist_ok=True)


def _build_find_link(use: list) -> tuple:
    """按库名生成 find_package 与 target_link_libraries 片段(初版映射表)。"""
    # 常见库名 -> CMake package / target(无 config 的库走 add_subdirectory 之外均需用户补充)
    find_lines = []
    link_lines = []
    for name in use:
        find_lines.append(f"find_package({name} CONFIG REQUIRED)")
        link_lines.append(f"{name}::{name}")
    return "\n".join(find_lines), " ".join(link_lines)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="新建项目脚手架")
    p.add_argument("lang", choices=sorted(LANGS), help="项目类型")
    p.add_argument("name", help="项目名(目录名)")
    p.add_argument("--libs", default="", help="逗号分隔库名,写入 deps.yaml use(默认空)")
    args = p.parse_args(argv)

    if not _NAME_RE.match(args.name):
        print(f"错误: 项目名 '{args.name}' 非法,仅允许 [A-Za-z0-9_-]。", file=sys.stderr)
        return 2

    dst = os.path.join(MINE_ROOT, args.name)
    if os.path.exists(dst):
        print(f"错误: 目录已存在 {dst}", file=sys.stderr)
        return 2

    use = [x.strip() for x in args.libs.split(",") if x.strip()]
    # 校验库名在全局清单存在
    gm = manifest.load_global_manifest(MINE_ROOT)
    manifest.resolve_libs(gm, use)  # 未定义会抛 KeyError

    find_frag, link_frag = _build_find_link(use)
    ctx = {
        "PROJECT_NAME": args.name,
        "DEPS": ", ".join(use),
        "DEPS_FIND": find_frag,
        "DEPS_LINK": f"target_link_libraries({args.name} PRIVATE {link_frag})" if use else "",
    }

    src_tpl = os.path.join(MINE_ROOT, "tools", "templates", args.lang)
    render_template(src_tpl, dst, ctx)

    print(f"已创建项目 {dst}")
    print(f"  依赖: {use or '无'}")
    if use:
        print("  下一步:")
        print(f"    python3 tools/fetch-deps.py --project {args.name}")
        print(f"    python3 tools/build-deps.py --project {args.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 6: 跑测试确认通过**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: 全部 PASS(13 + 1 = 14 tests)

- [ ] **Step 7: 手动冒烟:生成一个 cpp 项目并检查**

Run:
```bash
python3 tools/new-project.py cpp smoke-demo --libs fmt,glm
ls -R smoke-demo
```
Expected: 目录含 `CMakeLists.txt`、`CMakePresets.json`、`deps.yaml`(内容 `use: [fmt, glm]`)、`src/main.cpp`、`scripts/fetch-deps.py`、`scripts/build-deps.py`;`CMakeLists.txt` 已含 `find_package(fmt CONFIG REQUIRED)`。检查后删除 `smoke-demo`(验证用,不入库)。

- [ ] **Step 8: Commit**

```bash
git add tools/templates tools/new-project.py tools/tests/test_new_project.py
git commit -m "feat: new-project.py scaffolds cpp/python/web projects from templates"
```

---

### Task 7: README + 端到端验证 + 最终提交

**Files:**
- Create: `tools/README.md`
- Create: `README.md`(Mine 根)

**Interfaces:**
- Consumes: 全部已交付产物。
- Produces: 无新代码,文档 + 端到端验证结论。

- [ ] **Step 1: 写 tools/README.md**

Create `tools/README.md`:

```markdown
# tools —— 工作空间环境工具

职责:新机器快速搭建环境 —— 检测系统工具、拉取三方库源码、统一预编译进共享池。
**构建/编译是项目自己的事**,tools 只负责把库备好。

## 脚本
| 脚本 | 作用 |
|---|---|
| `setup-env.sh` | 检测/安装系统工具链(cmake/ninja/g++/pkg-config/git/python3) |
| `fetch-deps.py` | 拉取三方库源码进 `third_party/_src/`(只拉不编) |
| `build-deps.py` | 预编译三方库进 `third_party/_install/<name>-<ver>/<variant>/` |
| `new-project.py` | 新建项目骨架(cpp / python / web) |

## 常用命令
    # 新机器还原
    tools/setup-env.sh --check
    tools/fetch-deps.py --all
    tools/build-deps.py --all          # release + debug 双变体

    # 只处理某项目
    tools/fetch-deps.py --project <项目>
    tools/build-deps.py --project <项目>

    # 新建项目
    tools/new-project.py cpp myapp --libs fmt,glm

## 约定
- 全局清单 `third_party/deps.yaml` 是三方库唯一定义处;项目 `deps.yaml` 只 `use` 引用库名。
- 池目录:`_src/<name>-<ver>` 源码、`_install/<name>-<ver>/<variant>` 产物;`.built` 文件标记已编译。
- 源码/产物全部 gitignore,仓库只留清单 + 脚本。
```

- [ ] **Step 2: 写根 README.md**

Create `README.md`:

```markdown
# Mine 工作空间

快速新开任意项目的工作空间:一项目一文件夹,三方库全局共享、只拉一次、只编一次。

## 结构
    Mine/
    ├── tools/           环境工具(拉取/预编译/脚手架)
    ├── third_party/     全局共享三方库池(清单 deps.yaml + 源码/产物,后者 gitignore)
    └── <项目>/          一项目一文件夹(由 tools/new-project.py 生成)

## 快速开始
    # 1. 还原环境(新机器)
    tools/setup-env.sh
    tools/fetch-deps.py --all
    tools/build-deps.py --all

    # 2. 新建项目
    tools/new-project.py cpp myapp --libs fmt,glm
    cd myapp
    cmake --preset release && cmake --build --preset release

## 详见
- 设计:`docs/superpowers/specs/2026-08-23-workspace-bootstrap-design.md`
- 工具用法:`tools/README.md`
```

- [ ] **Step 3: 端到端验证(网络环境允许时)**

Run:
```bash
cd /home/qiansenwei/workspace/Mine
python3 tools/fetch-deps.py --all
python3 tools/build-deps.py --all --variant release
ls third_party/_install/fmt-10.2.1/release/.built
```
Expected: fmt、glm 拉取成功;fmt release 编译出 `.built`;二次运行 `build-deps.py --all --variant release` 全 `SKIP`。

> 若当前无外网(无法 clone github),本步骤降级为:仅验证 `--project`/`--all` 参数解析与空清单分支不报错,并在 README 备注「首次运行需外网」。

- [ ] **Step 4: 跑全部单元测试**

Run: `cd /home/qiansenwei/workspace/Mine && python3 -m unittest discover -s tools/tests -v`
Expected: 14 tests 全部 PASS。

- [ ] **Step 5: 确认 git 忽略生效 + 最终提交**

Run: `git status --short`
Expected: 不出现 `third_party/_src/`、`_install/`、`_build/`、`.pool.lock.json`。

```bash
git add README.md tools/README.md
git commit -m "docs: workspace README + end-to-end verification"
```

---

## 完成后验收(对照 Spec §13)

1. `tools/fetch-deps.py --all` + `tools/build-deps.py --all` 后,`_install/<name>-<ver>/{release,debug}/.built` 均存在。
2. 二次运行 fetch/build → 全部 `SKIP`,无重复拉取/编译。
3. `tools/new-project.py cpp <项目>` 生成骨架,`cmake --preset release` 能链接池产物并运行。
4. `git status` 干净,`_src/`、`_install/`、`_build/`、`.pool.lock.json` 均被忽略。
