import os
import sys
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


if __name__ == "__main__":
    unittest.main()
