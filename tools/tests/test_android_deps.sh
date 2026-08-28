#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ANDROID_DEPS="$ROOT/android-deps.sh"
[ -f "$ANDROID_DEPS" ] || { echo "FAIL: android-deps.sh 不存在"; exit 1; }

# 禁止 sudo
if grep -qE '\bsudo\b|\bapt install\b|\byum install\b|\bdnf install\b' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 出现 sudo/包管理器直装"; exit 1
fi
# 国内镜像优先(Adoptium/JDK + cmdline-tools 官方 URL 存在即可,镜像探测逻辑仿 win-deps.sh)
if ! grep -qE 'api\.adoptium\.net|mirrors\.huaweicloud\.com/openjdk' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 缺 JDK 下载源"; exit 1
fi
if ! grep -qE 'commandlinetools-(linux|windows|\$\{PLAT\})-latest\.zip' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 缺 cmdline-tools 下载 URL"; exit 1
fi
# 许可证自动接受
if ! grep -qE 'sdkmanager.*--licenses' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 缺 sdkmanager --licenses"; exit 1
fi
# env.sh 导出
if ! grep -qE 'export ANDROID_HOME=' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 未写 ANDROID_HOME 到 env.sh"; exit 1
fi
# 复用优先:先探测现成 SDK,找不到才下载
if ! grep -qE 'ANDROID_HOME|ANDROID_SDK_ROOT' "$ANDROID_DEPS"; then
  echo "FAIL: android-deps.sh 未优先探测已存在的 SDK"; exit 1
fi
echo "PASS android-deps.sh 无 sudo + 镜像 + 许可证 + env.sh 导出 + 复用优先"
