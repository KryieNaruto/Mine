import os
import sys
import tempfile
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


class TestPrefixPathInjection(unittest.TestCase):
    def _make_pool(self, built_release, built_debug):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = tmp.name
        for name in built_release:
            d = os.path.join(root, "third_party", "_install", name, "release")
            os.makedirs(d)
            with open(os.path.join(d, ".built"), "w") as f:
                f.write("")
        for name in built_debug:
            d = os.path.join(root, "third_party", "_install", name, "debug")
            os.makedirs(d)
            with open(os.path.join(d, ".built"), "w") as f:
                f.write("")
        return root

    def test_injects_only_built_release_prefixes(self):
        root = self._make_pool(
            built_release=["abseil-cpp-20260817.0", "glm-1.0.1"],
            built_debug=["abseil-cpp-20260817.0"],
        )
        lib = LibSpec(name="ink-stroke-modeler", repo="r", tag="main")
        cmd = cmake_driver.configure_command(root, lib, "release")
        joined = " ".join(cmd)
        self.assertIn("-DCMAKE_PREFIX_PATH=", joined)
        # 含已 build 的 release 前缀
        self.assertIn(
            os.path.join(root, "third_party", "_install", "abseil-cpp-20260817.0", "release"),
            joined,
        )
        self.assertIn(
            os.path.join(root, "third_party", "_install", "glm-1.0.1", "release"),
            joined,
        )
        # 不含未 build 的 debug 前缀(debug 前缀绝不进 release 的 PREFIX_PATH)
        self.assertNotIn(os.path.join(root, "third_party", "_install", "glm-1.0.1", "debug"), joined)

    def test_no_prefix_flag_when_nothing_built(self):
        root = self._make_pool(built_release=[], built_debug=[])
        lib = LibSpec(name="fmt", repo="r", tag="10.2.1")
        cmd = cmake_driver.configure_command(root, lib, "release")
        self.assertNotIn("-DCMAKE_PREFIX_PATH=", " ".join(cmd))


if __name__ == "__main__":
    unittest.main()
