#!/usr/bin/env python3
"""G1 门控:禁止几何着色器(spec §3 G1 / §7)。

扫描 `MagicBlocks/shaders/**`(必须存在且非空)与 `MagicBlocks/src/**`,出现下列任一即失败:
  - GLSL 里 `#pragma shader_stage(geom|geometry)`
  - 几何着色器专属内置:gl_PrimitiveIDIn / EmitVertex / EndPrimitive / gl_in[
  - 邻接图元:triangles with adjacency / GL_*_ADJACENCY
  - glslc 编译几何 stage:`-fshader-stage=geom|geometry`(兼容 `=` 与 ` = ` 两种写法)
  - 几何着色器源文件后缀 `.geom` / `.geometry`

另外断言 `MagicBlocks/shaders/` 目录**存在且含至少一个 shader 文件**——
避免"着色器根本不存在"时门控静默通过(第 1 版审阅指出)。

用法: python tools/check_no_geometry_shader.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。
"""

import argparse
import os
import re
import sys

FORBIDDEN_PATTERNS = [
    (re.compile(r"#\s*pragma\s+shader_stage\s*\(\s*(geom|geometry)\s*\)"), "GLSL shader_stage 指示几何着色器"),
    (re.compile(r"gl_PrimitiveIDIn"), "几何着色器专属内置 gl_PrimitiveIDIn"),
    (re.compile(r"\bEmitVertex\s*\("), "几何着色器专属内置 EmitVertex"),
    (re.compile(r"\bEndPrimitive\s*\("), "几何着色器专属内置 EndPrimitive"),
    (re.compile(r"\bgl_in\s*\["), "几何着色器专属输入数组 gl_in[]"),
    (re.compile(r"-fshader-stage\s*=\s*(geom|geometry)\b"), "glslc 编译几何着色器 stage"),
    (re.compile(r"\btriangles\s+with\s+adjacency\b", re.I), "邻接三角形图元"),
    (re.compile(r"GL_(TRIANGLES|LINES|LINES_STRIP)_ADJACENCY"), "邻接图元枚举"),
    (re.compile(r"\b(?:shader|Shader|Stage)\s*[:=]\s*(geom|geometry)\b"), "shader stage 声明为 geometry"),
    (re.compile(r"\bVK_SHADER_STAGE_GEOMETRY_BIT\b"), "Vulkan 几何着色器 stage 位"),
    # 几何着色器源文件后缀。只匹配"像文件路径里出现 .geom/.geometry"的情形:
    #   前面必须是路径分隔符/引号/行首(即后缀紧跟在一个文件名后面),
    #   后面必须是真正的文件名字符之后的行尾/引号/空白。
    # 不这么收紧的话,`painter.geometry()`、`p.geometry().vertices` 这类
    # **普通 C++ 方法调用**会被误判(实际发生过: Painter 测试里
    # `for (const auto& v : p.geometry().vertices)` 被这条规则命中)。
    # 收紧后依然能抓到 "shaders/stroke.geometry"、'x.geom"'、Path("a.geometry")。
    (re.compile(r"""(?:^|[/\\"'\s])[\w\-.]+\.(?:geom|geometry)["'\s,;)\]}]"""),
     "几何着色器源文件后缀"),
]

SHADER_EXTS = {".vert", ".frag", ".comp", ".es", ".glsl"}
CODE_EXTS = SHADER_EXTS | {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl"}
OTHER_EXTS = {".txt", ".cmake", ".py", ".json"}
SKIP_DIRS = {"build", ".git", "third_party", "vendor", "_install", "_src", "out", ".vs"}


def iter_files(base):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            ext = os.path.splitext(name)[1].lower()
            if ext in CODE_EXTS or ext in OTHER_EXTS or name == "CMakeLists.txt":
                yield os.path.join(dirpath, name)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=None, help="MagicBlocks 根目录(默认取脚本上级的 MagicBlocks)")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(args.root or os.path.join(os.path.dirname(here), "MagicBlocks"))
    if not os.path.isdir(root):
        print("[G1] 目录不存在: %s" % root)
        return 2

    problems = []
    victims = []

    # 1) shaders/ 必须存在且非空
    shader_dir = os.path.join(root, "shaders")
    if not os.path.isdir(shader_dir):
        print("[G1] 环境错误: shaders/ 目录不存在: %s" % shader_dir)
        return 2
    else:
        shaders = [f for f in sorted(os.listdir(shader_dir))
                   if os.path.splitext(f)[1].lower() in SHADER_EXTS]
        if not shaders:
            problems.append("shaders/ 目录内没有任何 shader 文件(应为 .vert/.frag/.comp)")
        else:
            print("[G1]   shaders/ 发现 %d 个 shader: %s" % (len(shaders), ", ".join(shaders)))

    # 2) 逐文件扫描
    for sub in ("shaders", "src", "include", "tests"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for path in iter_files(base):
            victims.append(path)
        # 根目录下的 CMakeLists.txt 也要扫(glslc 命令行就写在那里)
    root_cmake = os.path.join(root, "CMakeLists.txt")
    if os.path.isfile(root_cmake):
        victims.append(root_cmake)
    for path in victims:
        if True:
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    lines = f.read().splitlines()
            except OSError as exc:  # 读不到即报错,不静默跳过
                print("[G1] 读取失败: %s (%s)" % (path, exc))
                return 2
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            for lineno, line in enumerate(lines, 1):
                for pat, why in FORBIDDEN_PATTERNS:
                    if pat.search(line):
                        problems.append("%s:%d  %s  ->  %s" % (rel, lineno, why, line.strip()))

    if problems:
        print("[G1] 违规 %d 处(禁止几何着色器):" % len(problems))
        for p in problems:
            # 违规行可能带 BOM 或 Windows 控制台是 GBK,直接 print 会 UnicodeEncodeError
            # 崩掉 —— 那就变成"发现违规但门控自己崩了",退出码语义完全乱掉。
            # 统一用 backslashreplace,保证任何字节都能打出来且退出码仍是 1。
            print("  " + p.encode("utf-8", "backslashreplace").decode("utf-8"))
        return 1
    print("[G1] OK 扫描 %d 个文件,无几何着色器" % len(victims))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
