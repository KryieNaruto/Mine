#!/usr/bin/env python3
"""check_layer_purity.py(G3)的自测 —— 「先证明门控能变红」。

为什么需要这份自测:2026-11-11 t8 查出 G3 有一个暗漏洞 —— 它只匹配
`Vk` 前缀类型名、`vk`/`vma` 函数名和 Vulkan 头 include,**不认 `VK_*` 宏**
也**不认 `backend::` 命名空间**。于是
`src/android/scene_renderer.cpp` 里这种最直接的 backend 下钻

    auto* vkdev = dynamic_cast<backend::VulkanRenderDevice*>(device_.get());
    if (pt & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) rotation_deg_ = 90;

在门控眼里全是绿的。一条永远匹配不上任何东西的正则,与「没有这条规则」等价,
而它在门控输出里**看不出来**(报告永远写 "OK")。本文件的作用就是钉住:
每个模式都必须能在合成样本上变红。

运行(任选其一,不依赖 pytest):
  python -m unittest discover -s MagicBlocks/tools/tests -p "test_check_layer_purity.py" -v
  python MagicBlocks/tools/tests/test_check_layer_purity.py
"""

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.abspath(os.path.join(HERE, "..", "..", "..", "tools", "check_layer_purity.py"))
sys.path.insert(0, os.path.dirname(GATE))

import check_layer_purity as gate  # noqa: E402


def hits(code, rel="src/x.cpp", in_backend=False, in_features=False):
    """一行代码 → 违规列表(行号固定 1)。"""
    return gate.violations_in_code(rel, 1, code, in_backend, in_features)


class EveryPatternMustGoRed(unittest.TestCase):
    """每个模式都必须有自己的变红样本。少一条 = 门控回到暗漏洞。"""

    def test_vk_type(self):
        self.assertTrue(hits("VkDevice dev = VK_NULL_HANDLE;"))

    def test_vk_call(self):
        self.assertTrue(hits("vkQueuePresentKHR(queue, &info);"))

    def test_vk_include(self):
        self.assertTrue(hits('#include <vulkan/vulkan.h>'))

    def test_vk_macro(self):
        """t8 新增模式 1:VK_* 宏/枚举。"""
        self.assertTrue(hits("if (pt & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) rot = 90;"))

    def test_vk_macro_standalone(self):
        """单独出现的 VK_ 常量(既不是类型也不是调用)也必须红。"""
        self.assertTrue(hits("if (count == VK_WHOLE_SIZE) return;"))

    def test_backend_namespace(self):
        """t8 新增模式 2:backend:: 命名空间限定名。"""
        self.assertTrue(hits("auto* d = dynamic_cast<backend::VulkanRenderDevice*>(device_.get());"))

    def test_backend_namespace_qualified(self):
        """带 magicblocks:: 前缀的写法同样要红(否则换个前缀就能绕)。"""
        self.assertTrue(hits("auto* off = dynamic_cast<magicblocks::backend::OffscreenTarget*>(t);"))

    def test_backend_namespace_global_qualified(self):
        """::magicblocks::backend:: 这种写法同样要红。"""
        self.assertTrue(hits("auto* off = dynamic_cast<::magicblocks::backend::OffscreenTarget*>(t);"))

    def test_backend_namespace_value_type(self):
        """值类型也一样:include 一个 backend 头还能辩称「只是值类型」,
        直接写 backend:: 则连辩都没得辩。"""
        self.assertTrue(hits("backend::AttachmentShape shape{};"))

    def test_backend_include(self):
        """t3 新增模式 3:include 后端内部头。
        这是比 backend:: 更本质的一条 —— 名字可以拼,include 拼不了;
        旧版对 `#include "backend/vulkan/xxx.h"` 完全沉默。"""
        self.assertTrue(hits('#include "backend/vulkan/vulkan_render_device.h"'))
        self.assertTrue(hits('#include "backend/vulkan/vulkan_swapchain.h"'))


