#!/usr/bin/env bash
# Android 工具链(免提权,全部落盘用户级 .user-deps):JDK 17 + Android SDK cmdline-tools。
# 先探测现成(ANDROID_HOME/ANDROID_SDK_ROOT → 常见路径 → .user-deps/android-sdk),
# 找不到才下载;国内镜像优先、官方兜底。产物写 .user-deps/env.sh,供 setup-env.sh
# source 后传给 gen-projects.py(CMake/Android 构建需要 ANDROID_HOME/JAVA_HOME)。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
USER_DEPS="${USER_DEPS:-$ROOT/.user-deps}"
ENV_SH="$USER_DEPS/env.sh"
mkdir -p "$USER_DEPS"

info() { printf '[INFO] %s\n' "$*"; }
die()  { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

# zip 解压:7z 优先(Git Bash 常无 unzip,win-deps.sh 装了 p7zip 提供 7z),unzip 兜底。
unpack_zip() {
  if command -v 7z >/dev/null 2>&1; then 7z x -y "$1" -o"$2"
  elif command -v unzip >/dev/null 2>&1; then unzip -qo "$1" -d "$2"
  else die "无 7z/unzip 可解压 zip(可装 p7zip 后重跑)"; fi
}

case "$(uname -s)" in
  MINGW*|MSYS*) PLAT=windows ;;
  *) PLAT=linux ;;
esac

# --- ① JDK 17(AGP 8.5/Gradle 8.7 需要):探测现成,缺失下载 Temurin 17 到 .user-deps ---
JAVA_HOME=""
if command -v java >/dev/null 2>&1; then
  ver="$(java -version 2>&1 | head -1)"
  if printf '%s' "$ver" | grep -qE '"(1[7-9]|[2-9][0-9])\.'; then
    JAVA_HOME="$(dirname "$(dirname "$(command -v java)")")"   # 近似;够 gradle 用即可
    info "① 复用系统 JDK: $ver"
  fi
fi
if [ -z "$JAVA_HOME" ] && [ -x "$USER_DEPS/jdk17/bin/java" ]; then
  JAVA_HOME="$USER_DEPS/jdk17"
  info "① 复用 .user-deps 已装 JDK17"
fi
if [ -z "$JAVA_HOME" ]; then
  info "① 未探测到 JDK 17+,下载 Temurin 17 到 .user-deps/jdk17 …"
  JDK_DIR="$USER_DEPS/jdk17"
  mkdir -p "$JDK_DIR"
  # 下载源:华为云 openjdk 17 镜像优先,Adoptium API(官方 latest)兜底
  case "$PLAT" in
    linux)   JDK_MIRROR="https://mirrors.huaweicloud.com/openjdk/17/openjdk-17_linux-x64_bin.tar.gz"
             JDK_URL="https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse"
             JDK_ARCHIVE="$USER_DEPS/jdk17.tar.gz" ;;
    windows) JDK_MIRROR="https://mirrors.huaweicloud.com/openjdk/17/openjdk-17_windows-x64_bin.zip"
             JDK_URL="https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse"
             JDK_ARCHIVE="$USER_DEPS/jdk17.zip" ;;
  esac
  curl -fL --retry 3 -o "$JDK_ARCHIVE" "$JDK_MIRROR" \
    || curl -fL --retry 3 -o "$JDK_ARCHIVE" "$JDK_URL" \
    || die "JDK 下载失败(可手动装 JDK17 后重跑;或把 JDK 解压到 .user-deps/jdk17)"
  case "$PLAT" in
    linux)
      tar -xzf "$JDK_ARCHIVE" -C "$JDK_DIR" --strip-components=1 ;;
    windows)
      unpack_zip "$JDK_ARCHIVE" "$JDK_DIR"
      # zip 内是 <jdk-17.0.x>/ 一层;版本化目录兜底上移,使 bin/java 落在 $JDK_DIR/bin/java
      [ -d "$JDK_DIR/jdk-17" ] && mv "$JDK_DIR/jdk-17"/* "$JDK_DIR" || true
      _top="$(find "$JDK_DIR" -maxdepth 1 -mindepth 1 -type d -name 'jdk-*' | head -n1 || true)"
      if [ -n "$_top" ] && [ -d "$_top/bin" ]; then
        mv "$_top"/* "$JDK_DIR" 2>/dev/null || true
        rmdir "$_top" 2>/dev/null || true
      fi
      ;;
  esac
  rm -f "$JDK_ARCHIVE"
  JAVA_HOME="$JDK_DIR"
  [ -x "$JAVA_HOME/bin/java" ] || die "JDK 解压后找不到 java"
fi

# --- ② Android SDK cmdline-tools:探测现成,缺失下载 ---
ANDROID_HOME=""
for cand in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$USER_DEPS/android-sdk" \
            "${LOCALAPPDATA:-}/Android/Sdk" "$HOME/Android/Sdk" "/opt/android-sdk"; do
  [ -n "$cand" ] && [ -f "$cand/cmdline-tools/latest/bin/sdkmanager" ] && { ANDROID_HOME="$cand"; break; }
done
if [ -n "$ANDROID_HOME" ]; then
  info "② 复用已安装 Android SDK: $ANDROID_HOME"
else
  info "② 未探测到 Android SDK,下载 cmdline-tools 到 .user-deps/android-sdk …"
  ANDROID_HOME="$USER_DEPS/android-sdk"
  mkdir -p "$ANDROID_HOME/cmdline-tools"
  CT_URL="https://dl.google.com/android/repository/commandlinetools-${PLAT}-latest.zip"
  CT_ARCHIVE="$USER_DEPS/cmdline-tools.zip"
  curl -fL --retry 3 -o "$CT_ARCHIVE" "$CT_URL" \
    || die "cmdline-tools 下载失败(官方 URL 不可达;可手动下载同 URL 到 $CT_ARCHIVE 后重跑)"
  unpack_zip "$CT_ARCHIVE" "$ANDROID_HOME/cmdline-tools"
  [ -d "$ANDROID_HOME/cmdline-tools/cmdline-tools" ] \
    && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
  rm -f "$CT_ARCHIVE"
  [ -x "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ] || die "cmdline-tools 解压后缺 sdkmanager"
fi

# --- ③ 接受许可证(sdkmanager 需要 JAVA_HOME)---
if [ -n "${JAVA_HOME:-}" ]; then export JAVA_HOME; fi
# yes 在 sdkmanager 退出后必收 SIGPIPE,pipefail 下会误报;包一层 || true 只让 sdkmanager 的退出码决定成败。
{ yes || true; } | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --licenses --sdk_root="$ANDROID_HOME" >/dev/null 2>&1 \
  || info "许可证接受失败(不影响其他项目)"

# --- ④ 写 env.sh(幂等:存在则保留既有行,追加/更新本脚本的导出)---
if [ ! -f "$ENV_SH" ]; then
  printf '# Android 工具链环境(由 tools/android-deps.sh 生成)\n' > "$ENV_SH"
fi
_san() { grep -v '^# ' "$1" 2>/dev/null | grep -v '^$' || true; }
_san "$ENV_SH" | grep -qE '^export ANDROID_HOME=' \
  || printf 'export ANDROID_HOME="%s"\n' "$ANDROID_HOME" >> "$ENV_SH"
if [ -n "${JAVA_HOME:-}" ]; then
  _san "$ENV_SH" | grep -qE '^export JAVA_HOME=' \
    || printf 'export JAVA_HOME="%s"\n' "$JAVA_HOME" >> "$ENV_SH"
fi
info "④ Android 工具链就绪: ANDROID_HOME=$ANDROID_HOME"
