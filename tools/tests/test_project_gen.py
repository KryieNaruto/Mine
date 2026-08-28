import os
import sys
import tempfile
import unittest
from unittest import mock

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)  # tools/
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


class TestDiscoverVsGenerator(unittest.TestCase):
    _HELP_OUTPUT = """
Generators

The following generators are available on this platform (* marks default):
* Ninja                        = Generates build.ninja files.
  Visual Studio 16 2019        = Generates Visual Studio 2019 project files.
                                  Use -A option to specify architecture.
  Visual Studio 17 2022        = Generates Visual Studio 2022 project files.
                                  Use -A option to specify architecture.
"""

    def test_picks_newest_year(self):
        with mock.patch.object(
            project_gen.subprocess, "run",
            return_value=mock.Mock(stdout=self._HELP_OUTPUT),
        ):
            self.assertEqual(project_gen.discover_vs_generator(), "Visual Studio 17 2022")

    def test_no_vs_generator_returns_empty(self):
        with mock.patch.object(
            project_gen.subprocess, "run",
            return_value=mock.Mock(stdout="* Ninja = Generates build.ninja files.\n"),
        ):
            self.assertEqual(project_gen.discover_vs_generator(), "")

    def test_cmake_missing_returns_empty(self):
        with mock.patch.object(project_gen.subprocess, "run", side_effect=OSError("no cmake")):
            self.assertEqual(project_gen.discover_vs_generator(), "")


