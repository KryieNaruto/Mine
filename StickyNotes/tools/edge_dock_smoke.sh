#!/usr/bin/env bash
# 窗口化冒烟（不进测试门）：Xvfb + XTEST 验证边缘收齐/拉出。
# 依赖: xdotool（若可用）；跑完 kill，截图落 /tmp/edge_dock_smoke.png 供人工查看。
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

source /home/qiansenwei/workspace/Mine/.user-deps/env.sh

XVFB="$(command -v Xvfb || echo /home/qiansenwei/workspace/Mine/.user-deps/usr/usr/bin/Xvfb)"
DISPLAY=:99

echo "==> 起 Xvfb :99"
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99 2>/dev/null || true
"$XVFB" :99 -screen 0 1280x720x24 >/tmp/edge_dock_smoke_xvfb.log 2>&1 &
XVFB_PID=$!
trap 'kill $XVFB_PID 2>/dev/null || true' EXIT
sleep 1.5

echo "==> 启动 stickynotes (xcb)"
DISPLAY=:99 QT_QPA_PLATFORM=xcb ./build/release/stickynotes &
APP_PID=$!
sleep 2

if command -v xdotool >/dev/null 2>&1; then
  echo "==> 鼠标移到窗口近左缘"
  # 找到便签窗口并移到其左缘偏内
  WIN_ID="$(DISPLAY=:99 xdotool search --name '.*' 2>/dev/null | head -1 || true)"
  if [ -n "$WIN_ID" ]; then
    eval "$(DISPLAY=:99 xdotool getwindowgeometry --shell "$WIN_ID" 2>/dev/null)"
    [ -n "${X:-}" ] && DISPLAY=:99 xdotool mousemove $((X+5)) $((Y+160))
  else
    DISPLAY=:99 xdotool mousemove 5 300
  fi
  sleep 1
  DISPLAY=:99 xdotool mousemove 5 300
  sleep 1
fi

if command -v import >/dev/null 2>&1; then
  DISPLAY=:99 import -window root /tmp/edge_dock_smoke.png 2>/dev/null && echo "截图: /tmp/edge_dock_smoke.png"
else
  echo "无 ImageMagick import，跳过截图（人工验收仅需观察收齐行为）"
fi

kill "$APP_PID" 2>/dev/null || true
wait "$APP_PID" 2>/dev/null || true
echo "==> 冒烟完成（人工查看 /tmp/edge_dock_smoke.png）"
