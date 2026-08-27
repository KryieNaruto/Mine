import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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


class TestSubmoduleReady(unittest.TestCase):
    """_submodule_ready:现代 git 子模块形态(.git 是文件)必须判定为就位。"""

    def _make_src(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        src = os.path.join(tmp.name, "third_party", "_src", "swiftshader-master")
        os.makedirs(os.path.join(src, "third_party", "glslang"))
        return src

    def test_git_file_counts_as_ready(self):
        # 回归:浅拉成功后 .git 是「文件」(指向父仓 .git/modules),不是目录。
        # 旧实现用 os.path.isdir 判定 → 已拉取仍误报失败,setup 中断(本机复现)。
        src = self._make_src()
        with open(os.path.join(src, "third_party", "glslang", ".git"), "w") as f:
            f.write("gitdir: ../../.git/modules/third_party/glslang")
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            run.return_value = mock.Mock(
                returncode=0,
                stdout=" 2b2523fb951f63f072cfba514c26f2feea5f4329 third_party/glslang (2b2523f)\n",
            )
            self.assertTrue(fetch_mod._submodule_ready(src))
        args, _ = run.call_args
        self.assertIn("status", args[0])

    def test_plus_prefix_counts_as_ready(self):
        # + 前缀 = 已检出但 commit 与索引不同,仍可用
        src = self._make_src()
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            run.return_value = mock.Mock(returncode=0, stdout="+abc123 def third_party/glslang\n")
            self.assertTrue(fetch_mod._submodule_ready(src))

    def test_dash_prefix_not_ready(self):
        src = self._make_src()
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            run.return_value = mock.Mock(returncode=0, stdout="-abc123 def third_party/glslang\n")
            self.assertFalse(fetch_mod._submodule_ready(src))

    def test_missing_worktree_not_ready(self):
        src = self._make_src()
        os.rmdir(os.path.join(src, "third_party", "glslang"))
        self.assertFalse(fetch_mod._submodule_ready(src))  # 目录缺失直接 False,不触发 git


class TestEnsureSwiftshaderSubmodules(unittest.TestCase):
    def _make_src(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        src = os.path.join(tmp.name, "third_party", "_src", "swiftshader-master")
        os.makedirs(os.path.join(src, "third_party"))
        with open(os.path.join(src, ".gitmodules"), "w") as f:
            f.write('[submodule "third_party/glslang"]\n\tpath = third_party/glslang\n')
        return tmp.name, src

    def test_already_present_skips_update(self):
        # 已就位 → 不触发 submodule update
        root, src = self._make_src()
        with mock.patch.object(fetch_mod, "_submodule_ready", return_value=True) as ready, \
             mock.patch.object(fetch_mod.subprocess, "run") as run:
            ok, err = fetch_mod.ensure_swiftshader_submodules(root)
        self.assertTrue(ok, err)
        ready.assert_called_once()
        run.assert_not_called()

    def test_shallow_submodule_update_success(self):
        # 回归:glslang 子模块必须 --depth 1 浅拉(全量数百 MB 在弱网下 502);未就位 → update
        root, src = self._make_src()

        def _fake_run(cmd, **kw):
            self.assertIn("--depth", cmd)
            self.assertIn("third_party/glslang", cmd)
            return mock.Mock(returncode=0, stdout="", stderr="")

        with mock.patch.object(fetch_mod, "_submodule_ready", side_effect=[False, True]) as ready, \
             mock.patch.object(fetch_mod, "_reset_submodule") as reset, \
             mock.patch.object(fetch_mod.subprocess, "run", side_effect=_fake_run) as run:
            ok, err = fetch_mod.ensure_swiftshader_submodules(root)
        self.assertTrue(ok, err)
        self.assertEqual(run.call_count, 1)    # 仅 update 一次
        self.assertEqual(ready.call_count, 2)  # 更新前后各探测一次
        reset.assert_called_once()

    def test_retries_then_fails(self):
        root, src = self._make_src()
        fake = mock.Mock(returncode=128, stdout="", stderr="HTTP 502")
        with mock.patch.object(fetch_mod, "_submodule_ready", return_value=False), \
             mock.patch.object(fetch_mod, "_reset_submodule"), \
             mock.patch.object(fetch_mod.subprocess, "run", return_value=fake) as run:
            ok, err = fetch_mod.ensure_swiftshader_submodules(root)
        self.assertFalse(ok)
        self.assertEqual(run.call_count, 3)
        self.assertIn("HTTP 502", err)

    def test_reset_removes_broken_gitdir_and_worktree(self):
        # 中断残留(坏 .git 文件 + 半成品 .git/modules)必须清掉,否则重试沿用坏状态
        root, src = self._make_src()
        sub = os.path.join(src, "third_party", "glslang")
        mod = os.path.join(src, ".git", "modules", "third_party", "glslang")
        os.makedirs(sub)
        os.makedirs(mod)
        with open(os.path.join(sub, ".git"), "w") as f:
            f.write("gitdir: ../../.git/modules/third_party/glslang")
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            run.return_value = mock.Mock(returncode=0, stdout="", stderr="")
            fetch_mod._reset_submodule(src, sub)
        self.assertFalse(os.path.exists(sub))
        self.assertFalse(os.path.exists(mod))


if __name__ == "__main__":
    unittest.main()
