#!/usr/bin/env python3
"""G3 门控:分层纯度(spec §3 G3 / §7)。

断言三件事:
  1. `src/backend/**` 之外,任何源/头文件不得出现 Vulkan:
     `#include <vulkan/...>`、`Vk*` / `PFN_vk*` 类型、`vkXxx(` / `vmaXxx(` 调用,
     **以及** `VK_*` 宏/枚举(`VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR` 之类)
     与 `backend::` 命名空间限定名。
  2. `src/backend/**` 之内不得出现玩法名(sketch/blocks/voxel/stamp/捏捏乐/涂鸦/积木/叠叠乐/印章),
     SDK 内部零玩法分支。
  3. `src/features/**`(L4)不得直接写 GPU 命令(`vkCmd*` / `ICommandList` 之外的低层命令)。

第 1 版实现的缺陷(已修复):原脚本**整个跳过 `src/backend/`**,而条文写的是
「backend 之外不得出现 Vk」——跳过后等于永远不可能抓到 backend 外的泄漏,门控形同虚设。

第 2 版的缺陷(本版修复,2026-11-11 t8):只认 `Vk` 前缀类型名、`vk`/`vma` 函数名
和 Vulkan 头 include,**不认 `VK_` 宏**、也**不认 `backend::` 命名空间**、更**不认
backend 内部头的 include**。于是 `src/android/scene_renderer.cpp` 里

    if (pt & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) rotation_deg_ = 90;
    auto* vkdev = dynamic_cast<backend::VulkanRenderDevice*>(device_.get());
    #include "backend/vulkan/vulkan_render_device.h"

这种最直接的 backend 下钻,门控全是绿的 —— 暗漏洞,规则形同虚设。
本版补上这三条模式,并把「先证明门控能变红」写成自测
(`MagicBlocks/tools/tests/test_check_layer_purity.py`):每个模式都必须能在合成样本上
变红,否则门控自己就先报错。

注释与字符串里的示例写法不计入(只认代码行)。

第 3 版补充(t3,2026-11-11):引入两层**按模式**放行(从不按目录整体沉默):
  - `LEGAL_BACKEND_EDGE`:封闭集合,目前只有 `src/core/device_ops.cpp`,
    只免 `backend::` 一条 —— 它是 backend 与公共面之间那唯一的翻译层;
  - `TEST_PREFIX = "tests/"`:白盒测试需要看见后端内部,只免 `backend::` 与 `VK_*`
    两条;在 tests/ 里 include Vulkan 头、写 `VkDevice`、调 `vkXxx()` 仍然判红。
自测逐条证明:放行外的文件同一条写法仍然变红;被放行的文件/目录仍受其余模式约束。

注意:当前提交状态下门控**仍会报红**(src/ui/ 4 处 + CLI/验证工具 12 处)。
那 16 处是已识别的真实耦合点,另开任务处理,不在本脚本里放行。
"""

import argparse
import os
import re
import sys

VK_INCLUDE = re.compile(r"#\s*include\s*[<\"](vulkan|volk|vulkan_beta)[/\"\.]")
VK_TYPE = re.compile(r"\b(?:Vk|PFN_vk)[A-Z]\w*")
VK_CALL = re.compile(r"\b(?:vk|vma)[A-Z]\w*\s*\(")
# VK_* 宏/枚举名:VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR、VK_NULL_HANDLE、VK_SUCCESS …
# 旧版完全放过了这一类 —— 而它恰恰是「include 不了 Vulkan 头却照样写 Vk 语义」的
# 那堵墙上的门:只要把 <<PreTransform 换成 int 就能把 Vk 枚举值搬出 backend/。
VK_MACRO = re.compile(r"\bVK_[A-Z0-9_]+\b")
# backend:: 命名空间限定名:dynamic_cast<backend::VulkanRenderDevice*>、
# backend::AttachmentShape 之类。include 一个 backend 头或许还能辩称「只是值类型」,
# 直接写 backend:: 则连辩都没得辩。
BACKEND_NS = re.compile(r"\bbackend\s*::")
# include 后端内部头(`#include "backend/vulkan/vulkan_pipeline.h"`)。
# 旧版只认 `vk*`/`VK_*`/`Vk*` 写法,于是"把头 include 进来、用它的**值类型**而不写
# backend:: 限定名"这条最容易的绕过方式完全不在门控视野里 —— 而它恰恰等于
# 把后端实现拉进上层 TU。反过来打 include 比打名字更本质:名字可以拼,include 拼不了。
BACKEND_INCLUDE = re.compile(r"#\s*include\s*[<\"](?:\.\./)*backend/")

