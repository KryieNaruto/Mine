"""门控脚本自测:tools/check_*.py 必须在「违规样本」上红、在「合规样本」上绿。

第 1 版审阅指出:门控脚本存在但断言不对会一直误绿。这里用临时目录各造违规/合规样本,
直接对脚本的 `main(["--root", tmp])` 断言退出码与输出关键字。

运行: python -m unittest discover -s tools/tests -p "test_check_scripts.py" -v
"""

import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import check_layer_purity  # noqa: E402
import check_no_geometry_shader  # noqa: E402
import check_no_raw_alloc  # noqa: E402
import check_scene_no_clock  # noqa: E402


def make_project(tmp, shaders=None, src_files=None, include_files=None):
    """在 tmp 下造一个最小 MagicBlocks 目录树。"""
    os.makedirs(os.path.join(tmp, "shaders"), exist_ok=True)
    # 合规的 shader 样本
    shaders = shaders if shaders is not None else {
        "fullscreen.vert": "#version 450\nlayout(location=0) in vec2 aPos;\nvoid main(){gl_Position=vec4(aPos,0,1);}\n",
        "fullscreen.frag": "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){o=vec4(1);}\n",
    }
    for name, body in shaders.items():
        with open(os.path.join(tmp, "shaders", name), "w", encoding="utf-8") as f:
            f.write(body)
    for rel, body in (src_files or {}).items():
        path = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
    for rel, body in (include_files or {}).items():
        path = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
    return tmp


def run(mod, root):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = mod.main(["--root", root])
    return rc, buf.getvalue()


class GateNoGeometryShaderTest(unittest.TestCase):
    def test_compliant_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp)
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("无几何着色器", out)

    def test_pragma_geometry_shader_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, shaders={
                "s.vert": "#version 450\n#pragma shader_stage(geometry)\nvoid main(){}\n"})
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("几何着色器", out)

    def test_emit_vertex_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, shaders={
                "s.vert": "#version 450\nvoid main(){ EmitVertex(); EndPrimitive(); }\n"})
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 1, out)

    def test_glslc_geometry_stage_fails(self):
        # 关键:目标文件用 .vert 后缀,确保命中「-fshader-stage=geometry」规则本身,
        # 而不是被 `.geom 后缀`规则先命中(第 3 轮审阅指出的假绿)。
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "CMakeLists.txt":
                    "add_custom_command(COMMAND glslc -fshader-stage=geometry -o a.spv s.vert)\n"})
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("glslc 编译几何着色器", out)

    def test_glslc_geometry_stage_with_spaces_fails(self):
        # ` = ` 空格写法也必须被抓到(脚本 docstring 声称兼容,需有测试保障)。
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "CMakeLists.txt":
                    "add_custom_command(COMMAND glslc -fshader-stage = geom -o a.spv s.vert)\n"})
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("glslc 编译几何着色器", out)

    def test_adjacency_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vk.cpp": "void f(){ draw(Primitive::triangles with adjacency); }\n"})
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 1, out)

    def test_missing_shader_dir_fails(self):
        """着色器根本不存在时不得静默通过(第 1 版审阅指出的漏洞)。"""
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "src"), exist_ok=True)
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 2, "root 存在但 shaders/ 缺失应报错: %s" % out)
            self.assertIn("shaders/", out)

    def test_empty_shader_dir_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "shaders"), exist_ok=True)
            rc, out = run(check_no_geometry_shader, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("没有任何 shader", out)


class GateLayerPurityTest(unittest.TestCase):
    def test_compliant_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp,
                         src_files={"src/backend/vulkan/vk_device.cpp":
                                    '#include <vulkan/vulkan.h>\nVkDevice d;\nvoid f(){vkCreateDevice(nullptr,nullptr,&d);}\n'},
                         include_files={"include/magicblocks/device/render_device.h":
                                        "struct IRenderDevice { virtual ~IRenderDevice() = default; };\n"})
            rc, out = run(check_layer_purity, tmp)
            self.assertEqual(rc, 0, out)

    def test_vk_type_outside_backend_fails(self):
        """backend 之外出现 Vk* 必须被抓到(第 1 版脚本跳过 backend,等价于永不报错)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, include_files={
                "include/magicblocks/device/leak.h": "struct X { VkDevice dev; };\n"})
            rc, out = run(check_layer_purity, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("backend 之外", out)

    def test_vulkan_include_outside_backend_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/cli/main.cpp": "#include <vulkan/vulkan.h>\nint main(){return 0;}\n"})
            rc, out = run(check_layer_purity, tmp)
            self.assertEqual(rc, 1, out)

    def test_gameplay_name_in_backend_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/special.cpp": "void draw_sketch_special(){}\n"})
            rc, out = run(check_layer_purity, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("玩法名", out)

    def test_comment_and_string_ignored(self):
        """注释/字符串里的示例写法不误报。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, include_files={
                "include/magicblocks/device/notes.h":
                    '// 这里不出现 VkDevice,仅注释示例\nconst char* k = "VkDevice never here";\n'})
            rc, out = run(check_layer_purity, tmp)
            self.assertEqual(rc, 0, out)