class TestGenVs(unittest.TestCase):
    def setUp(self):
        self._old = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._old)

    def _make_project(self, root, name="EasyPainter", with_cmake=True):
        d = os.path.join(root, name)
        os.makedirs(d, exist_ok=True)
        if with_cmake:
            _write(os.path.join(d, "CMakeLists.txt"), "project(demo)\n")
        return d

    def test_skips_when_no_cmakelists(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True):
            self._make_project(root, with_cmake=False)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertTrue(ok)
            self.assertTrue(msg.startswith("跳过"))

    def test_fails_when_no_vs_generator_found(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value=""):
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertFalse(ok)
            self.assertIn("Visual Studio", msg)

    def test_fails_when_msvc_env_injection_fails(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=False):
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertFalse(ok)

    def test_command_shape_and_success(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes",
                                return_value=[os.path.join(root, "third_party/_install/abseil-cpp-1/release")]), \
             mock.patch.object(project_gen.cmake_driver, "_stream", return_value=(True, "")) as stream:
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertTrue(ok)
            self.assertTrue(msg.endswith("EasyPainter.sln"))
            cmd = stream.call_args[0][0]
            self.assertEqual(cmd[0], "cmake")
            self.assertIn("-G", cmd)
            self.assertIn("Visual Studio 17 2022", cmd)
            self.assertIn("-A", cmd)
            self.assertIn("x64", cmd)
            self.assertIn("-DCMAKE_CONFIGURATION_TYPES=Release", cmd)
            joined = " ".join(cmd)
            self.assertIn("-DCMAKE_PREFIX_PATH=", joined)
            self.assertIn("abseil-cpp-1", joined)
            self.assertEqual(os.environ.get("MINE_ROOT"), root)

    def test_clears_stale_cmake_cache_before_reconfigure(self):
        # 回归:_gen_vs 每次都 -B 进同一个 build/vs 目录复用。find_package 的结果
        # (如 glfw3_DIR)一旦被 CMakeCache.txt 缓存住,哪怕后来 CMakeLists.txt 改了
        # 搜索范围(收窄只认池前缀),重新 configure 也不会重新搜——CMake 直接复用
        # 缓存里已经定住的(哪怕是误链到 MinGW glfw3 的)路径,新 CMakeLists 的收窄
        # 形同虚设(本机复现:上一版修复后 libglfw3.a 的 LNK2019 原样重现)。必须在
        # 每次 configure 前清掉配置期缓存(CMakeCache.txt + CMakeFiles/)强制全新搜索。
        # 已编译产物(Debug/Release 下的 obj/lib/exe)不在这两处,不应被这次清理动到——
        # _gen_vs 的职责是"只 configure 不编译",清缓存不该连带强制整包重编。
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes", return_value=[]), \
             mock.patch.object(project_gen.cmake_driver, "_stream", return_value=(True, "")):
            project_dir = self._make_project(root)
            build_dir = os.path.join(project_dir, "build", "vs")
            os.makedirs(os.path.join(build_dir, "CMakeFiles"), exist_ok=True)
            _write(os.path.join(build_dir, "CMakeCache.txt"),
                   "glfw3_DIR:PATH=/mingw64/lib/cmake/glfw3\n")
            _write(os.path.join(build_dir, "CMakeFiles", "stale.stamp"), "stale")
            # 已编译产物:清缓存不该动它
            _write(os.path.join(build_dir, "Release", "easypainter.exe"), "binary")

            ok, _ = project_gen._gen_vs(root, "EasyPainter", "release", None)

            self.assertTrue(ok)
            self.assertFalse(os.path.exists(os.path.join(build_dir, "CMakeCache.txt")),
                              "陈旧 CMakeCache.txt 必须在 configure 前清掉")
            self.assertFalse(os.path.exists(os.path.join(build_dir, "CMakeFiles")),
                              "陈旧 CMakeFiles/ 必须在 configure 前清掉")
            self.assertTrue(os.path.isfile(os.path.join(build_dir, "Release", "easypainter.exe")),
                             "已编译产物不应被清缓存动到")

    def test_debug_variant_uses_debug_configuration_and_separate_build_dir(self):
        # 回归:gen-projects.py 的 --variant 早就存在(CLI 默认 release),但 _gen_vs
        # 之前不论 variant 恒 -DCMAKE_CONFIGURATION_TYPES=Release 且写死 build/vs ——
        # 传 --variant debug 会静默生成一个自称 Debug 却仍指向 release 池前缀语义
        # 混乱的目录,还会覆盖 release 的 build/vs。debug 变体必须落在独立目录,且
        # 请求 Debug 配置类型,使两者可共存、各自可在 VS 里直接打开。
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes",
                                return_value=[os.path.join(root, "third_party/_install/abseil-cpp-1/debug")]), \
             mock.patch.object(project_gen.cmake_driver, "_stream", return_value=(True, "")) as stream:
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "debug", None)
            self.assertTrue(ok)
            cmd = stream.call_args[0][0]
            self.assertIn("-DCMAKE_CONFIGURATION_TYPES=Debug", cmd)
            self.assertNotIn("-DCMAKE_CONFIGURATION_TYPES=Release", cmd)
            b_idx = cmd.index("-B")
            build_dir = cmd[b_idx + 1]
            self.assertNotEqual(
                build_dir, os.path.join(root, "EasyPainter", "build", "vs"),
                "debug 变体不能复用 release 的 build/vs 目录(会互相覆盖)",
            )
            self.assertTrue(msg.startswith(build_dir))

    def test_release_variant_still_uses_build_vs_dir(self):
        # release 是既有默认行为,目录名不应因本次改动而变化(向后兼容)。
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes", return_value=[]), \
             mock.patch.object(project_gen.cmake_driver, "_stream", return_value=(True, "")) as stream:
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertTrue(ok)
            cmd = stream.call_args[0][0]
            b_idx = cmd.index("-B")
            self.assertEqual(cmd[b_idx + 1], os.path.join(root, "EasyPainter", "build", "vs"))

    def test_generator_override_skips_discovery(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator") as discover, \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes", return_value=[]), \
             mock.patch.object(project_gen.cmake_driver, "_stream", return_value=(True, "")) as stream:
            self._make_project(root)
            ok, _ = project_gen._gen_vs(root, "EasyPainter", "release", "Visual Studio 16 2019")
            self.assertTrue(ok)
            discover.assert_not_called()
            self.assertIn("Visual Studio 16 2019", stream.call_args[0][0])

    def test_configure_failure_reports_tail_log(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen, "discover_vs_generator", return_value="Visual Studio 17 2022"), \
             mock.patch.object(project_gen.msvc_env, "ensure_msvc_env", return_value=True), \
             mock.patch.object(project_gen.cmake_driver, "_built_prefixes", return_value=[]), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(False, "CMake Error: find_package(absl) 失败")):
            self._make_project(root)
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
            self.assertFalse(ok)
            self.assertIn("find_package(absl)", msg)


