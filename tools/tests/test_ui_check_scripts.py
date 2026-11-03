"""UI 消费层门控自测:tools/check_ui_no_readback.py(UIG1)与
tools/check_ui_single_submit.py(UIG2)。

与 test_check_scripts.py 同风格:用 `tempfile.TemporaryDirectory` 造
`MagicBlocks/src/ui/...` 最小树,在「违规样本」上断言红、在「合规样本」上断言绿。
区别是这里按任务要求走**子进程黑盒**调用:`subprocess.run([sys.executable, script,
"--root", tmpdir])`,断言退出码与输出关键字 —— 这样退出码语义就是终端里真实看到的语义。

运行: python -m unittest discover -s tools/tests -p "test_ui_check_scripts.py" -v
"""

import os
import subprocess
import sys
import tempfile
import unittest

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NO_READBACK = os.path.join(TOOLS_DIR, "check_ui_no_readback.py")
SINGLE_SUBMIT = os.path.join(TOOLS_DIR, "check_ui_single_submit.py")

# 每棵临时树默认的合规样本:一个 Presenter(允许提交)、一个 HUD、一个 offscreen 后端。
ELEMENTS_OK = {
    "src/ui/presenter.cpp":
        "void Presenter::DrawFrame() {\n"
        "  cmd->Draw();\n"
        "  queue->submit(cmd);\n"
        "}\n",
    "src/ui/hud/overlay.cpp":
        "void Overlay::Record(ICommandList* cmd) {\n"
        "  cmd->DrawUiBatch();\n"
        "}\n",
    "src/ui/offscreen/capture.cpp":
        "void Capture::Save() {\n"
        "  readback_rgba8(&img, &pixels);\n"
        "  vkMapMemory(dev, mem, 0, sz, 0, &p);\n"
        "  vkCmdCopyImageToBuffer(cb, img, LAYOUT, buf, 1, &region);\n"
        "  save_png(pixels, w, h);\n"
        "}\n",
}


