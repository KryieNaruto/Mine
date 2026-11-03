#!/usr/bin/env python3
"""把 glslc 产出的 SPIR-V 二进制内嵌成 C++ 头(沿用 EasyPainter/shaders/embed_shaders.py 范式)。

用法:
    python embed_shaders.py <输出头文件> <a.spv> <b.spv> ...

生成内容:
    namespace magicblocks::shaders {
      extern const uint32_t kFullscreenVert[]; extern const size_t kFullscreenVertSize;
      ...
    }
变量名由文件名推导:`fullscreen.vert.spv` → `kFullscreenVert`。
"""
import os
import sys


def cpp_ident(name: str) -> str:
    """fullscreen.vert.spv -> kFullscreenVert"""
    base = name
    for suffix in (".spv",):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
    parts = [p for p in base.replace("-", "_").split(".") if p]
    return "k" + "".join(p[:1].upper() + p[1:] for p in parts)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out_path = argv[0]
    spv_paths = argv[1:]

    lines = [
        "// 由 shaders/embed_shaders.py 自动生成,请勿手改。",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace magicblocks::shaders {",
        "",
    ]
    for path in spv_paths:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) % 4 != 0:
            print("SPIR-V 长度不是 4 的倍数: %s" % path)
            return 1
        ident = cpp_ident(os.path.basename(path))
        words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
        lines.append("// %s (%d bytes)" % (os.path.basename(path), len(data)))
        lines.append("alignas(4) inline const std::uint32_t %s[] = {" % ident)
        for i in range(0, len(words), 8):
            chunk = ", ".join("0x%08xu" % w for w in words[i:i + 8])
            lines.append("    " + chunk + ",")
        lines.append("};")
        lines.append("inline constexpr std::size_t %sSize = sizeof(%s);" % (ident, ident))
        lines.append("")

    lines.append("}  // namespace magicblocks::shaders")
    lines.append("")

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    old = None
    if os.path.isfile(out_path):
        with open(out_path, encoding="utf-8") as f:
            old = f.read()
    text = "\n".join(lines)
    if old == text:
        return 0
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
