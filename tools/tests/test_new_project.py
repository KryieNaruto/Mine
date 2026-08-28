import importlib.util
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # tools/
sys.path.insert(0, _TOOLS)
# 模块文件名为 new-project.py(带连字符),无法用 `import new_project` 直接导入,
# 此处用 importlib 按路径加载。
_spec = importlib.util.spec_from_file_location(
    "new_project", os.path.join(_TOOLS, "new-project.py")
)
np_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(np_mod)


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


class TestNewProjectAndroid(unittest.TestCase):
    """new-project.py as 类型:生成可被 _gen_as 消费的 Android 骨架。"""

    def setUp(self):
        # 不在真实 MINE_ROOT 上写项目:把 MINE_ROOT 指向临时目录(与 TestTypeFlag 同款)。
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        os.makedirs(os.path.join(self.root, "third_party"), exist_ok=True)
        with open(os.path.join(self.root, "third_party", "deps.yaml"), "w", encoding="utf-8") as f:
            f.write("libs: {}\n")
        shutil.copytree(
            os.path.join(_TOOLS, "templates"),
            os.path.join(self.root, "tools", "templates"),
        )

    def test_as_lang_generates_android_skeleton(self):
        with mock.patch.object(np_mod, "LANGS", {"cpp", "python", "web", "as"}), \
             mock.patch.object(np_mod, "MINE_ROOT", self.root):
            rc = np_mod.main(["as", "HelloAndroid"])
        self.assertEqual(rc, 0)
        dst = os.path.join(self.root, "HelloAndroid")
        for rel in ("settings.gradle", "build.gradle", "gradle.properties",
                    "app/build.gradle", "app/src/main/AndroidManifest.xml",
                    "gradle/wrapper/gradle-wrapper.properties",
                    "gradlew"):
            self.assertTrue(os.path.isfile(os.path.join(dst, rel)), rel)
        with open(os.path.join(dst, "deps.yaml"), encoding="utf-8") as f:
            self.assertIn("type: as", f.read())


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


if __name__ == "__main__":
    unittest.main()
