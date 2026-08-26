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


if __name__ == "__main__":
    unittest.main()
