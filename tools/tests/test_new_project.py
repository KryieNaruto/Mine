import importlib.util
import os
import sys
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
