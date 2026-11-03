#!/usr/bin/env bash
# Linux / macOS:PATH 里没有 SwiftShader 时用 lavapipe(mesa 的软件 Vulkan)。
# 用法:  source tools/vulkan_env.sh

set -u

# lavapipe 来自 mesa-vulkan-drivers,常见位置:
for f in /usr/share/vulkan/icd.d/lvp_icd.*.json \
         /usr/share/vulkan/icd.d/lavapipe_icd.*.json; do
    if [ -e "$f" ]; then
        export VK_DRIVER_FILES="$f"
        export VK_ICD_FILENAMES="$f"
        echo "VK_DRIVER_FILES = $f"
        echo "验证: MagicBlocks/build/magicblocks-l0-cli device"
        return 0 2>/dev/null || exit 0
    fi
done

echo "未找到 lavapipe ICD。请安装:" >&2
echo "  Debian/Ubuntu: sudo apt install mesa-vulkan-drivers" >&2
echo "  Fedora:        sudo dnf install mesa-vulkan-drivers" >&2
return 1 2>/dev/null || exit 1
