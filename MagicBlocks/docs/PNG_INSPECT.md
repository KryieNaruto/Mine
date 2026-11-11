# PNG 视觉验证:`png_inspect`(机器可读,纯标准库)。输出 `key = value` 标量行;断言不过退出码 1(用法/解码错 2,全过 0)。`I` = `python MagicBlocks/tools/png_inspect.py`。**本工具在 `MagicBlocks/tools/`,与仓库级门控脚本 `Mine/tools/`(`check_ui_no_readback.py` 等)不是同一目录。**
| 尺寸/通道/位深/alpha(任何模式都先打) | `I a.png --info` |
| 主色 top-N 与占比(找"一块纯色"/"面板没画出来") | `I a.png --colors 8` |
| 区域:非背景占比/颜色种数/亮度跨度/过渡色像素(可重复) | `I a.png --region 0,0,320,60 --region 0,260,320,380` |
| 边沿平滑度(判 MSAA):`--band` 把统计范围**限定到场景区**、`--fg` 给前景 | `I a.png --edge-metric --band 0,260,2160,1180 --fg 133,112,28` |
| 两图逐像素对比 + 差异图(相同处=#202020),尺寸不等报错 | `I a.png --diff b.png --diff-out d.png` |
| 断言任一标量键(`--list-metrics` 列出);`--quiet` 只留结论、`--quiet --json -` 出纯 JSON;`--bg` 默认左上角像素、`--bg-tol` 控容差 | `I a.png --diff b.png --assert diff_pixels==0` |

**`--allow-empty-sample` 在 ctest / 任何门控里禁止使用**(它只给人手工排查用;自动化验收里出现它 = 把"指标无效"当成"通过")。自测里有一条 grep 用例守着这条:该开关只允许出现在 `png_inspect.py` 本体、`docs/`、以及 `--allow-empty-sample` 自身用例的放行检查里。
**面板/UI 存在性不要只用"这块区域有颜色变化"证明** —— 要断言面板背景色、条目位置与数量;`--region` 区域断言必须满宽,判读前确认前景像素非零。

**`--band` 只用于把统计范围限定到场景区(如 y 从 HUD 下沿到画面底部),宽度必须取满图宽;禁止用比图窄的宽度**,否则采样区里前景像素会被剪光,`transition_pixels=0` 是"没数据"而不是"没变化"。**判读任何 edge 指标前先看 `edge_foreground_pixels > 0`**:为 0 时工具默认**退出码 1**并报"采样区内无前景,指标无效"(强行出数须显式加 `--allow-empty-sample`,且 `edge_sample_valid` 仍为 0)。自测 `python -m unittest discover -s MagicBlocks/tools/tests -p "test_png_inspect.py"`。

**范例(2160×1440,`build/tmp/m1`=1× / `m4`=4× / `m8`=8× MSAA,均有效口径)**:
全图像素变化 `diff m1→m4`:22188 像素 = 0.7133%,包围盒 x[77,1941] y[48,229](宽占图 86.3%、高 12.6%、面积 10.9%);`m4→m8`:374 像素 = 0.0120%,包围盒 x[190,1891] y[76,227]。
全图 edge:`m1` fg=1247 / transition=944684 / neighbor=0;`m4` fg=1235 / transition=944936(**+252,≈0.03%**);`m8` fg=1249 / transition=944922。
场景区(全宽 band `0,260,2160,1180` + `--fg 133,112,28`):三张均为 fg=792990 / transition=3002。
**结论表述**:MSAA 在这条路径上产生了**可测但极小**的边际变化(全图 +252 过渡像素,0.03%),差异集中在顶部区域(y≤229);只能说"影响范围受限",**不能**声称"场景区没有变化"。
