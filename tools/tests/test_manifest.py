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


class TestWindowsPackages(unittest.TestCase):
    def test_extract_returns_windows_package_names(self):
        gm = {
            "libs": {
                "abseil-cpp": {"repo": "x", "tag": "1", "windows_package": "mingw-w64-x86_64-abseil-cpp"},
                "fmt": {"repo": "x", "tag": "1"},
                "glfw": {"repo": "x", "tag": "1", "windows_package": "mingw-w64-x86_64-glfw"},
            }
        }
        self.assertEqual(
            manifest.extract_windows_packages(gm),
            ["mingw-w64-x86_64-abseil-cpp", "mingw-w64-x86_64-glfw"],
        )

    def test_extract_empty_when_none(self):
        self.assertEqual(manifest.extract_windows_packages({"libs": {}}), [])

    def test_extract_tolerates_missing_libs_key(self):
        self.assertEqual(manifest.extract_windows_packages({}), [])


if __name__ == "__main__":
    unittest.main()
