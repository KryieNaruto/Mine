#!/usr/bin/env python3
"""门控 R1 —— 离屏是产品级出口(无窗口/无显示服务必须能出图)。

裁定背景(`docs/superpowers/specs/2026-11-03-magicblocks-r1-positioning-ruling.md`):
SDK 定位为"计算/几何库 + 离屏渲染器",上屏移交平台壳层。于是 SDK 的**对外交付证明**
就是"不依赖窗口句柄、不依赖显示服务,也能出一张正确的图"。

本门控检查三件事(静态,不跑 GPU):

  R1-1  公共头里不得出现平台窗口类型
        —— `include/magicblocks/` 下不得出现 `HWND`/`HINSTANCE`/`ANativeWindow`/
           `xcb_connection_t`/`CA::MetalLayer`/`MTKView`,窗口互通只允许经
           `device/types.h` 的 `NativeSurface{kind, handle}`(那个字段本来就是不透明指针)。

  R1-2  离屏出图路径不得建立任何 surface/swapchain
        —— `src/ui_cli/`、`src/cli/`、`src/l2_cli/` 与 `src/ui/presenter_offscreen.cpp`
           里不得出现 `create_swapchain` / `ISwapchain` / `vkCreate*SurfaceKHR` / `vkQueuePresentKHR`。
           离屏是"一等公民"的前提就是它**够不到**上屏。

  R1-3  CLI 必须存在离屏出图入口
        —— `src/ui_cli/main.cpp` 里有 `--output` 且**没有** `--mode`(离屏是默认路径,
           不需要切换);`src/ui_cli/` 下不得出现显示服务/平台窗口符号。

退出码:0 通过;1 有违规(逐条打印 file:line 与原因)。
"""
from __future__ import annotations

import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 工程根:SDK 代码在 MagicBlocks/ 下(本脚本在仓库根 tools/ 下)
PROJ = os.path.join(ROOT, "MagicBlocks")
if not os.path.isdir(os.path.join(PROJ, "src")):
    PROJ = ROOT

# R1-1:公共头禁止的平台窗口类型(窗口互通只能经 NativeSurface)
PUBLIC_FORBIDDEN = [
    "HWND", "HINSTANCE", "ANativeWindow", "xcb_connection_t",
    "CA::MetalLayer", "MTKView", "NSView", "UIView",
]

# R1-2:离屏路径禁止出现的上屏符号
OFFSCREEN_FORBIDDEN = [
    "create_swapchain", "ISwapchain", "vkCreateWin32SurfaceKHR",
    "vkCreateXcbSurfaceKHR", "vkCreateAndroidSurfaceKHR", "vkQueuePresentKHR",
]

# R1-3:CLI 里禁止出现的显示服务/平台窗口符号(离屏不需要它们)
CLI_FORBIDDEN = [
    "wayland", "DISPLAY_ENV", "GetDC", "HWND", "ANativeWindow", "xcb_",
]

OFFSCREEN_DIRS = ["src/cli", "src/l2_cli", "src/ui_cli"]
OFFSCREEN_EXTRA = ["src/ui/presenter_offscreen.cpp"]


def iter_files(rel_dirs):
    for rel in rel_dirs:
        base = os.path.join(PROJ, rel)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in filenames:
                if fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                    yield os.path.join(dirpath, fn)


def scan(path, needles):
    hits = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            for n in needles:
                if n in line:
                    hits.append((lineno, n, stripped[:110]))
    return hits


def main() -> int:
    failures = []

    # ---- R1-1
    pub = os.path.join(PROJ, "include", "magicblocks")
    for dirpath, _d, files in os.walk(pub):
        for fn in files:
            if not fn.endswith((".h", ".hpp")):
                continue
            path = os.path.join(dirpath, fn)
            for lineno, needle, text in scan(path, PUBLIC_FORBIDDEN):
                failures.append(
                    f"R1-1 {os.path.relpath(path, ROOT)}:{lineno} 公共头出现平台窗口类型 "
                    f"'{needle}' —— 窗口互通必须经 NativeSurface(不透明 handle): {text}"
                )

    # ---- R1-2
    targets = list(iter_files(OFFSCREEN_DIRS))
    for rel in OFFSCREEN_EXTRA:
        p = os.path.join(PROJ, rel)
        if os.path.isfile(p):
            targets.append(p)
    if not targets:
        failures.append("R1-2 未找到任何离屏路径源文件(src/cli, src/l2_cli, src/ui_cli)")
    for path in targets:
        for lineno, needle, text in scan(path, OFFSCREEN_FORBIDDEN):
            failures.append(
                f"R1-2 {os.path.relpath(path, ROOT)}:{lineno} 离屏路径出现上屏符号 "
                f"'{needle}' —— 离屏必须够不到 swapchain/present: {text}"
            )

    # ---- R1-3
    cli = os.path.join(PROJ, "src", "ui_cli", "main.cpp")
    if not os.path.isfile(cli):
        failures.append("R1-3 找不到 src/ui_cli/main.cpp —— 离屏出图入口是产品级出口,必须存在")
    else:
        with open(cli, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "--output" not in text:
            failures.append(
                f"R1-3 {os.path.relpath(cli, ROOT)} 缺少 '--output' —— "
                "验收命令 `magicblocks-ui-cli render --output x.png` 不可用"
            )
        if "--mode" in text:
            failures.append(
                f"R1-3 {os.path.relpath(cli, ROOT)} 出现了 '--mode' —— "
                "离屏是默认路径(R1 裁定),不应再需要模式开关"
            )
        for lineno, needle, snippet in scan(cli, CLI_FORBIDDEN):
            failures.append(
                f"R1-3 {os.path.relpath(cli, ROOT)}:{lineno} CLI 引用了显示服务符号 "
                f"'{needle}' —— 离屏出图不得依赖窗口系统: {snippet}"
            )

    if failures:
        print(f"R1 门控失败:{len(failures)} 处违规")
        for f in failures:
            print("  " + f)
        return 1
    print("R1 门控通过:离屏出口无窗口依赖、无 swapchain 依赖、CLI 入口齐备")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