class GateNoRawAllocTest(unittest.TestCase):
    """G4:L1 口径 = 全仓零裸分配 + vk_mem_alloc.h 唯一注入点。

    L0 版曾豁免 backend 内 `// VMA-injection` 标记块里的裸分配。L1 收紧后该标记
    **不再豁免任何调用**,下面 `test_raw_alloc_inside_injection_block_fails` 就是
    这个口径变化的关键负向样本(L0 版这里是绿的)。
    """

    ALLOCATOR = "src/backend/vulkan/vulkan_allocator.cpp"
    VMA_OK = ('#define VMA_IMPLEMENTATION\n#include "vk_mem_alloc.h"\n'
              "void init(){ vmaCreateAllocator(nullptr,nullptr); }\n")

    def test_compliant_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vk_device.cpp": "void f(){ /* 走 VMA */ }\n",
                self.ALLOCATOR: self.VMA_OK})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("[G4] OK", out)

    def test_compliant_with_injection_mark_comment_only_passes(self):
        """标记本身作为注释「注入点存档」可以留着,只要它周围没有裸分配。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vulkan_target.cpp":
                    "void f(){\n  // VMA-injection:已由 VMA 替换,仅存档\n  vma_free(mem);\n}\n",
                self.ALLOCATOR: self.VMA_OK})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)

    def test_empty_tree_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "src"), exist_ok=True)
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("扫描 0 个文件", out)

    def test_missing_root_returns_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = run(check_no_raw_alloc, os.path.join(tmp, "nope"))
            self.assertEqual(rc, 2, out)
            self.assertIn("目录不存在", out)

    # ---- 裸分配:backend 之外 ----

    def test_raw_alloc_outside_backend_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/cli/main.cpp": "void f(){ vkAllocateMemory(d,nullptr,nullptr); }\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("裸内存分配", out)
            self.assertIn("backend 之外", out)

    def test_each_raw_alloc_entry_detected(self):
        """四个裸分配入口逐个都必须被抓到,且报出正确行号与函数名。"""
        calls = {
            "vkAllocateMemory": "void f(){ vkAllocateMemory(d,nullptr,nullptr,&m); }",
            "vkFreeMemory": "void f(){ vkFreeMemory(d,m,nullptr); }",
            "vkMapMemory": "void f(){ vkMapMemory(d,m,0,n,0,&p); }",
            "vkUnmapMemory": "void f(){ vkUnmapMemory(d,m); }",
        }
        for name, body in calls.items():
            with self.subTest(fn=name), tempfile.TemporaryDirectory() as tmp:
                make_project(tmp, src_files={"src/cli/main.cpp": "int x;\n" + body + "\n"})
                rc, out = run(check_no_raw_alloc, tmp)
                self.assertEqual(rc, 1, out)
                self.assertIn("src/cli/main.cpp:2", out)
                self.assertIn("`%s(`" % name, out)

    def test_public_include_header_raw_alloc_fails(self):
        """扫描范围涵盖 include/**:公共头里出现裸分配也必须红。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, include_files={
                "include/magicblocks/device/leak.h":
                    "inline void f(){ vkAllocateMemory(d,nullptr,nullptr,nullptr); }\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("include/magicblocks/device/leak.h:1", out)

    def test_tests_dir_raw_alloc_fails(self):
        """扫描范围涵盖 tests/**:测试里也别想偷偷裸分配。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "tests/magicblocks_test.cpp": "void f(){ vkFreeMemory(d,m,nullptr); }\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("tests/magicblocks_test.cpp:1", out)

    def test_repo_tools_subdir_raw_alloc_fails(self):
        """扫描范围涵盖 tools/**(MagicBlocks/tools 下的辅助代码)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "tools/probe/probe.cpp": "void f(){ vkMapMemory(d,m,0,n,0,&p); }\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("tools/probe/probe.cpp:1", out)

    # ---- 裸分配:backend 之内(L1 不再豁免) ----

    def test_raw_alloc_in_backend_without_mark_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vk_mem.cpp": "void f(){ vkFreeMemory(d,m,nullptr); }\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("裸内存分配", out)
            self.assertIn("backend 内", out)

    def test_raw_alloc_inside_injection_block_fails(self):
        """口径收紧的关键点:带 `// VMA-injection` 标记的 backend 内裸分配现在**必须红**。

        L0 版此样本是绿的(标记行开启注入区,到空行结束 → 豁免)。L1 起标记不再豁免
        任何调用,脚本还必须显式提示「标记已失效」,防止有人照旧文档以为这里能放过。
        """
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vk_mem.cpp":
                    "void f(){\n  // VMA-injection\n  vkAllocateMemory(d,nullptr,nullptr);\n}\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("vk_mem.cpp:3", out)
            self.assertIn("不再豁免", out)

    def test_raw_alloc_with_trailing_mark_fails(self):
        """行尾注释写法的标记同样不豁免。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vulkan_target.cpp":
                    "void f(){\n  vkFreeMemory(d,m,nullptr);  // VMA-injection\n}\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("vulkan_target.cpp:2", out)
            self.assertIn("不再豁免", out)

    def test_l0_style_nine_sites_all_reported(self):
        """L0 遗留形态:9 处逐一都必须被报出(不是只有第一处)。"""
        body = []
        for i in range(3):
            body.append("  // VMA-injection")
            body.append("  vkAllocateMemory(d,nullptr,nullptr,&m%d);" % i)
            body.append("  vkFreeMemory(d,m%d,nullptr);  // VMA-injection" % i)
        body.append("  vkMapMemory(d,m0,0,n,0,&p);  // VMA-injection")
        body.append("  vkUnmapMemory(d,m0);  // VMA-injection")
        body.append("  vkFreeMemory(d,m0,nullptr);  // VMA-injection")
        src = "void f(){\n" + "\n".join(body) + "\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={"src/backend/vulkan/vulkan_target.cpp": src})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("违规 9 处", out)

    def test_allocator_u_free_passes(self):
        """vmaCreateAllocator 等合法 VMA 调用不得被误判(只有 vkXxxMemory 才违规)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vk_device.cpp":
                    "void f(){ vkGetPhysicalDeviceMemoryProperties(pd,&m); }\n",
                self.ALLOCATOR:
                    self.VMA_OK + "void g(){ vmaDestroyAllocator(a); vmaSetCurrentFrameIndex(a,1); }\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)

    # ---- 注释 / 字符串不计 ----

    def test_comment_method_doc_ignored(self):
        """注释里写调用不算(ARCHITECTURE.md §7 的「创建一张图像并绑定内存」式文档)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vulkan_target.h":
                    "// 创建图像并绑定内存(曾用裸 vkAllocateMemory(d,m),已换 VMA)\n"
                    "/* 历史: vkFreeMemory(d,m,nullptr); */\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)

    def test_string_literal_ignored(self):
        """字符串里出现 `vkAllocateMemory(` 不算违规(陷阱清单:错误信息别带括号)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vk_device.cpp":
                    'const char* kMsg = "raw vkAllocateMemory(x) is banned";\n'})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)

    def test_string_literal_vma_include_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vk_device.cpp":
                    'const char* kHdr = "#include <vk_mem_alloc.h>";\n'})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)

    # ---- VMA 唯一注入点 ----

    def test_vma_include_in_non_allocator_file_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vulkan_target.cpp": '#include "vk_mem_alloc.h"\nvoid f(){}\n'})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("VMA 唯一注入点", out)
            self.assertIn("vulkan_target.cpp:1", out)

    def test_vma_include_angle_bracket_spaced_in_shared_header_fails(self):
        """`#  include  <vk_mem_alloc.h>`(空格+尖括号)也要抓到;静态库里别处 include 同样红。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, include_files={
                "include/magicblocks/resources/memory.h":
                    "#  include  <vk_mem_alloc.h>\nstruct S {};\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("include/magicblocks/resources/memory.h:1", out)

    def test_vma_include_in_allocator_header_fails(self):
        """唯一注入点是 .cpp;allocator 的 .h 里 include 也不行(会把 VMA 漏给全仓)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vulkan_allocator.h": "#include <vk_mem_alloc.h>\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("vulkan_allocator.h:1", out)

    def test_vma_implementation_outside_allocator_fails(self):
        """VMA_IMPLEMENTATION 也必须在唯一注入点定义,否则每 TU 一份实现。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vulkan_device.cpp": "#define VMA_IMPLEMENTATION\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("VMA_IMPLEMENTATION", out)

    def test_vma_include_in_allocator_cpp_passes(self):
        for spelling in ('#include <vk_mem_alloc.h>',
                         '#include "vk_mem_alloc.h"',
                         '#  include "vk_mem_alloc.h"  // VMA'):
            with self.subTest(src=spelling), tempfile.TemporaryDirectory() as tmp:
                make_project(tmp, src_files={self.ALLOCATOR: spelling + "\n"})
                rc, out = run(check_no_raw_alloc, tmp)
                self.assertEqual(rc, 0, out)

    def test_vendored_third_party_copy_excluded(self):
        """构建产物/第三方树里自带的 vk_mem_alloc.h 本体不参与门控(否则永远红)。

        这里以 build 产物的形态给一个样本:排除生效 → 绿。
        """
        with tempfile.TemporaryDirectory() as tmp:
            make_project(tmp, src_files={
                "src/backend/vulkan/vk_device.cpp": "void f(){ vmaCreateAllocator(nullptr,nullptr); }\n",
                self.ALLOCATOR: self.VMA_OK,
                "build/_deps/vma-src/vk_mem_alloc.h":
                    "#define VMA_IMPLEMENTATION\nvoid vkAllocateMemory(){}\n"})
            rc, out = run(check_no_raw_alloc, tmp)
            self.assertEqual(rc, 0, out)


class GateSceneNoClockTest(unittest.TestCase):
    """L2 时钟门控:场景层不允许出现任何时钟/计时 API(spec D13)。

    正向:干净代码必须 rc=0。
    负向:每种时钟写法都必须 rc=1 —— 门控脚本只"存在"不"断言"是第 11 版审阅
    点名过的失效模式,所以每个 pattern 都要有独立负向样本。
    """

    SCENE_SRC = "src/scene/command_stream.cpp"
    SCENE_INC = "include/magicblocks/scene/scene.h"

    @staticmethod
    def _scene_project(tmp, body, rel=None):
        rel = rel or GateSceneNoClockTest.SCENE_SRC
        path = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
        return tmp

    def test_clean_scene_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._scene_project(tmp, "void f(){ unsigned long long t = 42; }\n")
            rc, out = run(check_scene_no_clock, tmp)
            self.assertEqual(rc, 0, out)

    def test_header_also_scanned(self):
        """include/magicblocks/scene/** 同样在范围内(不只是 src/)。"""
        with tempfile.TemporaryDirectory() as tmp:
            self._scene_project(
                tmp, "inline auto now(){ return std::chrono::steady_clock::now(); }\n",
                rel=self.SCENE_INC)
            rc, out = run(check_scene_no_clock, tmp)
            self.assertEqual(rc, 1, out)

    def test_each_clock_api_fails(self):
        spellings = [
            "auto a = std::chrono::steady_clock::now();",
            "auto a = std::chrono::system_clock::now();",
            "auto a = std::chrono::high_resolution_clock::now();",
            "auto a = steady_clock::now();",
            "auto a = system_clock::now();",
            "auto a = high_resolution_clock::now();",
            "std::time_t a = time(nullptr);",
            "clock_t a = clock();",
            "DWORD a = GetTickCount();",
            "DWORD64 a = GetTickCount64();",
            "LARGE_INTEGER c; QueryPerformanceCounter(&c);",
            "gettimeofday(&tv, nullptr);",
            "clock_gettime(CLOCK_MONOTONIC, &ts);",
            "timespec_get(&ts, TIME_UTC);",
        ]
        for spelling in spellings:
            with self.subTest(spelling=spelling), tempfile.TemporaryDirectory() as tmp:
                self._scene_project(tmp, spelling + "\n")
                rc, out = run(check_scene_no_clock, tmp)
                self.assertEqual(rc, 1, "未抓到: %s -- %s" % (spelling, out))

    def test_comment_mentions_are_ignored(self):
        """注释里提到时钟不算违规(门控只认代码行)。"""
        body = (
            "// 这里绝不能写 std::chrono::steady_clock\n"
            "/* 历史实现用过 time( 和 clock( */\n"
            "void f(){ int x = 0; }\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            self._scene_project(tmp, body)
            rc, out = run(check_scene_no_clock, tmp)
            self.assertEqual(rc, 0, out)

    def test_string_literal_mentions_are_ignored(self):
        """错误消息里写出 API 名字不算违规。"""
        body = 'const char* msg = "禁止 time( 与 std::chrono";\nvoid f(){}\n'
        with tempfile.TemporaryDirectory() as tmp:
            self._scene_project(tmp, body)
            rc, out = run(check_scene_no_clock, tmp)
            self.assertEqual(rc, 0, out)

    def test_tests_and_tools_not_scanned(self):
        """tests/ 与 tools/ 不在范围内(测试本身可以用时钟)。"""
        with tempfile.TemporaryDirectory() as tmp:
            for rel in ("tests/clock_using_test.cpp", "tools/helper.cpp"):
                path = os.path.join(tmp, rel)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8") as f:
                    f.write("auto t = std::chrono::steady_clock::now();\n")
            rc, out = run(check_scene_no_clock, tmp)
            self.assertEqual(rc, 0, out)

    def test_violation_reports_file_and_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._scene_project(tmp, "void f(){}\nvoid g(){ time(nullptr); }\n")
            rc, out = run(check_scene_no_clock, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn(":2", out)
            self.assertIn(self.SCENE_SRC, out)


if __name__ == "__main__":
    unittest.main()