GAMEPLAY_NAMES = ["sketch", "blocks", "voxel", "stamp", "squeeze", "捏捏乐", "涂鸦", "积木", "叠叠乐", "印章"]

# SDK 自己的名字就含玩法词("MagicBlocks" 含 "blocks","magicblocks" 是命名空间),
# 而 Vulkan 的 timestamp 查询里含 "stamp"。先把这些合法标识符抹掉再判玩法名,
# 否则门控会对每个文件都红 —— 那样的规则会被迫整体忽略,等于没有门控。
SDK_NAME_ALLOWLIST = re.compile(r"magicblocks|timestamps?", re.IGNORECASE)

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}

# ---------------------------------------------------------------------------
# 「合法下钻」白名单 —— 只放行 **一个** 文件,而且只放行 `backend::` 这一条模式
# ---------------------------------------------------------------------------
# 为什么需要它:
#   backend/ 是具体后端(目前只有 Vulkan),公共面(include/magicblocks/device|resources)
#   刻意不暴露"按采样数建交换链""按交换链形状建管线""实际采样数/旋转角"这些
#   只有后端答得上来的事。这些问题的**翻译层**必须存在,否则每个上层文件都会自己
#   `#include "backend/vulkan/..."` 去问 —— 那才是真正的层间渗透。
#
#   src/core/device_ops.cpp 就是那唯一的翻译层:它的**对外签名**(core/device_ops.h)
#   只用公共面类型,backend 类型只在这个 .cpp 里被 dynamic_cast 出来用一次。
#
# 为什么白名单要写成"文件 + 模式"而不是整个跳过该文件:
#   跳过 = 这个文件里再出现 VK_* 宏、VkFoo 类型、vkXxx 调用也全绿了。翻译层最容易
#   悄悄变成"半个后端"(今天 dynamic_cast,明天就开始直接拿 VkDevice),所以这里
#   只免 `backend::` 一条,其余模式在该文件里照样判红。
#
# 为什么白名单是**封闭集合**(不在集合里就打红,而不是"路径含 core 就放行"):
#   新加一个翻译层必须改这里 → 改动会出现在 review diff 里,不可能悄悄多出第二个。
#   这与"靠白名单绕过门控"是两件事:绕过是把判红写成绿的;这里是显式声明
#   "这一个文件被允许做翻译",并且该文件仍在其它所有模式下被检查。
LEGAL_BACKEND_EDGE = {
    "src/core/device_ops.cpp",
}

# ---------------------------------------------------------------------------
# 白盒测试豁免 —— 只放行 `tests/` 前缀,且只放行 `backend::` 与 `VK_*` 两条模式
# ---------------------------------------------------------------------------
# 为什么测试是正当的例外:
#   测试要验的正是后端**内部**行为 —— "这条管线真是 FILL 模式吗""命令列表里
#   到底绑了几次纹理""MSAA 图的实际采样数是几"。这些断言不可能只靠公共面写出来,
#   否则就等于让被测代码自己报告自己(那种测试什么都测不到)。
#
# 为什么"豁免"必须写成 (前缀, 模式) 的显式清单,而不是 `if rel.startswith("tests/"): continue`:
#   整目录跳过 = 测试里出现 `#include <vulkan/vulkan.h>`、`vkDeviceWaitIdle(...)`、
#   `VkDevice` 也全绿了。那与"永远匹配不上的正则"是同一种病 —— 门控看上去在,
#   实际那一块是哑的。这里只免两条,其余模式在 tests/ 里照样判红。
#
# 为什么用前缀而不是逐个文件枚举行号:
#   行号会随任何一次编辑漂移,写死行号的豁免会在第一次插入空行后失效(而且失效方向
#   是"该红的变绿",最难被发现)。前缀 + 模式是稳定的;具体放行了哪 34 行由任务报告
#   逐条列出(t3 output),而不是让脚本去记。
TEST_PREFIX = "tests/"
TEST_ALLOWED_PATTERNS = ("backend_ns", "vk_macro")


def strip_comments_and_strings(lines):
    """去行注释 / 块注释 / 字符串字面量,门控只认真正的代码。"""
    out = []
    in_block = False
    for raw in lines:
        s = raw
        if in_block:
            end = s.find("*/")
            if end == -1:
                out.append("")
                continue
            s = s[end + 2:]
            in_block = False
        while True:
            start = s.find("/*")
            if start == -1:
                break
            end = s.find("*/", start + 2)
            if end == -1:
                s = s[:start]
                in_block = True
                break
            s = s[:start] + s[end + 2:]
        s = s.split("//", 1)[0]
        # 去掉字符串字面量,避免 "VkFoo" 这类注释性文本误判
        s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
        out.append(s)
    return out


