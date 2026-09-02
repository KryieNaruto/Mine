import json
import os
import sys
import tempfile
import unittest
from unittest import mock

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

    def test_is_pacman_provided_false_on_windows_msvc(self):
        # MSVC 工具链下 pacman 的 MinGW 库不兼容 → 恒 False。
        # 构造"Windows + deps.yaml 声明 pacman 包 + pacman -Q 成功"的旧逻辑
        # 返回 True 场景,验证新逻辑仍恒 False。
        os.makedirs(os.path.join(self.root, "third_party"))
        with open(os.path.join(self.root, "third_party", "deps.yaml"), "w", encoding="utf-8") as f:
            f.write("libs:\n  abseil-cpp:\n    windows_package: mingw-w64-x86_64-abseil\n")
        with mock.patch.object(pool, "on_windows", return_value=True), \
             mock.patch.object(
                 pool.subprocess, "run",
                 return_value=mock.Mock(returncode=0),
             ):
            self.assertFalse(pool.is_pacman_provided(self.root, "abseil-cpp"))

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

    def test_options_sig_sorted_and_empty(self):
        self.assertEqual(pool.options_sig(["FMT_TEST=OFF"]), "FMT_TEST=OFF")
        self.assertEqual(pool.options_sig(["b=1", "a=2"]), "a=2|b=1")
        self.assertEqual(pool.options_sig(None), "")
        self.assertEqual(pool.options_sig([]), "")

    def test_built_options_change_invalidates(self):
        # 模拟有 git 元数据(否则指纹为空直接放行,测不到 opts 分支)
        with mock.patch.object(pool, "_src_fingerprint", return_value="abc123|0"):
            inst = pool.install_dir(self.root, "fmt", "10.2.1", "release")
            os.makedirs(inst)
            with open(os.path.join(inst, ".built"), "w", encoding="utf-8") as f:
                f.write("variant=release\nsrc=abc123|0\nopts=FMT_TEST=OFF\n")
            # 选项没变 → 已建
            self.assertTrue(
                pool.is_built(self.root, "fmt", "10.2.1", "release", ["FMT_TEST=OFF"]))
            # 选项变了 → 需重编
            self.assertFalse(
                pool.is_built(self.root, "fmt", "10.2.1", "release", ["FMT_TEST=ON"]))
            # 不传 options → 保持旧的只看源码行为
            self.assertTrue(pool.is_built(self.root, "fmt", "10.2.1", "release"))

    def test_built_legacy_without_opts_is_allowed(self):
        # 旧 .built(改版前)没有 opts= 行:无法回溯,按已建放行,避免误伤全池重编
        with mock.patch.object(pool, "_src_fingerprint", return_value="abc123|0"):
            inst = pool.install_dir(self.root, "fmt", "10.2.1", "release")
            os.makedirs(inst)
            with open(os.path.join(inst, ".built"), "w", encoding="utf-8") as f:
                f.write("variant=release\nsrc=abc123|0\n")
            self.assertTrue(
                pool.is_built(self.root, "fmt", "10.2.1", "release", ["FMT_TEST=OFF"]))


if __name__ == "__main__":
    unittest.main()
