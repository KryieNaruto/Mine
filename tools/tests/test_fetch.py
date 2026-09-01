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
    # 仓库本地 user 配置(CI/沙箱可能无全局 user.email/name,commit 会 128)
    subprocess.run(["git", "-C", path, "config", "user.email", "t@t"], check=True)
    subprocess.run(["git", "-C", path, "config", "user.name", "t"], check=True)
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

        # 本地 repo 非 github URL,禁用镜像探测(避免测试碰网络/给本地路径挂镜像前缀)
        with mock.patch.object(fetch_mod.mirror, "pick_mirror_prefix", return_value=None):
            ok, commit = fetch_mod.clone_lib(self.root, lib)
        self.assertTrue(ok, msg=commit)
        self.assertTrue(commit)
        # 已拉取则跳过:再次 clone 到已存在目录应报错
        ok2, _ = fetch_mod.clone_lib(self.root, lib)
        self.assertFalse(ok2)


class TestCloneLibMirror(unittest.TestCase):
    """clone_lib 镜像路径:镜像优先,官方源只在镜像失败时才出现。"""

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
            ok, msg = fetch_mod.clone_lib(self.tmp.name, self.lib)
        self.assertTrue(ok, msg)

    def test_mirror_clone_success_skips_original(self):
        with mock.patch("deps_lib.fetch.mirror.pick_mirror_prefix", return_value="https://ghproxy.net/"), \
             mock.patch("deps_lib.fetch.subprocess.run",
                        side_effect=self._fake_run({
                            "ghproxy.net/https://github.com/fmtlib/fmt.git": 0,
                            "rev-parse HEAD": 0,
                        })) as run:
            ok, _ = fetch_mod.clone_lib(self.tmp.name, self.lib)
        self.assertTrue(ok)
        # c.args[0] 是命令列表,展平所有调用的全部 token 拼成一条串
        joined = " ".join(arg for c in run.call_args_list for arg in c.args[0])
        self.assertIn("ghproxy.net", joined)
        self.assertNotIn("ghproxy.net", joined.replace("ghproxy.net/https://github.com/fmtlib/fmt.git", "", 1))
        # 官方源只在镜像失败时才出现
        self.assertEqual(joined.count("https://github.com/fmtlib/fmt.git"), 1)