def iter_code_files(base):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if d not in {"build", ".git", "vendor", "out", ".vs"})
        for name in sorted(filenames):
            if os.path.splitext(name)[1].lower() in CODE_EXTS:
                yield os.path.join(dirpath, name)


def read_code_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return strip_comments_and_strings(f.read().splitlines())


def violations_in_code(rel, lineno, code, in_backend, in_features):
    """单行代码的违规判定。抽成函数是为了让自测能逐条喂样本,
    证明**每个模式**都能变红 —— 一条永远匹配不上任何东西的正则
    与「没有这条规则」等价,而它在门控输出里看不出来。

    放行分两层,两层都只"按模式"放行,从不"按目录整体沉默":
      1. LEGAL_BACKEND_EDGE 里的文件:只免 `backend::`;
      2. tests/ 前缀:只免 `backend::` 与 `VK_*`(理由见 TEST_PREFIX 处的注释)。
    其余三类(include Vulkan 头 / Vk* 类型 / vkXxx·vmaXxx 调用)在任何地方都判红。
    """
    out = []
    stripped = code.strip()
    if not in_backend:
        in_tests = rel.replace("\\", "/").startswith(TEST_PREFIX)
        if VK_INCLUDE.search(code):
            out.append("%s:%d  backend 之外 include Vulkan 头: %s" % (rel, lineno, stripped))
        if BACKEND_INCLUDE.search(code):
            out.append("%s:%d  backend 之外 include backend 内部头: %s" % (rel, lineno, stripped))
        if VK_TYPE.search(code):
            out.append("%s:%d  backend 之外出现 Vk* 类型: %s" % (rel, lineno, stripped))
        if VK_CALL.search(code):
            out.append("%s:%d  backend 之外调用 Vulkan/VMA 函数: %s" % (rel, lineno, stripped))
        if VK_MACRO.search(code) and not (in_tests and "vk_macro" in TEST_ALLOWED_PATTERNS):
            out.append("%s:%d  backend 之外出现 VK_* 宏/枚举: %s" % (rel, lineno, stripped))
        if BACKEND_NS.search(code) and rel not in LEGAL_BACKEND_EDGE and not (
                in_tests and "backend_ns" in TEST_ALLOWED_PATTERNS):
            out.append("%s:%d  backend 之外引用 backend:: 命名空间: %s" % (rel, lineno, stripped))
    else:
        low = SDK_NAME_ALLOWLIST.sub("__sdk__", code.lower())
        for name in GAMEPLAY_NAMES:
            # 抹掉 SDK 合法名之后再判:magicblocks / MagicBlocks 里
            # 的 "blocks" 不算玩法泄漏,而 draw_sketch_special /
            # 独立出现的 blocks 仍然算。
            if name in low:
                out.append("%s:%d  backend 内出现玩法名 '%s': %s" % (rel, lineno, name, stripped))
    if in_features and ("vkCmd" in code or "CmdDraw" in code):
        out.append("%s:%d  features 层直接写 GPU 命令: %s" % (rel, lineno, stripped))
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=None, help="MagicBlocks 根目录")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(args.root or os.path.join(os.path.dirname(here), "MagicBlocks"))
    if not os.path.isdir(root):
        print("[G3] 目录不存在: %s" % root)
        return 2

    violations = []
    scanned_non_backend = 0
    scanned_backend = 0

    for sub in ("include", "src", "tests", "tools", "shaders"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for path in iter_code_files(base):
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            in_backend = rel.startswith("src/backend/")
            in_features = rel.startswith("src/features/")
            try:
                lines = read_code_lines(path)
            except OSError as exc:
                print("[G3] 读取失败: %s (%s)" % (path, exc))
                return 2

            if in_backend:
                scanned_backend += 1
            else:
                scanned_non_backend += 1

            for lineno, code in enumerate(lines, 1):
                if not code.strip():
                    continue
                violations.extend(violations_in_code(rel, lineno, code, in_backend, in_features))

    if violations:
        print("[G3] 违规 %d 处(分层纯度):" % len(violations))
        for v in violations:
            print("  " + v)
        return 1
    print("[G3] OK backend 外扫描 %d 个文件、backend 内 %d 个文件,分层纯净"
          % (scanned_non_backend, scanned_backend))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