class MustStayGreen(unittest.TestCase):
    """误报会让规则被迫整体忽略 —— 那比没有规则更糟(假装有门控)。"""

    def test_backend_dir_exempt_for_vk(self):
        self.assertFalse(hits("VkDevice dev = VK_NULL_HANDLE;", rel="src/backend/vulkan/x.cpp", in_backend=True))
        self.assertFalse(hits("backend::VulkanRenderDevice* d;", rel="src/backend/vulkan/x.cpp", in_backend=True))

    def test_comment_stripped(self):
        lines = gate.strip_comments_and_strings([
            "// auto* d = dynamic_cast<backend::VulkanRenderDevice*>(p);",
            "/* if (x & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) */",
            "int ok = 1;",
        ])
        for i, code in enumerate(lines, 1):
            self.assertEqual(hits(code), [], "注释行 %d 不应计入: %r" % (i, code))

    def test_string_literal_stripped(self):
        lines = gate.strip_comments_and_strings(['const char* m = "VK_NULL_HANDLE backend::Foo";'])
        self.assertEqual(hits(lines[0]), [])

    def test_magicblocks_namespace_is_not_a_member(self):
        """`magicblocks::device::ISwapchain` 是 L0 公共面,不是 backend。
        反向断言:如果规则写成裸 `backend` 子串,这里会红。"""
        self.assertEqual(hits("device::ISwapchain* sc = nullptr;"), [])
        self.assertEqual(hits("std::unique_ptr<magicblocks::device::IRenderDevice> dev;"), [])

    def test_ordinary_identifiers(self):
        self.assertEqual(hits("std::uint32_t backend_count = 0;"), [])
        self.assertEqual(hits("int VK = 0;"), [])

    def test_vk_in_middle_of_identifier(self):
        """`myVkThing` / `vulkan_alloc` 这类名字不能被误伤。"""
        self.assertEqual(hits("int myVkThing = 0;"), [])
        self.assertEqual(hits("int vulkan_alloc = 0;"), [])

    def test_gameplay_name_in_backend(self):
        self.assertTrue(hits("void draw_voxel();", rel="src/backend/vulkan/x.cpp", in_backend=True))
        self.assertEqual(hits("namespace magicblocks {}", rel="src/backend/vulkan/x.cpp", in_backend=True), [])

    def test_features_gpu_command(self):
        self.assertTrue(hits("cmd->vkCmdDraw(3, 1, 0, 0);", rel="src/features/stack/x.cpp", in_features=True))


class LegalBackendEdgeIsNarrow(unittest.TestCase):
    """t3:翻译层白名单(src/core/device_ops.cpp)必须**只**免掉 backend:: 一条,
    而且只对那一个文件生效。任何一条断言红了,都说明门控被悄悄放宽了。"""

    def test_edge_file_may_name_backend_namespace(self):
        self.assertEqual(hits("return backend::create_vulkan_device(desc);",
                              rel="src/core/device_ops.cpp"), [])

    def test_edge_file_still_red_for_vk_macro(self):
        self.assertTrue(hits("if (x & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {}",
                             rel="src/core/device_ops.cpp"))

    def test_edge_file_still_red_for_vk_type(self):
        self.assertTrue(hits("VkDevice dev = nullptr;", rel="src/core/device_ops.cpp"))

    def test_edge_file_still_red_for_vk_include(self):
        self.assertTrue(hits('#include <vulkan/vulkan.h>', rel="src/core/device_ops.cpp"))

    def test_edge_file_still_red_for_vk_call(self):
        self.assertTrue(hits("vkDeviceWaitIdle(dev);", rel="src/core/device_ops.cpp"))

    def test_any_other_file_is_red(self):
        code = "auto* d = dynamic_cast<backend::VulkanRenderDevice*>(p);"
        for rel in ("src/core/device_ops.h", "src/core/other.cpp", "src/android/scene_renderer.cpp",
                    "src/ui/offscreen_target.h", "src/core/device_ops.cpp.bak"):
            self.assertTrue(hits(code, rel=rel), "%s 不该被白名单放行" % rel)

    def test_allowlist_is_a_closed_set_of_one(self):
        self.assertEqual(sorted(gate.LEGAL_BACKEND_EDGE), ["src/core/device_ops.cpp"])


