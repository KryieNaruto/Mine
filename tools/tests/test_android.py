import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from deps_lib import android


class TestFindAndroidSdk(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self._env = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)

    def test_env_var_wins(self):
        sdk = os.path.join(self.tmp.name, "sdk")
        os.makedirs(sdk)
        os.environ["ANDROID_HOME"] = sdk
        os.environ["ANDROID_SDK_ROOT"] = "/nonexistent"
        self.assertEqual(android.find_android_sdk(), sdk)

    def test_home_android_sdk_fallback(self):
        fake_home = os.path.join(self.tmp.name, "home")
        sdk = os.path.join(fake_home, "Android", "Sdk")
        os.makedirs(sdk)
        with mock.patch("os.path.expanduser", return_value=fake_home):
            os.environ.pop("ANDROID_HOME", None)
            os.environ.pop("ANDROID_SDK_ROOT", None)
            self.assertEqual(android.find_android_sdk(), sdk)

    def test_user_deps_android_sdk_fallback(self):
        deps = os.path.join(self.tmp.name, "user-deps")
        sdk = os.path.join(deps, "android-sdk")
        os.makedirs(sdk)
        # 把 HOME 指向不存在的目录,避免被真实 ~/Android/Sdk 抢先命中,确保走到 user-deps 兜底
        with mock.patch("os.path.expanduser", return_value=os.path.join(self.tmp.name, "no-home")), \
             mock.patch("os.environ.get", wraps=os.environ.get):
            os.environ.pop("ANDROID_HOME", None)
            os.environ.pop("ANDROID_SDK_ROOT", None)
            os.environ["USER_DEPS"] = deps
            self.assertEqual(android.find_android_sdk(), sdk)

    def test_mine_root_user_deps_fallback_when_user_deps_unset(self):
        # USER_DEPS 未设时,默认落到 MINE_ROOT/.user-deps/android-sdk(android-deps.sh 落地目录)。
        # mock MINE_ROOT + expanduser 指向临时目录,隔离真实机器的 ~/Android/Sdk 与仓库 .user-deps。
        sdk = os.path.join(self.tmp.name, "mine", ".user-deps", "android-sdk")
        os.makedirs(sdk)
        with mock.patch.object(android, "MINE_ROOT", os.path.join(self.tmp.name, "mine")), \
             mock.patch("os.path.expanduser", return_value=os.path.join(self.tmp.name, "no-home")):
            os.environ.pop("ANDROID_HOME", None)
            os.environ.pop("ANDROID_SDK_ROOT", None)
            os.environ.pop("USER_DEPS", None)
            os.environ.pop("LOCALAPPDATA", None)
            self.assertEqual(android.find_android_sdk(), sdk)

    def test_none_when_missing(self):
        os.environ.pop("ANDROID_HOME", None)
        os.environ.pop("ANDROID_SDK_ROOT", None)
        os.environ.pop("USER_DEPS", None)
        os.environ.pop("LOCALAPPDATA", None)
        with mock.patch("os.path.expanduser", return_value="/nonexistent-home"), \
             mock.patch.object(android, "MINE_ROOT", "/nonexistent-mine-root"):
            self.assertIsNone(android.find_android_sdk())


class TestEscapeProperties(unittest.TestCase):
    def test_backslash_escaped(self):
        self.assertEqual(android._escape_properties(r"D:\qsw\Android\Sdk"),
                         r"D:\\qsw\\Android\\Sdk")


class TestWriteLocalProperties(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_writes_sdk_dir(self):
        project_dir = os.path.join(self.tmp.name, "proj")
        os.makedirs(project_dir, exist_ok=True)
        p = android.write_local_properties(project_dir, r"D:\qsw\Android\Sdk")
        with open(p, encoding="utf-8") as f:
            self.assertIn(r"sdk.dir=D:\\qsw\\Android\\Sdk", f.read())
