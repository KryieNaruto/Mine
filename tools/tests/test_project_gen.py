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