class TestsPrefixWaiverIsNarrow(unittest.TestCase):
    """t3 收尾:白盒测试放行必须只覆盖 `backend::` 与 `VK_*` 两条模式,
    且只覆盖 tests/ 前缀。任何一条红了都说明放行被悄悄扩大。"""

    def test_tests_backend_ns_is_green(self):
        self.assertEqual(hits("auto* vp = dynamic_cast<magicblocks::backend::VulkanPipeline*>(p);",
                              rel="tests/magicblocks_l1_test.cpp"), [])
        self.assertEqual(hits("auto* d = dynamic_cast<backend::VulkanRenderDevice*>(dev);",
                              rel="tests/magicblocks_msaa_test.cpp"), [])

    def test_tests_vk_macro_is_green(self):
        self.assertEqual(hits("EXPECT_EQ(vp->polygon_mode(), VK_POLYGON_MODE_FILL);",
                              rel="tests/magicblocks_l1_test.cpp"), [])
        self.assertEqual(hits("EXPECT_EQ(vp->vk_set_layout(), VK_NULL_HANDLE);",
                              rel="tests/magicblocks_l1_test.cpp"), [])

    def test_tests_still_red_for_vulkan_include(self):
        """放行的是两条模式,不是整个目录 —— include 后端头在 tests/ 里仍然判红。"""
        self.assertTrue(hits('#include <vulkan/vulkan.h>', rel="tests/magicblocks_l1_test.cpp"))
        self.assertTrue(hits('#include "backend/vulkan/vulkan_pipeline.h"',
                             rel="tests/magicblocks_l1_test.cpp"))
        self.assertTrue(hits('#include "backend/vulkan/vulkan_pipeline.h"',
                             rel="src/ui/offscreen_target.h"))

    def test_tests_still_red_for_vk_type(self):
        self.assertTrue(hits("VkDevice dev = nullptr;", rel="tests/magicblocks_l1_test.cpp"))

    def test_tests_still_red_for_vk_call(self):
        self.assertTrue(hits("vkDeviceWaitIdle(dev);", rel="tests/magicblocks_l1_test.cpp"))
        self.assertTrue(hits("vmaCreateBuffer(a, &i, &x, &b, &y, &z);",
                             rel="tests/magicblocks_l1_test.cpp"))

    def test_tests_still_red_for_gameplay_name_in_backend(self):
        """tests/ 前缀只影响 backend 之外的判定;backend 目录内的玩法名规则不受影响。"""
        self.assertTrue(hits("void draw_voxel();", rel="tests/x.cpp", in_backend=True))

    def test_prefix_does_not_leak_outside_tests(self):
        code = "auto* d = dynamic_cast<backend::VulkanRenderDevice*>(p);"
        for rel in ("src/ui/offscreen_target.h", "src/ui/scene_sink_host.cpp",
                    "src/ui_cli/main.cpp", "src/sink_verify/main.cpp",
                    "src/cli/main.cpp", "src/l2_cli/main.cpp",
                    "testsuite/x.cpp", "src/tests/x.cpp"):
            self.assertTrue(hits(code, rel=rel), "%s 不在 tests/ 前缀下,不该被放行" % rel)
        self.assertTrue(hits("if (x & VK_NULL_HANDLE) {}", rel="src/ui/offscreen_target.h"))

    def test_waiver_is_a_pattern_list_not_a_directory_skip(self):
        """放行清单的形状本身就是契约:两条模式,不多不少。"""
        self.assertEqual(gate.TEST_PREFIX, "tests/")
        self.assertEqual(sorted(gate.TEST_ALLOWED_PATTERNS), ["backend_ns", "vk_macro"])


class EndToEndOnSyntheticTree(unittest.TestCase):
    """真的写一棵小目录树跑一遍 CLI:退出码 1 且输出里带文件:行。"""

    def test_red_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src", "android")
            os.makedirs(src)
            with open(os.path.join(src, "leak.cpp"), "w", encoding="utf-8") as f:
                f.write("int f() {\n"
                        "  auto* d = dynamic_cast<backend::VulkanRenderDevice*>(nullptr);\n"
                        "  return d && (1 & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR);\n"
                        "}\n")
            r = subprocess.run([sys.executable, GATE, "--root", tmp],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("src/android/leak.cpp:2", r.stdout)
            self.assertIn("src/android/leak.cpp:3", r.stdout)

    def test_green_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src", "android")
            os.makedirs(src)
            with open(os.path.join(src, "clean.cpp"), "w", encoding="utf-8") as f:
                f.write("#include \"magicblocks/mb_capi.h\"\n"
                        "// VK_NULL_HANDLE 与 backend::Foo 只在注释里出现\n"
                        "int f() { return 0; }\n")
            r = subprocess.run([sys.executable, GATE, "--root", tmp],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
