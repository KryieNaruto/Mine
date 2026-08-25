#!/usr/bin/env bash
# 生成 golden 基准 PNG：空 store → 内置 fixture（中文标题/任务）。
# 用法: bash tools/gen-golden.sh [build_dir]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

source /home/qiansenwei/workspace/Mine/.user-deps/env.sh
BUILD_DIR="${1:-build}"

tmp_store="$(mktemp /tmp/stickynotes-golden-store-XXXXXX.json)"
trap 'rm -f "$tmp_store"' EXIT
echo '{"notes":[]}' > "$tmp_store"

QT_QPA_PLATFORM=offscreen "$BUILD_DIR/stickynotes-cli" \
  --render golden/stickynotes_golden.png "$tmp_store"

echo "golden written to golden/stickynotes_golden.png"