class TestSubmoduleReady(unittest.TestCase):
    """_submodule_ready:现代 git 子模块形态(.git 是文件)必须判定为就位。"""

    def _make_src(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        src = os.path.join(tmp.name, "third_party", "_src", "swiftshader-master")
        os.makedirs(os.path.join(src, "third_party", "glslang"))
        with open(os.path.join(src, "third_party", "glslang", ".git"), "w") as f:
            f.write("gitdir: ../../.git/modules/third_party/glslang")
        return src

    def test_git_file_counts_as_ready(self):
        # 回归:浅拉成功后 .git 是「文件」(指向父仓 .git/modules),不是目录。
        # 旧实现用 os.path.isdir 判定 → 已拉取仍误报失败,setup 中断(本机复现)。
        src = self._make_src()
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
        gl = os.path.join(src, "third_party", "glslang")
        os.remove(os.path.join(gl, ".git"))
        os.rmdir(gl)
        self.assertFalse(fetch_mod._submodule_ready(src))  # 目录缺失直接 False,不触发 git

    def test_no_gitfile_not_ready(self):
        # 目录在但既无 .git 文件也无 .git 目录 → 未就位,不触发 git
        src = self._make_src()
        os.remove(os.path.join(src, "third_party", "glslang", ".git"))
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            self.assertFalse(fetch_mod._submodule_ready(src))
        run.assert_not_called()

    def test_standalone_git_dir_counts_as_ready(self):
        # 独立克隆产物:.git 是目录,git rev-parse --git-dir 成功即就位
        src = self._make_src()
        os.remove(os.path.join(src, "third_party", "glslang", ".git"))
        os.makedirs(os.path.join(src, "third_party", "glslang", ".git"))
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            run.return_value = mock.Mock(returncode=0, stdout=os.path.join(src, ".git"), stderr="")
            self.assertTrue(fetch_mod._submodule_ready(src))
        args, _ = run.call_args
        self.assertIn("rev-parse", args[0])

    def test_standalone_broken_git_dir_not_ready(self):
        src = self._make_src()
        os.remove(os.path.join(src, "third_party", "glslang", ".git"))
        os.makedirs(os.path.join(src, "third_party", "glslang", ".git"))
        with mock.patch.object(fetch_mod.subprocess, "run") as run:
            run.return_value = mock.Mock(returncode=128, stdout="", stderr="fatal")
            self.assertFalse(fetch_mod._submodule_ready(src))


class TestEnsureSwiftshaderSubmodules(unittest.TestCase):
    """独立克隆方案:绕开 git submodule 机制,按固定 commit 直接克隆 glslang。"""

    def _make_src(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        src = os.path.join(tmp.name, "third_party", "_src", "swiftshader-master")
        os.makedirs(os.path.join(src, "third_party"))
        with open(os.path.join(src, ".gitmodules"), "w") as f:
            f.write('[submodule "third_party/glslang"]\n\tpath = third_party/glslang\n')
        return tmp.name, src

    def test_already_present_skips_clone(self):
        root, src = self._make_src()
        with mock.patch.object(fetch_mod, "_submodule_ready", return_value=True) as ready, \
             mock.patch.object(fetch_mod, "_clone_glslang") as clone:
            ok, err = fetch_mod.ensure_swiftshader_submodules(root)
        self.assertTrue(ok, err)
        ready.assert_called_once()
        clone.assert_not_called()

    def test_swiftshader_not_fetched_skips(self):
        # swiftshader 本身未拉取 → 直接放行,不折腾 glslang
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        with mock.patch.object(fetch_mod, "_submodule_ready", return_value=False), \
             mock.patch.object(fetch_mod, "_clone_glslang") as clone:
            ok, err = fetch_mod.ensure_swiftshader_submodules(tmp.name)
        self.assertTrue(ok, err)
        clone.assert_not_called()

    def test_missing_glslang_commit_fails(self):
        root, src = self._make_src()
        with mock.patch.object(fetch_mod, "_submodule_ready", return_value=False), \
             mock.patch.object(fetch_mod, "_glslang_commit", return_value=""), \
             mock.patch.object(fetch_mod, "_clone_glslang") as clone:
            ok, err = fetch_mod.ensure_swiftshader_submodules(root)
        self.assertFalse(ok)
        self.assertIn("固定 commit", err)
        clone.assert_not_called()

    def test_mirror_fails_then_official(self):
        # 镜像克隆失败 → 退官方直连(独立克隆),并清陈旧 insteadOf 改写
        root, src = self._make_src()
        sub = os.path.join(src, "third_party", "glslang")
        results = [(False, "mirror 502"), (True, "")]
        with mock.patch.object(fetch_mod, "_submodule_ready", return_value=False), \
             mock.patch.object(fetch_mod, "_glslang_commit", return_value="abc123"), \
             mock.patch.object(fetch_mod.mirror, "pick_mirror_prefix",
                               return_value="https://gh-proxy.com/"), \
             mock.patch.object(fetch_mod, "_clear_mirror_rewrites") as clear, \
             mock.patch.object(fetch_mod, "_clone_glslang", side_effect=results) as clone:
            ok, err = fetch_mod.ensure_swiftshader_submodules(root)
        self.assertTrue(ok, err)
        self.assertEqual(clone.call_count, 2)
        calls = [c.args for c in clone.call_args_list]
        self.assertEqual(calls[0], (src, sub, "abc123", "https://gh-proxy.com/"))
        self.assertEqual(calls[1], (src, sub, "abc123", None))  # 镜像失败退官方
        clear.assert_called()

    def test_all_clone_failures(self):
        root, src = self._make_src()
        with mock.patch.object(fetch_mod, "_submodule_ready", return_value=False), \
             mock.patch.object(fetch_mod, "_glslang_commit", return_value="abc123"), \
             mock.patch.object(fetch_mod.mirror, "pick_mirror_prefix", return_value=None), \
             mock.patch.object(fetch_mod, "_clear_mirror_rewrites"), \
             mock.patch.object(fetch_mod, "_clone_glslang", return_value=(False, "net err")):
            ok, err = fetch_mod.ensure_swiftshader_submodules(root)
        self.assertFalse(ok)
        self.assertIn("net err", err)


class TestCloneGlslang(unittest.TestCase):
    """_clone_glslang:独立克隆命令序列 + 镜像优先/官方兜底。"""

    def _make_src(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        src = os.path.join(tmp.name, "third_party", "_src", "swiftshader-master")
        return tmp.name, src, os.path.join(src, "third_party", "glslang")

    def _stateful_run(self, sub, commit, fail_first_fetch=True):
        """模拟独立克隆命令序列;fail_first_fetch=True 时首次 fetch(镜像)失败。"""
        fetch_count = 0

        def _run(args, **kw):
            nonlocal fetch_count
            joined = " ".join(args)
            if "init -q" in joined:
                os.makedirs(sub, exist_ok=True)
                return mock.Mock(returncode=0, stdout="", stderr="")
            if "remote get-url" in joined:
                return mock.Mock(returncode=1, stdout="", stderr="")
            if "remote add origin" in joined or "remote set-url" in joined:
                return mock.Mock(returncode=0, stdout="", stderr="")
            if "fetch --depth 1" in joined:
                fetch_count += 1
                if fail_first_fetch and fetch_count == 1:
                    return mock.Mock(returncode=128, stdout="", stderr="mirror 502")
                return mock.Mock(returncode=0, stdout="", stderr="")
            if "checkout" in joined:
                return mock.Mock(returncode=0, stdout="", stderr="")
            if "rev-parse HEAD" in joined:
                return mock.Mock(returncode=0, stdout=commit + "\n", stderr="")
            raise AssertionError(f"未预期的命令: {joined}")
        return _run

    def test_mirror_first_official_fallback(self):
        root, src, sub = self._make_src()
        with mock.patch.object(fetch_mod.subprocess, "run",
                               side_effect=self._stateful_run(sub, "abc123")) as run:
            ok, err = fetch_mod._clone_glslang(src, sub, "abc123", "https://gh-proxy.com/")
        self.assertTrue(ok, err)
        joined = " ".join(a for c in run.call_args_list for a in c.args[0])
        # 镜像 URL 进 remote add,官方 URL 也出现,且 fetch 两次(镜像失败→官方)
        self.assertIn("remote add origin https://gh-proxy.com/https://github.com/"
                      "KhronosGroup/glslang.git", joined)
        self.assertIn("remote add origin https://github.com/KhronosGroup/glslang.git", joined)
        self.assertEqual(joined.count("fetch --depth 1"), 2)

    def test_official_only_success(self):
        # prefix 为 None → 只走官方一路,一次成功
        root, src, sub = self._make_src()
        with mock.patch.object(fetch_mod.subprocess, "run",
                               side_effect=self._stateful_run(sub, "abc123", False)) as run:
            ok, err = fetch_mod._clone_glslang(src, sub, "abc123", None)
        self.assertTrue(ok, err)
        joined = " ".join(a for c in run.call_args_list for a in c.args[0])
        self.assertNotIn("gh-proxy", joined)
        self.assertEqual(joined.count("fetch --depth 1"), 1)

    def test_full_clone_fallback(self):
        # 浅拉全失败(源不支持按 SHA 拉取)→ 官方全量 clone + checkout commit
        root, src, sub = self._make_src()
        calls = []

        def _run(args, **kw):
            joined = " ".join(args)
            calls.append(joined)
            if "clone" in joined and " -C " not in joined:
                os.makedirs(sub, exist_ok=True)
                return mock.Mock(returncode=0, stdout="", stderr="")
            if "checkout" in joined:
                return mock.Mock(returncode=0, stdout="", stderr="")
            return mock.Mock(returncode=128, stdout="", stderr="shallow fail")
        with mock.patch.object(fetch_mod.subprocess, "run", side_effect=_run):
            ok, err = fetch_mod._clone_glslang(src, sub, "abc123", None)
        self.assertTrue(ok, err)
        self.assertIn("git clone", " ".join(calls))


class TestGlslangCommit(unittest.TestCase):
    def test_parses_ls_tree_gitlink(self):
        with mock.patch("deps_lib.fetch.subprocess.run") as run:
            run.return_value = mock.Mock(
                returncode=0,
                stdout="160000 commit 2b2523fb951f63f072cfba514c26f2feea5f4329\tthird_party/glslang\n",
                stderr="")
            self.assertEqual(fetch_mod._glslang_commit("/src"),
                             "2b2523fb951f63f072cfba514c26f2feea5f4329")

    def test_ls_tree_failure_returns_empty(self):
        with mock.patch("deps_lib.fetch.subprocess.run") as run:
            run.return_value = mock.Mock(returncode=128, stdout="", stderr="fatal")
            self.assertEqual(fetch_mod._glslang_commit("/src"), "")


class TestRmRf(unittest.TestCase):
    def test_removes_file(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        p = os.path.join(tmp.name, "x")
        with open(p, "w") as f:
            f.write("hi")
        self.assertTrue(fetch_mod._rm_rf(p))
        self.assertFalse(os.path.exists(p))

    def test_removes_dir_tree(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = os.path.join(tmp.name, "sub")
        os.makedirs(os.path.join(d, "inner"))
        with open(os.path.join(d, "inner", "f"), "w") as f:
            f.write("x")
        self.assertTrue(fetch_mod._rm_rf(d))
        self.assertFalse(os.path.exists(d))

    def test_missing_path_is_ok(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.assertTrue(fetch_mod._rm_rf(os.path.join(tmp.name, "nope")))


class TestMirrorRewrite(unittest.TestCase):
    def test_clear_mirror_rewrites_unsets_all(self):
        results = [
            mock.Mock(returncode=0, stdout=(
                "url.https://ghproxy.net/https://github.com/.insteadOf\n"
                "url.https://gh-proxy.com/https://github.com/.insteadOf\n")),
            mock.Mock(returncode=0, stdout="", stderr=""),
            mock.Mock(returncode=0, stdout="", stderr=""),
        ]
        with mock.patch("deps_lib.fetch.subprocess.run", side_effect=results) as run:
            fetch_mod._clear_mirror_rewrites("/src")
        self.assertEqual(run.call_count, 3)  # get-regexp 列名 + 两个 unset
        keys = [c.args[0][c.args[0].index("--unset-all") + 1]
                for c in run.call_args_list[1:]]
        self.assertEqual(keys, [
            "url.https://ghproxy.net/https://github.com/.insteadOf",
            "url.https://gh-proxy.com/https://github.com/.insteadOf",
        ])


class TestGlslangStandaloneRealGit(unittest.TestCase):
    """真实 git 集成:坏状态(gitfile→缺失 gitdir)下 ensure_swiftshader_submodules
    经独立克隆自动恢复,HEAD 对齐索引固定 commit。用本地 file:// 假 glslang origin,
    不打网络。"""

    def setUp(self):
        self._old_proto = os.environ.get("GIT_ALLOW_PROTOCOL")
        os.environ["GIT_ALLOW_PROTOCOL"] = "file:http:https"
        self.addCleanup(self._restore_proto)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

        # 1) 本地 glslang origin,固定 commit(HEAD)
        self.origin = os.path.join(self.tmp.name, "glslang-origin")
        _make_local_repo(self.origin, "pinned")
        subprocess.run(["git", "-C", self.origin, "config",
                        "uploadpack.allowReachableSHA1InWant", "true"], check=True)
        self.pinned = subprocess.run(["git", "-C", self.origin, "rev-parse", "HEAD"],
                                     capture_output=True, text=True, check=True).stdout.strip()

        # 2) swiftshader 超项目,gitlink 指向该 commit(路径必须符合 pool.src_dir 约定)
        self.sw = os.path.join(self.tmp.name, "third_party", "_src", "swiftshader-master")
        os.makedirs(self.sw)
        subprocess.run(["git", "init", "-q", self.sw], check=True)
        subprocess.run(["git", "-C", self.sw, "config", "user.email", "t@t"], check=True)
        subprocess.run(["git", "-C", self.sw, "config", "user.name", "t"], check=True)
        os.makedirs(os.path.join(self.sw, "third_party"))
        with open(os.path.join(self.sw, ".gitmodules"), "w") as f:
            f.write('[submodule "third_party/glslang"]\n\tpath = third_party/glslang\n'
                    "\turl = %s\n" % self.origin)
        subprocess.run(["git", "-C", self.sw, "add", "."], check=True)
        subprocess.run(["git", "-C", self.sw, "commit", "-q", "-m", "init"], check=True)
        subprocess.run(["git", "-C", self.sw, "update-index", "--add", "--cacheinfo",
                        "160000,%s,third_party/glslang" % self.pinned], check=True)
        subprocess.run(["git", "-C", self.sw, "commit", "-q", "-m", "gitlink"], check=True)

        # 3) 坏状态:gitfile 指向不存在的 gitdir(用户 Windows 上残留的形态)
        self.sub = os.path.join(self.sw, "third_party", "glslang")
        os.makedirs(self.sub)
        with open(os.path.join(self.sub, ".git"), "w") as f:
            f.write("gitdir: ../../.git/modules/third_party/glslang")

        self.pool_root = self.tmp.name  # ensure 用 pool.src_dir(root, "swiftshader", "master")

    def _restore_proto(self):
        if self._old_proto is None:
            os.environ.pop("GIT_ALLOW_PROTOCOL", None)
        else:
            os.environ["GIT_ALLOW_PROTOCOL"] = self._old_proto

    def _head(self):
        return subprocess.run(["git", "-C", self.sub, "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()

    def test_broken_gitfile_recovers_via_standalone_clone(self):
        # 进入前:坏 gitfile 在位
        self.assertTrue(os.path.isfile(os.path.join(self.sub, ".git")))
        with mock.patch.object(fetch_mod, "GLSLANG_URL", self.origin), \
             mock.patch.object(fetch_mod.mirror, "pick_mirror_prefix", return_value=None):
            ok, err = fetch_mod.ensure_swiftshader_submodules(self.pool_root)
        self.assertTrue(ok, err)
        # 恢复后:独立克隆(.git 是目录),HEAD 对齐索引固定 commit
        self.assertTrue(os.path.isdir(os.path.join(self.sub, ".git")))
        self.assertEqual(self._head(), self.pinned)
        # 父仓无残留子模块 gitdir
        self.assertFalse(os.path.exists(
            os.path.join(self.sw, ".git", "modules", "third_party", "glslang")))

    def test_mirror_fails_then_official_real_git(self):
        # 镜像(必败的本地 file:// 路径)失败 → 自动退官方(本地 origin)成功,不打网络
        with mock.patch.object(fetch_mod, "GLSLANG_URL", self.origin), \
             mock.patch.object(fetch_mod.mirror, "pick_mirror_prefix",
                               return_value="file:///definitely/missing/"):
            ok, err = fetch_mod.ensure_swiftshader_submodules(self.pool_root)
        self.assertTrue(ok, err)
        self.assertTrue(os.path.isdir(os.path.join(self.sub, ".git")))
        self.assertEqual(self._head(), self.pinned)

    def test_reproduce_git_submodule_bug_before_fix(self):
        # 对照锚点:坏状态下直接 git submodule update 必然本地失败(不碰网络)——
        # 这正是必须绕开 git submodule 机制的原因。修复后此测试依然成立。
        r = subprocess.run(
            ["git", "-C", self.sw, "submodule", "update", "--init", "--depth", "1",
             "third_party/glslang"],
            capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertTrue(os.path.isfile(os.path.join(self.sub, ".git")))  # 仍坏,未被修复


if __name__ == "__main__":
    unittest.main()
