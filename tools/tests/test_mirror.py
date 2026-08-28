import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from deps_lib import mirror


class TestMirrorUrl(unittest.TestCase):
    def test_mirror_url_prepends_prefix(self):
        self.assertEqual(
            mirror.mirror_url("https://github.com/glfw/glfw.git", "https://ghproxy.net/"),
            "https://ghproxy.net/https://github.com/glfw/glfw.git",
        )

    def test_mirror_url_none_prefix_returns_original(self):
        self.assertEqual(
            mirror.mirror_url("https://github.com/glfw/glfw.git", None),
            "https://github.com/glfw/glfw.git",
        )


class TestPickMirrorPrefix(unittest.TestCase):
    def test_returns_fastest_reachable(self):
        with mock.patch.object(mirror, "_probe", side_effect=[3.0, 0.2]):
            self.assertEqual(mirror.pick_mirror_prefix(), "https://gh-proxy.com/")

    def test_returns_none_when_all_unreachable(self):
        with mock.patch.object(mirror, "_probe", return_value=None):
            self.assertIsNone(mirror.pick_mirror_prefix())

    def test_returns_none_when_no_prefixes(self):
        with mock.patch.object(mirror, "MIRROR_PREFIXES", []):
            self.assertIsNone(mirror.pick_mirror_prefix())


class TestProbe(unittest.TestCase):
    def test_probe_returns_elapsed_on_success(self):
        # 用 MagicMock:with urlopen(...) 走上下文管理器协议,普通 Mock 不支持 __enter__
        fake = mock.MagicMock()
        fake.read.return_value = b""
        with mock.patch("deps_lib.mirror.urllib.request.urlopen", return_value=fake):
            t = mirror._probe("https://ghproxy.net/", 3.0)
        self.assertIsInstance(t, float)
        self.assertGreater(t, 0)

    def test_probe_returns_none_on_error(self):
        with mock.patch("deps_lib.mirror.urllib.request.urlopen",
                        side_effect=OSError("unreachable")):
            self.assertIsNone(mirror._probe("https://ghproxy.net/", 3.0))