def make_tree(tmp, files=None):
    """在 tmp 下造最小 MagicBlocks 树,默认带一套全合规样本。"""
    files = ELEMENTS_OK if files is None else files
    for rel, body in files.items():
        path = os.path.join(tmp, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
    return tmp


def run_gate(script, root):
    """子进程跑门控脚本,返回 (rc, stdout+stderr)。

    注意:Windows 控制台默认 GBK,而两个门控脚本统一按 UTF-8 输出(并带 errors=replace);
    这里必须用 **bytes** 接、再自己按 UTF-8 解码。若用 text=True 让父进程按本地
    cp936 解码,中文断言会全部乱码 —— 那是测试的锅,不是脚本的锅。
    另外显式设 PYTHONIOENCODING=utf-8,模拟 CI/终端同样以 UTF-8 呈现的行为。
    """
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    proc = subprocess.run([sys.executable, script, "--root", root],
                          capture_output=True, env=env)
    out = (proc.stdout or b"") + (proc.stderr or b"")
    return proc.returncode, out.decode("utf-8", "replace")


class UiNoReadbackGateTest(unittest.TestCase):
    """UIG1:上屏零回读(spec §5.4 / §10 验收门 1)。"""

    def test_compliant_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp)
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("[UIG1] OK", out)

    def test_empty_ui_dir_passes(self):
        """src/ui 存在但为空 -> 0(目录缺失才是 2)。"""
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "src", "ui"))
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("扫描 0 个 UI 文件", out)

    def test_no_ui_dir_passes(self):
        """骨架还没落地(root 为空)不是环境错误:没有 UI 代码就没有回读。"""
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "src"))
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)

    def test_missing_root_returns_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = run_gate(NO_READBACK, os.path.join(tmp, "nope"))
            self.assertEqual(rc, 2, out)
            self.assertIn("目录不存在", out)

    def test_readback_in_presenter_fails(self):
        """呈递路径上出现 readback_rgba8( 必须红。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, dict(ELEMENTS_OK, **{
                "src/ui/presenter_gpu.cpp":
                    "void Presenter::Grab() {\n  readback_rgba8(&img, &px);\n}\n"}))
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("presenter_gpu.cpp:2", out)
            self.assertIn("readback_rgba8", out)

    def test_vkmapmemory_in_presenter_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, dict(ELEMENTS_OK, **{
                "src/ui/present/presenter.h":
                    "inline void f(){ vkMapMemory(d, m, 0, n, 0, &p); }\n"}))
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("vkMapMemory", out)

    def test_copy_image_to_buffer_in_presenter_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, dict(ELEMENTS_OK, **{
                "src/ui/presenter_frame.cpp":
                    "void f(){ vkCmdCopyImageToBuffer(cb, img, L, buf, 1, &r); }\n"}))
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("CopyImageToBuffer", out)

    def test_each_keyword_detected_anywhere_in_ui(self):
        """全 UI 层:每个关键字逐条都要被抓到(含非 present 文件名,如 hud/)。"""
        samples = {
            "readback_rgba8": "readback_rgba8(&img, &px);",
            "present_to_cpu": "present_to_cpu(img, w, h);",
            "readback": "auto rb = frame_readback_state;",
            "vkMapMemory": "vkMapMemory(dev, mem, 0, n, 0, &p);",
            "vmaMapMemory": "vmaMapMemory(alloc, m, &p);",
            "CopyImageToBuffer": "cfg.copy = CopyImageToBuffer;",
            "vkCmdCopyImageToBuffer": "vkCmdCopyImageToBuffer(cb, img, L, buf, 1, &r);",
            "arrow_map": "auto* px = staging->map();",
            "dot_map": "auto* px = ring_.map();",
        }
        for fn, code in samples.items():
            with self.subTest(kw=fn), tempfile.TemporaryDirectory() as tmp:
                make_tree(tmp, dict(ELEMENTS_OK, **{
                    "src/ui/hud/probe.cpp": "int x;\n" + code + "\n"}))
                rc, out = run_gate(NO_READBACK, tmp)
                self.assertEqual(rc, 1, "%s 未被抓到: %s" % (fn, out))
                self.assertIn("hud/probe.cpp:2", out)

    def test_offscreen_subdir_exempt(self):
        """offscreen 子目录里的 readback_rgba8 是 spec §5.5 允许的 -> 0。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp)
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("豁免 1 个", out)

    def test_offscreen_filename_in_present_dir_exempt(self):
        """`src/ui/presenter_offscreen.cpp`(路径含 offscreen)同样豁免。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/presenter_offscreen.cpp":
                    "void f(){ auto* p = img->map(); readback_rgba8(&img, p); }\n"})
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)

    def test_comment_and_string_ignored(self):
        """注释/字符串里出现关键字不算(只认代码行)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/presenter.cpp":
                    "// 绝不调用 vkCmdCopyImageToBuffer / readback_rgba8(x)\n"
                    "/* 历史: vkMapMemory(d,m,0,n,0,&p); */\n"
                    "const char* kMsg = \"readback_rgba8( is banned\";\n"
                    "void f(){ queue->submit(cmd); }\n"})
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)

    def test_comment_marker_does_not_grant_exemption(self):
        """注释里写个记号不免检:豁免只看路径。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/hud/steal.cpp":
                    "// allow-readback\n"
                    "void f(){ readback_rgba8(&img, &px); }\n"})
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("steal.cpp:2", out)

    def test_indirection_allow_macro_does_not_grant_exemption(self):
        """`#define` 个名字带 allowed 的宏也不免检。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/hud/steal2.cpp":
                    "#define READBACK_ALLOWED 1\n"
                    "void f(){ vkMapMemory(d, m, 0, n, 0, &p); }\n"})
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 1, out)

    def test_std_map_not_false_positive(self):
        """`std::map(` / `unordered_map(` 不是回读,不得误报(否则门控会被迫忽略)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/hud/registry.cpp":
                    "std::map<int, int> m = std::map<int, int>();\n"
                    "std::unordered_map<int,int> u;\n"
                    "void f(){ auto it = table.find(1); }\n"})
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)

    def test_tests_dir_ui_file_scanned(self):
        """tests/ 下文件名含 ui 的文件也在范围内。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, dict(ELEMENTS_OK, **{
                "tests/ui_presenter_test.cpp":
                    "void test(){ CopyImageToBuffer(img, buf); }\n"}))
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("tests/ui_presenter_test.cpp:1", out)

    def test_tests_dir_non_ui_file_ignored(self):
        """tests/ 下文件名不含 ui 的文件不在范围(它是 SDK 测试,离屏读回合法)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, dict(ELEMENTS_OK, **{
                "tests/magicblocks_test.cpp":
                    "void test(){ readback_rgba8(&img, &px); }\n"}))
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 0, out)

    def test_violation_count_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/presenter.cpp":
                    "void f(){\n  vkMapMemory(d,m,0,n,0,&p);\n  readback_rgba8(&i,&p);\n}\n"})
            rc, out = run_gate(NO_READBACK, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("违规 2 处", out)


class UiSingleSubmitGateTest(unittest.TestCase):
    """UIG2:主路径每帧单次提交(spec §5.1 P3 / §5.4 / §10 验收门 2)。"""

    def test_compliant_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp)
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("[UIG2] OK", out)

    def test_empty_ui_dir_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "src", "ui"))
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("扫描 0 个 UI 文件", out)

    def test_missing_root_returns_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = run_gate(SINGLE_SUBMIT, os.path.join(tmp, "nope"))
            self.assertEqual(rc, 2, out)
            self.assertIn("目录不存在", out)

    def test_submit_outside_presenter_fails(self):
        """UI 单独一次 submit(§5.4 末行)最典型的退化:提交跑进 HUD/Router。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, dict(ELEMENTS_OK, **{
                "src/ui/hud/overlay.cpp":
                    "void Overlay::Record(ICommandList* cmd) {\n  queue->submit(cmd);\n}\n"}))
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("hud/overlay.cpp:2", out)
            self.assertIn("PresentPaths 之外", out)

    def test_each_submit_form_detected(self):
        samples = {
            "arrow": "queue->submit(cmd);",
            "arrow_space": "queue->submit (cmd);",
            "dot": "device.submit(cmd);",
            "dot_template": "device.submit<Bindless>(cmd);",
            "vkQueueSubmit": "vkQueueSubmit(q, 1, &si, fence);",
            "vkQueueSubmit2": "vkQueueSubmit2(q, 1, &si, fence);",
            "QueueSubmit": "renderer.QueueSubmit(cmd);",
            "queue_submit": "queue_submit(q, cmd);",
        }
        for name, code in samples.items():
            with self.subTest(form=name), tempfile.TemporaryDirectory() as tmp:
                make_tree(tmp, dict(ELEMENTS_OK, **{
                    "src/ui/hud/probe.cpp": "int x;\n" + code + "\n"}))
                rc, out = run_gate(SINGLE_SUBMIT, tmp)
                self.assertEqual(rc, 1, "%s 未被抓到: %s" % (name, out))
                self.assertIn("hud/probe.cpp:2", out)

    def test_submit_in_presenter_passes(self):
        """提交出现在 src/ui/presenter_*.cpp 属正常(帧提交点)-> 0。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/presenter_main.cpp":
                    "void Presenter::DrawFrame(){ queue->submit(cmd); vkQueueSubmit(q,1,&si,f); }\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 0, out)
            self.assertIn("2 处提交", out)

    def test_submit_in_presenter_offscreen_passes(self):
        """离屏录制走有自己的提交点(presenter_offscreen.cpp 也是呈递路径)。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/presenter_offscreen.cpp": "void f(){ queue->submit(cmd); }\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 0, out)

    def test_present_directory_without_present_basename_fails(self):
        """豁免看 basename:`src/ui/present_utils/queue.cpp` 不得被整目录免检。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/present_utils/queue.cpp": "void f(){ queue->submit(cmd); }\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("present_utils/queue.cpp:1", out)

    def test_offscreen_save_dir_not_exempt_from_submit(self):
        """UIG1 的 offscreen 豁免**不**延伸到本门控:离屏也不许自己再开一个提交点。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/offscreen/capture.cpp": "void f(){ queue->submit(cmd); }\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 1, out)

    def test_bare_submit_signal_not_false_positive(self):
        """裸 `submit(`(如自定义 Signal::submit)不误报 —— 只认 `->`/`.`/QueueSubmit。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/router/signal.cpp": "void f(){ onSubmit(cb); button.submitHandler(); }\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 0, out)

    def test_comment_and_string_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/hud/overlay.cpp":
                    "// 不要 queue->submit(cmd),UI 与场景同一次提交\n"
                    "/* vkQueueSubmit(q,1,&si,f); */\n"
                    'const char* kErr = "queue->submit( called twice";\n'})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 0, out)

    def test_comment_marker_does_not_grant_exemption(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/hud/steal.cpp":
                    "// allow-submit\nvoid f(){ queue->submit(cmd); }\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 1, out)

    def test_tests_dir_submit_fails(self):
        """tests/ 下提交同样算第二条上屏路径。"""
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "tests/magicblocks_test.cpp": "void t(){ vkQueueSubmit(q,1,&si,f); }\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("tests/magicblocks_test.cpp:1", out)

    def test_violation_count_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_tree(tmp, {
                "src/ui/router/router.cpp":
                    "void f(){\n  a->submit(cmd);\n  b.submit(cmd);\n}\n"})
            rc, out = run_gate(SINGLE_SUBMIT, tmp)
            self.assertEqual(rc, 1, out)
            self.assertIn("违规 2 处", out)


if __name__ == "__main__":
    unittest.main()
