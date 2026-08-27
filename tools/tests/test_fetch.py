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


class TestEnsureSwiftshaderSubmodules(unittest.TestCase):
    def _make_swiftshader(self, root, glslang_present=False):
        src = os.path.join(root, "third_party", "_src", "swiftshader-master")
        os.makedirs(os.path.join(src, "third_party"), exist_ok=True)
        with open(os.path.join(src, ".gitmodules"), "w") as f:
            f.write('[submodule "third_party/glslang"]\n\tpath = third_party/glslang\n')
        if glslang_present:
            os.makedirs(os.path.join(src, "third_party", "glslang", ".git"))
        return src

    def test_already_present_skips(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self._make_swiftshader(tmp.name, glslang_present=True)
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            ok, err = fetch_mod.ensure_swiftshader_submodules(tmp.name)
        self.assertTrue(ok)
        run.assert_not_called()

    def test_shallow_submodule_update_success(self):
        # 回归:glslang 子模块必须 --depth 1 浅拉(全量数百 MB 在弱网下 502),就位带 .git 后
        # SwiftShader CMake 的 InitSubmodule 检测到 .git 直接跳过,configure 不再走网络。
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        src = self._make_swiftshader(tmp.name)
        glslang_git = os.path.join(src, "third_party", "glslang", ".git")

        def _fake_run(cmd, **kw):
            os.makedirs(glslang_git, exist_ok=True)
            return mock.Mock(returncode=0, stdout="", stderr="")

        with mock.patch.object(fetch_mod.subprocess, "run", side_effect=_fake_run) as run:
            ok, err = fetch_mod.ensure_swiftshader_submodules(tmp.name)
        self.assertTrue(ok, err)
        self.assertEqual(run.call_count, 1)
        args, _ = run.call_args
        self.assertIn("--depth", args[0])
        self.assertIn("third_party/glslang", args[0])

    def test_retries_then_fails(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self._make_swiftshader(tmp.name)
        fake = mock.Mock(returncode=128, stdout="", stderr="HTTP 502")
        with mock.patch.object(fetch_mod.subprocess, "run", return_value=fake) as run:
            ok, err = fetch_mod.ensure_swiftshader_submodules(tmp.name)
        self.assertFalse(ok)
        self.assertEqual(run.call_count, 3)
        self.assertIn("HTTP 502", err)


if __name__ == "__main__":
    unittest.main()