class TestGenVsLinuxBuild(unittest.TestCase):
    def setUp(self):
        self._old = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._old)

    def test_linux_uses_preset_and_builds(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(True, "")) as stream:
            d = os.path.join(root, "EasyPainter")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "CMakeLists.txt"), "w") as f:
                f.write("project(demo)\n")
            with open(os.path.join(d, "CMakePresets.json"), "w") as f:
                f.write("{}")
            ok, msg = project_gen._gen_vs(root, "EasyPainter", "release", None)
        self.assertTrue(ok)
        cmds = [c.args[0] for c in stream.call_args_list]
        self.assertIn(["cmake", "--preset", "release", "-S", d], cmds)
        self.assertTrue(any("--build" in c for c in cmds))
        self.assertTrue(msg.startswith("构建完成"))
        self.assertEqual(os.environ.get("MINE_ROOT"), root)

    def test_linux_debug_variant_uses_explicit_build_type(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(True, "")) as stream:
            d = os.path.join(root, "EasyPainter")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "CMakeLists.txt"), "w") as f:
                f.write("project(demo)\n")
            ok, _ = project_gen._gen_vs(root, "EasyPainter", "debug", None)
        self.assertTrue(ok)
        cfg = stream.call_args_list[0].args[0]
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", cfg)
        b_idx = cfg.index("-B")
        self.assertEqual(cfg[b_idx + 1], os.path.join(d, "build", "debug"),
                         "debug 变体必须落在 build/debug,不能复用 release 的 build/release")


class TestGenAs(unittest.TestCase):
    def setUp(self):
        self._old = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._old)

    def _make_android_project(self, root, with_gradlew=True):
        d = os.path.join(root, "HelloAndroid")
        os.makedirs(os.path.join(d, "app"), exist_ok=True)
        if with_gradlew:
            with open(os.path.join(d, "gradlew"), "w") as f:
                f.write("#!/usr/bin/env bash\n")
        return d

    def test_windows_with_sdk_writes_local_properties(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen.android, "find_android_sdk",
                               return_value=r"D:\Android\Sdk"), \
             mock.patch.object(project_gen.android, "write_local_properties",
                               return_value="") as wlp:
            self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        wlp.assert_called_once()
        self.assertIn("local.properties", msg)

    def test_windows_no_sdk_skips(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=True), \
             mock.patch.object(project_gen.android, "find_android_sdk", return_value=None):
            self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        self.assertTrue(msg.startswith("跳过"))

    def test_linux_with_sdk_builds_apk(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.android, "find_android_sdk",
                               return_value="/opt/android-sdk"), \
             mock.patch.object(project_gen.cmake_driver, "_stream",
                                return_value=(True, "")) as stream:
            d = self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        cmd, kw = stream.call_args
        self.assertEqual(cmd[0], ["./gradlew", "assembleDebug"])
        self.assertEqual(kw.get("cwd"), d)
        self.assertIn("app-debug.apk", msg)

    def test_linux_no_sdk_skips(self):
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(project_gen.pool, "on_windows", return_value=False), \
             mock.patch.object(project_gen.android, "find_android_sdk", return_value=None):
            self._make_android_project(root)
            ok, msg = project_gen._gen_as(root, "HelloAndroid", "release", None)
        self.assertTrue(ok)
        self.assertTrue(msg.startswith("跳过"))


class TestGenerateDispatch(unittest.TestCase):
    def test_unknown_type_fails(self):
        ok, msg = project_gen.generate("/root", "X", "bogus", "release", None)
        self.assertFalse(ok)
        self.assertIn("bogus", msg)

    def test_dispatches_to_registered_generator(self):
        with mock.patch.dict(project_gen.GENERATORS, {"vs": mock.Mock(return_value=(True, "ok"))}):
            ok, msg = project_gen.generate("/root", "EasyPainter", "vs", "release", None)
            self.assertTrue(ok)
            self.assertEqual(msg, "ok")
            project_gen.GENERATORS["vs"].assert_called_once_with("/root", "EasyPainter", "release", None)


class TestRealProjectsDeclareVsType(unittest.TestCase):
    def test_easypainter_and_stickynotes_declare_vs(self):
        mine_root = os.path.dirname(_TOOLS)
        for name in ("EasyPainter", "StickyNotes"):
            deps_path = os.path.join(mine_root, name, "deps.yaml")
            self.assertTrue(os.path.isfile(deps_path), f"{deps_path} 不存在")
            with open(deps_path, encoding="utf-8") as f:
                content = f.read()
            self.assertIn("type: vs", content, f"{deps_path} 缺少显式 type: vs 声明")


if __name__ == "__main__":
    unittest.main()
