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
# 注:cmdline-tools 只是最小 SDK 管理器本体,不含 platforms/android-34 与 build-tools;
# 这些由 AGP 首次构建时按 compileSdk 自动经 sdkmanager 下载(需联网 + 已接受许可证)。
# sdkmanager 可执行文件名因平台而异:Linux zip 只有无扩展名 shell 脚本 sdkmanager,
# Windows zip 只有 sdkmanager.bat(实测 commandlinetools-win-16111833 的 bin/ 全是 .bat,
# 无扩展名脚本根本不存在)——两者都要认,否则 Windows 上误判"缺 sdkmanager"。
resolve_sdkmanager() { # $1 = sdk root;设 SDKMANAGER(shell 脚本或 .bat);找不到返回非零
  local b="$1/cmdline-tools/latest/bin"
  SDKMANAGER=""
  if [ -x "$b/sdkmanager" ]; then SDKMANAGER="$b/sdkmanager"
  # Windows zip 只有 sdkmanager.bat(无扩展名脚本根本不存在);MSYS 视 .bat 为可执行,
  # 用 -f 而非 -x(Windows 下两者等价,Linux 上 -x 对 .bat 恒假、无法本地验证)
  elif [ -f "$b/sdkmanager.bat" ]; then SDKMANAGER="$b/sdkmanager.bat"; fi
  [ -n "$SDKMANAGER" ]
}

ANDROID_HOME=""
for cand in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$USER_DEPS/android-sdk" \
            "${LOCALAPPDATA:-}/Android/Sdk" "$HOME/Android/Sdk" "/opt/android-sdk"; do
  [ -n "$cand" ] || continue
  if resolve_sdkmanager "$cand"; then ANDROID_HOME="$cand"; break; fi
done
if [ -n "$ANDROID_HOME" ]; then
  info "② 复用已安装 Android SDK: $ANDROID_HOME"
else
  info "② 未探测到 Android SDK,下载 cmdline-tools 到 .user-deps/android-sdk …"
  ANDROID_HOME="$USER_DEPS/android-sdk"
  mkdir -p "$ANDROID_HOME/cmdline-tools"
  # cmdline-tools 命名里平台 token 是 linux/win(非 windows),单独映射;版本必须显式
  # 带出(官方与镜像都没有 -latest 别名,`-latest.zip` 实测 404)。版本锁 16111833。
  case "$PLAT" in windows) CT_PLAT=win ;; *) CT_PLAT=linux ;; esac
  CT_VER="16111833"
  # 国内镜像优先(腾讯云 AndroidSDK 仓库,实测 200),官方 dl.google.com 兜底。
  CT_MIRROR="https://mirrors.cloud.tencent.com/AndroidSDK/commandlinetools-${CT_PLAT}-${CT_VER}_latest.zip"
  CT_OFFICIAL="https://dl.google.com/android/repository/commandlinetools-${CT_PLAT}-${CT_VER}_latest.zip"
  CT_ARCHIVE="$USER_DEPS/cmdline-tools.zip"
  curl -fL --retry 3 -o "$CT_ARCHIVE" "$CT_MIRROR" \
    || curl -fL --retry 3 -o "$CT_ARCHIVE" "$CT_OFFICIAL" \
    || die "cmdline-tools 下载失败(镜像与官方 URL 均不可达;可手动下载同 URL 到 $CT_ARCHIVE 后重跑)"
  unpack_zip "$CT_ARCHIVE" "$ANDROID_HOME/cmdline-tools"
  # cmdline-tools zip 结构:老版本带 cmdline-tools/ 顶层包装目录(需改名 latest);个别版本
  # 可能直接把 bin/lib 摆在根。两种情况都归一到 latest/bin/sdkmanager。
  if [ -d "$ANDROID_HOME/cmdline-tools/cmdline-tools" ]; then
    mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
  elif [ -d "$ANDROID_HOME/cmdline-tools/bin" ] && [ ! -d "$ANDROID_HOME/cmdline-tools/latest" ]; then
    mkdir -p "$ANDROID_HOME/cmdline-tools/latest"
    mv "$ANDROID_HOME/cmdline-tools/bin" "$ANDROID_HOME/cmdline-tools/latest/" 2>/dev/null || true
    mv "$ANDROID_HOME/cmdline-tools/lib" "$ANDROID_HOME/cmdline-tools/latest/" 2>/dev/null || true
    mv "$ANDROID_HOME/cmdline-tools/source.properties" "$ANDROID_HOME/cmdline-tools/latest/" 2>/dev/null || true
    mv "$ANDROID_HOME/cmdline-tools/NOTICE.txt" "$ANDROID_HOME/cmdline-tools/latest/" 2>/dev/null || true
  fi
  rm -f "$CT_ARCHIVE"
  resolve_sdkmanager "$ANDROID_HOME" || die "cmdline-tools 解压后缺 sdkmanager(.bat)"
fi

# --- ③ 接受许可证(sdkmanager 需要 JAVA_HOME)---
if [ -n "${JAVA_HOME:-}" ]; then export JAVA_HOME; fi
# sdkmanager.bat 走 cmd.exe,只认 Windows 路径(MSYS 的参数转换不一定覆盖 --flag= 形式),显式 cygpath -m。
if [ "$PLAT" = windows ]; then
  SDK_ROOT_ARG="--sdk_root=$(cygpath -m "$ANDROID_HOME")"
else
  SDK_ROOT_ARG="--sdk_root=$ANDROID_HOME"
fi
# yes 在 sdkmanager 退出后必收 SIGPIPE,pipefail 下会误报;包一层 || true 只让 sdkmanager 的退出码决定成败。
{ yes || true; } | "$SDKMANAGER" --licenses "$SDK_ROOT_ARG" >/dev/null 2>&1 \
  || info "许可证接受失败(不影响其他项目)"

# --- ④ 写 env.sh(幂等:存在则保留既有行,追加/更新本脚本的导出)---
# Windows 下 ANDROID_HOME/JAVA_HOME 是 MSYS POSIX 路径(/c/...),原生 Windows Python
# (find_android_sdk 用 os.path.isdir 判存在)与 Android Studio(local.properties)都不认;
# 写进 env.sh 前用 cygpath -m 转成 Windows 原生形式(C:\...),cygpath 不可用则退回原路径。
if [ "$PLAT" = "windows" ]; then
  AH_ENV="$(cygpath -m "$ANDROID_HOME" 2>/dev/null || printf '%s' "$ANDROID_HOME")"
  JH_ENV="$(cygpath -m "$JAVA_HOME" 2>/dev/null || printf '%s' "$JAVA_HOME")"
else
  AH_ENV="$ANDROID_HOME"
  JH_ENV="$JAVA_HOME"
fi
if [ ! -f "$ENV_SH" ]; then
  printf '# Android 工具链环境(由 tools/android-deps.sh 生成)\n' > "$ENV_SH"
fi
_san() { grep -v '^# ' "$1" 2>/dev/null | grep -v '^$' || true; }
_san "$ENV_SH" | grep -qE '^export ANDROID_HOME=' \
  || printf 'export ANDROID_HOME="%s"\n' "$AH_ENV" >> "$ENV_SH"
if [ -n "${JAVA_HOME:-}" ]; then
  _san "$ENV_SH" | grep -qE '^export JAVA_HOME=' \
    || printf 'export JAVA_HOME="%s"\n' "$JH_ENV" >> "$ENV_SH"
fi
info "④ Android 工具链就绪: ANDROID_HOME=$ANDROID_HOME"
