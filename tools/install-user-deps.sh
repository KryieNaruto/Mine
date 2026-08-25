#!/usr/bin/env bash
# 无 sudo 用户级部署系统依赖(Vulkan SDK / X11 开发头 / lavapipe 软件光栅 / Xvfb 虚拟 display)。
# 全部落盘到 $MINE_ROOT/.user-deps/,并生成 .user-deps/env.sh(构建/运行前 source)。
# 适用:服务器无 root,无法 apt-get install 的场景。
set -euo pipefail

# --- 路径解析 ---------------------------------------------------------------
MINE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_DEPS="$MINE_ROOT/.user-deps"
USBIN="$USER_DEPS/usr"                       # dpkg -x 落点: .user-deps/usr/...
UARCH="$(uname -m)"
case "$UARCH" in
  x86_64) MULTIARCH="x86_64-linux-gnu"; DEBARCH="amd64" ;;
  aarch64) MULTIARCH="aarch64-linux-gnu"; DEBARCH="arm64" ;;
  *) MULTIARCH="$(gcc -print-multiarch 2>/dev/null || echo 'x86_64-linux-gnu')"; DEBARCH="${DEBARCH:-amd64}" ;;
esac

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
err()  { printf '[ERROR] %s\n' "$*" >&2; }
die()  { err "$*"; exit 1; }
has()  { command -v "$1" >/dev/null 2>&1; }

# --- 前置工具检查 -----------------------------------------------------------
for c in curl tar dpkg-deb dpkg sed; do
  has "$c" || die "缺少命令: $c"
done
has apt-get || warn "未检测到 apt-get;.deb 将走远程 archive.ubuntu.com 下载(需能出网)"
if ! has gcc && ! has cc && ! has g++; then
  die "缺少 C/C++ 编译器(gcc/cc/g++),探针无法编译"
fi

# apt 镜像基址(供 curl 兜底用)
APT_MIRROR=""
for src in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  [ -f "$src" ] || continue
  APT_MIRROR="$(grep -h -E '^(deb|Types:.*deb|URIs:)' "$src" 2>/dev/null \
    | grep -oE 'https?://[^ ]+' | grep -vE 'security|codeload|github' | head -1 || true)"
  [ -n "$APT_MIRROR" ] && break
done
APT_MIRROR="${APT_MIRROR:-http://archive.ubuntu.com/ubuntu}"

# --- .deb 下载与解包 --------------------------------------------------------
DEB_CACHE="$USER_DEPS/.deb-cache"
mkdir -p "$DEB_CACHE"

# deb_filename <pkg> : 从本地 apt lists 解析 Filename;失败则远程拉 Packages.gz。
deb_filename() {
  local pkg="$1" arch="$MULTIARCH" codename="" line f
  codename="$(apt-cache policy "$pkg" 2>/dev/null | grep -oE '/[a-z]+(-updates|-security|-backports)?/' | head -1 | tr -d '/' || true)"
  [ -z "$codename" ] && codename="$(grep -ohE 'noble|jammy|focal|bookworm|bullseye|trixie' /etc/os-release /etc/lsb-release /etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null | head -1 || echo noble)"
  # 本地 apt lists 优先(Architecture 字段用 deb 架构名如 amd64)
  line="$(grep -A 30 "^Package: ${pkg}\$" /var/lib/apt/lists/*Packages* 2>/dev/null \
          | awk -v a="$DEBARCH" '$1=="Architecture:" && $2==a {arch_ok=1} /^Filename:/ && arch_ok {print $2; exit}')"
  if [ -n "$line" ]; then
    printf '%s' "$line"; return 0
  fi
  # 远程兜底:拉 dists/<codename>/main/binary-<arch>/Packages.gz 解析
  local gz="$DEB_CACHE/Packages-$codename-$arch.gz"
  curl -fsSL -o "$gz" "$APT_MIRROR/dists/$codename/main/binary-$arch/Packages.gz" \
    || curl -fsSL -o "$gz" "http://archive.ubuntu.com/ubuntu/dists/$codename/main/binary-$arch/Packages.gz" \
    || return 1
  f="$(zcat "$gz" 2>/dev/null | awk -v p="$pkg" '
      $1=="Package:" && $2==p {pkg=1; next}
      pkg && /^Architecture:/ && $2!="'"$arch"'" {pkg=0}
      pkg && /^Filename:/ {print $2; exit}')"
  printf '%s' "$f"
}

# dl_deb <pkg> : 下载 .deb 到 $DEB_CACHE(apt 或 curl 兜底)。回显本地路径。
dl_deb() {
  local pkg="$1" url file
  if has apt-get && apt-get download --print-uris "$pkg" >/dev/null 2>&1; then
    (cd "$DEB_CACHE" && apt-get download "$pkg" >/dev/null 2>&1) || true
    file="$(ls "$DEB_CACHE"/${pkg}_*.deb 2>/dev/null | head -1 || true)"
    [ -n "$file" ] && { printf '%s\n' "$file"; return 0; }
  fi
  # curl 兜底
  url="$(deb_filename "$pkg")"
  [ -n "$url" ] || die "无法解析包 '$pkg' 的下载地址(apt lists 缺失且远程解析失败)"
  file="$DEB_CACHE/$(basename "$url")"
  curl -fsSL -o "$file" "$APT_MIRROR/$url" || curl -fsSL -o "$file" "http://archive.ubuntu.com/ubuntu/$url" \
    || die "下载失败: $pkg"
  printf '%s\n' "$file"
}

# x_deb <pkg> : 下载并 dpkg -x 到 $USER_DEPS。
x_deb() {
  local pkg="$1" f
  f="$(dl_deb "$pkg")"
  dpkg-deb -x "$f" "$USBIN"
  info "解包 $pkg -> $USBIN"
}

# dep_names <pkg> : dpkg-deb 解析 Depends,输出纯包名(去版本/替代项)。
dep_names() {
  local f="$1" deps
  deps="$(dpkg-deb -f "$f" Depends 2>/dev/null || true)"
  [ -n "$deps" ] || return 0
  printf '%s' "$deps" \
    | tr ',' '\n' \
    | sed -E 's/^[[:space:]]*//; s/[[:space:]]*$//' \
    | sed -E 's/^([^ |]+).*/\1/' \
    | sed -E 's/[[:space:]]*\(.*$//' \
    | sed -E 's/^([^:]+)(:any|:native)$/\1/' \
    | grep -vE '^$' || true
}

# installed <pkg> : 系统是否已安装该包。
installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'; }

# provided_installed <virtual> : 是否有已安装包 Provides 该虚拟包。
# 例: libqt6core6t64 已装且 Provides "qt6-base-abi (= 6.4.2)",则依赖 qt6-base-abi 已满足,
# 不应尝试下载不存在的 .deb(虚拟包无实体包可下)。
provided_installed() {
  local dep="$1"
  awk -v d="$dep" '
    /^Status: install ok installed$/ { inst=1; next }
    /^Status:/ { inst=0; next }
    /^$/ { inst=0; next }
    inst && /^Provides:/ {
      line=$0; sub(/^Provides:[[:space:]]*/, "", line)
      n=split(line, items, ",")
      for (i=1; i<=n; i++) {
        it=items[i]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", it)
        name=it; sub(/[[:space:]]*\(.*$/, "", name)
        if (name==d) { print 1; exit }
      }
    }
  ' /var/lib/dpkg/status 2>/dev/null | grep -q 1
}

# fetch_runtime_deps <pkg> <maxdepth> : 递归下载+解包运行期 Depends(未系统安装的)。
fetch_runtime_deps() {
  local pkg="$1" depth="${2:-3}" f
  f="$(dl_deb "$pkg")"
  for dep in $(dep_names "$f"); do
    [ "$depth" -le 0 ] && break
    if installed "$dep" || provided_installed "$dep"; then continue; fi
    info "  → 运行期依赖 $dep"
    x_deb "$dep"
    fetch_runtime_deps "$dep" $((depth-1))
  done
}

mkdir -p "$USER_DEPS"

# --- ① Vulkan SDK(headers + glslc + libs) -----------------------------------
SDK_URL="https://sdk.lunarg.com/sdk/download/latest/linux/vulkan-sdk-latest.tar.xz"
if [ ! -d "$USER_DEPS/vulkan-sdk" ]; then
  info "① 下载 Vulkan SDK ($SDK_URL)"
  curl -fL --retry 3 -o "$DEB_CACHE/vulkan-sdk-latest.tar.xz" "$SDK_URL"
  mkdir -p "$USER_DEPS/vulkan-sdk"
  tar -xJf "$DEB_CACHE/vulkan-sdk-latest.tar.xz" -C "$USER_DEPS/vulkan-sdk" --strip-components=0
  info "① Vulkan SDK 解压完成"
else
  info "① Vulkan SDK 已存在,跳过"
fi
SDK_X64="$(find "$USER_DEPS/vulkan-sdk" -maxdepth 3 -type d -name x86_64 | head -1 || true)"
[ -n "$SDK_X64" ] || SDK_X64="$(find "$USER_DEPS/vulkan-sdk" -maxdepth 4 -type d -name "$MULTIARCH" | head -1 || true)"
[ -n "$SDK_X64" ] || die "未在 .user-deps/vulkan-sdk 下找到 SDK 架构目录(x86_64/$MULTIARCH)"
info "① SDK 架构目录: $SDK_X64"
[ -f "$SDK_X64/include/vulkan/vulkan.h" ] || die "SDK 缺少 include/vulkan/vulkan.h"
[ -x "$SDK_X64/bin/glslc" ] || warn "SDK 缺少 glslc: $SDK_X64/bin/glslc"

# --- ② X11 开发头 + 运行期库(含传递依赖,GLFW X11 后端所需) --------------------
# -dev 包只带 .so 符号链接;运行期 .so.6 在独立运行包,必须一并部署,否则符号悬空→退静态 .a
X11_DEVS="libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
          libxext-dev libxcb1-dev libx11-xcb-dev x11proto-dev libxau-dev \
          libxrender-dev libxfixes-dev"
X11_RUNTIMES="libx11-6 libxrandr2 libxinerama1 libxcursor1 libxi6 libxext6 libxcb1 libxau6 libx11-xcb1 libxrender1 libxfixes3"
# 注:EasyPainter 为纯 Vulkan 应用,glfw3.h 用 #define GLFW_INCLUDE_VULKAN 引 vulkan.h,不需要 GL/gl.h
if [ -f "$USBIN/.x11-deployed" ]; then
  info "② X11 已部署(标记文件),跳过"
else
  for pkg in $X11_DEVS $X11_RUNTIMES; do
    x_deb "$pkg"
  done
  touch "$USBIN/.x11-deployed"
fi
[ -f "$USBIN/usr/include/X11/Xlib.h" ] || die "X11 头部署失败: $USBIN/usr/include/X11/Xlib.h 不存在"
[ -f "$USBIN/usr/include/X11/extensions/Xrandr.h" ] || die "X11 扩展头部署失败: Xrandr.h 不存在(GLFW 需要)"
[ -f "$USBIN/usr/lib/$MULTIARCH/libX11.so.6" ] || die "X11 运行期库部署失败: libX11.so.6 不存在"

# --- ③ 重写 .pc 绝对路径 ----------------------------------------------------
info "③ 重写 .pc 前缀 -> $USBIN"
find "$USBIN" -name '*.pc' -exec sed -i \
  -e "s|^prefix=.*|prefix=$USBIN/usr|" \
  -e "s|^exec_prefix=.*|exec_prefix=$USBIN/usr|" \
  -e "s|^includedir=.*|includedir=$USBIN/usr/include|" \
  -e "s|^libdir=.*|libdir=$USBIN/usr/lib/$MULTIARCH|" \
  -e "s|^pkgconfigdir=.*|pkgconfigdir=$USBIN/usr/lib/$MULTIARCH/pkgconfig|" \
  {} + 2>/dev/null || true
info "③ .pc 重写完成: $(find "$USBIN" -name '*.pc' | wc -l) 个"

# --- ④ Vulkan loader 链接符号 + lavapipe(无 GPU 软件光栅) --------------------
VK_DRIVER_FILES=""
# libvulkan-dev 提供 libvulkan.so 链接符号(探针与项目 find_package(Vulkan) 均需)。
# libvulkan1 是运行期 loader(提供 libvulkan.so.1),-dev 符号链依赖之,须一并强制部署,
# 否则符号链悬空导致 find_library 找不到库。
if [ ! -f "$USBIN/.vulkan-deployed" ]; then
  info "④ 部署 libvulkan-dev + libvulkan1(loader 符号链)"
  x_deb "libvulkan-dev"
  x_deb "libvulkan1"
  touch "$USBIN/.vulkan-deployed"
fi
# 系统已有 lavapipe 则直接使用(含 lvp_icd.json)
if [ -f /usr/share/vulkan/icd.d/lvp_icd.json ] && [ -f /usr/lib/$MULTIARCH/libvulkan_lvp.so ]; then
  info "④ 使用系统已装 lavapipe(lvp_icd.json)"
  VK_DRIVER_FILES="/usr/share/vulkan/icd.d/lvp_icd.json"
elif [ -f /usr/share/vulkan/icd.d/lvp_icd.x86_64.json ]; then
  info "④ 使用系统已装 lavapipe(lvp_icd.x86_64.json)"
  VK_DRIVER_FILES="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
else
  info "④ 部署 lavapipe(mesa-vulkan-drivers / libvulkan-lavapipe)"
  LVP_PKG=""
  apt-get download --print-uris mesa-vulkan-drivers >/dev/null 2>&1 && LVP_PKG="mesa-vulkan-drivers"
  [ -z "$LVP_PKG" ] && apt-get download --print-uris libvulkan-lavapipe >/dev/null 2>&1 && LVP_PKG="libvulkan-lavapipe"
  [ -n "$LVP_PKG" ] || die "未找到 lavapipe 包(mesa-vulkan-drivers / libvulkan-lavapipe)"
  x_deb "$LVP_PKG"
  fetch_runtime_deps "$LVP_PKG" 3
  # sed 改写 ICD json 的 library_path 为实际路径
  local_icd=""
  for j in "$USBIN"/usr/share/vulkan/icd.d/lvp_icd*.json; do
    [ -f "$j" ] || continue
    local_icd="$j"
    lib="$(grep -oE '"library_path"[[:space:]]*:[[:space:]]*"[^"]+"' "$j" | sed -E 's/.*"([^"]+)".*/\1/')"
    # 若 lib 是绝对 /usr 路径 → 改写为 $USBIN/usr;相对名 → 拼接绝对路径
    if [[ "$lib" == /* ]]; then
      sed -i "s|$lib|$USBIN$lib|g" "$j"
    else
      sed -i "s|\"$lib\"|\"$USBIN/usr/lib/$MULTIARCH/$lib\"|g" "$j"
    fi
    break
  done
  [ -n "$local_icd" ] || warn "lavapipe ICD json 未找到"
  VK_DRIVER_FILES="${local_icd:-}"
fi

# --- ⑤ Xvfb(虚拟 X display) -------------------------------------------------
XVFB_BIN=""
if [ -x "$USBIN/usr/bin/Xvfb" ]; then
  XVFB_BIN="$USBIN/usr/bin/Xvfb"
elif has Xvfb; then
  XVFB_BIN="$(command -v Xvfb)"
elif has xvfb-run; then
  info "⑤ 系统已有 xvfb-run,使用之"
else
  info "⑤ 部署 Xvfb"
  x_deb "xvfb"
  fetch_runtime_deps "xvfb" 3
  [ -x "$USBIN/usr/bin/Xvfb" ] || die "Xvfb 部署失败"
  XVFB_BIN="$USBIN/usr/bin/Xvfb"
fi
info "⑤ Xvfb: ${XVFB_BIN:-xvfb-run}"

# --- ⑥ Qt6 dev: 头文件 / moc·uic·rcc / Qt6 cmake 配置 / QtTest 运行期 ----------
if [ -f "$USBIN/.qt6-deployed" ]; then
  info "⑥ Qt6 已部署(标记文件),跳过"
else
  info "⑥ Qt6: 部署 qt6-base-dev + qt6-base-dev-tools + libqt6test6t64"
  QT_DEVS="qt6-base-dev qt6-base-dev-tools libqt6test6t64"
  for p in $QT_DEVS; do
    if installed "$p"; then info "Qt 包 $p 已系统安装，跳过"; continue; fi
    x_deb "$p" || die "Qt 包 $p 下载/解包失败"
    fetch_runtime_deps "$p" 3
  done
  touch "$USBIN/.qt6-deployed"
fi

# dpkg -x 后改写 Qt cmake 配置内的 /usr 绝对路径 → 用户级前缀(与 .pc 改写同款)。
# 系统未装 dev 头,若 Qt6Config.cmake 命中但内部 /usr 路径未改写 → 编译期 include 失败。
find "$USBIN/usr/lib/$MULTIARCH/cmake" -type f \( -name '*.cmake' -o -name '*Config*.cmake' \) 2>/dev/null \
  | xargs sed -i "s|/usr/include|$USBIN/usr/include|g; s|/usr/lib|$USBIN/usr/lib|g; s|/usr/share|$USBIN/usr/share|g" 2>/dev/null || true
find "$USBIN/usr/lib/qt6" -type f \( -name '*.cmake' -o -name '*.pri' \) 2>/dev/null \
  | xargs sed -i "s|/usr/include|$USBIN/usr/include|g; s|/usr/lib|$USBIN/usr/lib|g; s|/usr/share|$USBIN/usr/share|g" 2>/dev/null || true
# 运行期 .so 由系统包提供(如 libqt6core6t64, fetch_runtime_deps 因已装而跳过);
# dev 包只带 .so 符号链,改写后的 cmake targets 引用 $USBIN/.../libQt6Core.so.6.4.2 落空。
# 补齐: 将系统 Qt6 运行期库软链进用户级 lib 目录(软链到系统真实文件,运行期 LD_LIBRARY_PATH 已含用户级)。
for _qso in /usr/lib/$MULTIARCH/libQt6*.so.6; do
  [ -e "$_qso" ] || continue
  _base="$(basename "$_qso")"
  [ -e "$USBIN/usr/lib/$MULTIARCH/$_base" ] || ln -sfn "$_qso" "$USBIN/usr/lib/$MULTIARCH/$_base"
  _real="$(readlink -f "$_qso" 2>/dev/null || true)"
  if [ -n "$_real" ] && [ -e "$_real" ]; then
    _rbase="$(basename "$_real")"
    [ -e "$USBIN/usr/lib/$MULTIARCH/$_rbase" ] || ln -sfn "$_real" "$USBIN/usr/lib/$MULTIARCH/$_rbase"
  fi
done
info "⑥ Qt6 cmake 配置内 /usr 绝对路径已改写 -> $USBIN (运行期 .so 已软链系统库)"

# --- ⑦ 生成 env.sh -----------------------------------------------------------
cat > "$USER_DEPS/env.sh" <<EOF
# EasyPainter/StickyNotes 用户级系统依赖环境(由 tools/install-user-deps.sh 生成)。
# 用法: source $USER_DEPS/env.sh
export MINE_ROOT="$MINE_ROOT"
export USER_DEPS="$USER_DEPS"
export QT_PREFIX="$USBIN/usr"
export PATH="$USBIN/usr/lib/qt6/bin:$SDK_X64/bin:$USBIN/usr/bin:\$PATH"
export PKG_CONFIG_PATH="$USBIN/usr/lib/$MULTIARCH/pkgconfig:$USBIN/usr/share/pkgconfig\${PKG_CONFIG_PATH:+:\$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$USBIN/usr:$SDK_X64"
export CMAKE_INCLUDE_PATH="$USBIN/usr/include/$MULTIARCH/qt6:$USBIN/usr/include:$SDK_X64/include"
export LD_LIBRARY_PATH="$SDK_X64/lib:$USBIN/usr/lib/$MULTIARCH\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export VK_DRIVER_FILES="$VK_DRIVER_FILES"
EOF
info "⑥ 已生成 $USER_DEPS/env.sh"

# ===================== 真实探针(防假绿) ======================================
PROBE_DIR="$DEB_CACHE/probe"
mkdir -p "$PROBE_DIR"
export PATH="$SDK_X64/bin:$USBIN/usr/bin:$PATH"
export PKG_CONFIG_PATH="$USBIN/usr/lib/$MULTIARCH/pkgconfig:$USBIN/usr/share/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$USBIN/usr:$SDK_X64"
export CMAKE_INCLUDE_PATH="$USBIN/usr/include:$SDK_X64/include"
export LD_LIBRARY_PATH="$SDK_X64/lib:$USBIN/usr/lib/$MULTIARCH:${LD_LIBRARY_PATH:-}"
export VK_DRIVER_FILES="$VK_DRIVER_FILES"
CC="${CC:-cc}"

# 探针 ③: glslc 编译最小 frag → spv
info "探针 ③: glslc 编译 frag"
"$SDK_X64/bin/glslc" -fshader-stage=fragment -o "$PROBE_DIR/probe.frag.spv" - <<'EOF' || die "glslc 编译失败"
#version 450
layout(location=0) out vec4 outColor;
void main(){ outColor = vec4(1.0,0.0,0.0,1.0); }
EOF
info "探针 ③ 通过: $(ls -la "$PROBE_DIR/probe.frag.spv" | awk '{print $5}') 字节"

# 探针 ②: lavapipe vkCreateInstance + vkEnumeratePhysicalDevices ≥1
cat > "$PROBE_DIR/vk_probe.c" <<'EOF'
#include <vulkan/vulkan.h>
#include <stdio.h>
int main(void) {
  VkInstance inst;
  VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="p", .applicationVersion=1, .apiVersion=VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app};
  VkResult r = vkCreateInstance(&ci, NULL, &inst);
  if (r != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance failed: %d\n", r); return 2; }
  uint32_t n = 0;
  r = vkEnumeratePhysicalDevices(inst, &n, NULL);
  if (r != VK_SUCCESS || n < 1) { fprintf(stderr, "no physical device (r=%d n=%u)\n", r, n); return 3; }
  fprintf(stderr, "physical devices=%u\n", n);
  vkDestroyInstance(inst, NULL);
  return 0;
}
EOF
info "探针 ②: 编译+运行 lavapipe 物理设备枚举"
# 优先用部署的 libvulkan.so 符号;否则 -l:libvulkan.so.1(loader soname)
VK_LINK=""
if [ -f "$USBIN/usr/lib/$MULTIARCH/libvulkan.so" ]; then
  VK_LINK="-L$USBIN/usr/lib/$MULTIARCH -lvulkan"
elif [ -f /usr/lib/$MULTIARCH/libvulkan.so ]; then
  VK_LINK="-L/usr/lib/$MULTIARCH -lvulkan"
else
  VK_LINK="-l:libvulkan.so.1"
fi
"$CC" -std=c11 -I"$SDK_X64/include" -o "$PROBE_DIR/vk_probe" "$PROBE_DIR/vk_probe.c" $VK_LINK || die "lavapipe 探针编译失败"
if [ -n "$VK_DRIVER_FILES" ]; then
  VK_DRIVER_FILES="$VK_DRIVER_FILES" "$PROBE_DIR/vk_probe" || die "lavapipe 探针失败(无物理设备)"
else
  "$PROBE_DIR/vk_probe" || die "lavapipe 探针失败(无物理设备)"
fi
info "探针 ② 通过: lavapipe 枚举到 ≥1 物理设备"

# 探针 ①: Xvfb + X11 XOpenDisplay
if [ -n "${XVFB_BIN:-}" ] && [ -x "${XVFB_BIN:-/nonexistent}" ]; then
  info "探针 ①: 起 Xvfb :99 并测 XOpenDisplay"
  # 清理可能残留的 X99 锁/套接字
  rm -f /tmp/.X99-lock /tmp/.X11-unix/X99 2>/dev/null || true
  "$XVFB_BIN" :99 -screen 0 1280x720x24 >"$DEB_CACHE/xvfb.log" 2>&1 &
  XVFB_PID=$!
  trap 'kill $XVFB_PID 2>/dev/null || true' EXIT
  sleep 1.5
  cat > "$PROBE_DIR/x11_probe.c" <<'EOF'
#include <X11/Xlib.h>
#include <stdio.h>
int main(void) {
  Display *d = XOpenDisplay(NULL);
  if (!d) { fprintf(stderr, "XOpenDisplay failed\n"); return 2; }
  fprintf(stderr, "display %s ok\n", DisplayString(d));
  XCloseDisplay(d);
  return 0;
}
EOF
  X11_CFLAGS="-I$USBIN/usr/include -I$USBIN/usr/include/$MULTIARCH"
  X11_LIBS="-L$USBIN/usr/lib/$MULTIARCH -lX11"
  "$CC" -std=c11 $X11_CFLAGS -o "$PROBE_DIR/x11_probe" "$PROBE_DIR/x11_probe.c" $X11_LIBS -Wl,-rpath,"$USBIN/usr/lib/$MULTIARCH" || die "X11 探针编译失败"
  DISPLAY=:99 "$PROBE_DIR/x11_probe" || die "X11 探针失败(Xvfb 未就绪?)"
  kill "$XVFB_PID" 2>/dev/null || true
  wait "$XVFB_PID" 2>/dev/null || true
  trap - EXIT
  info "探针 ① 通过: Xvfb + XOpenDisplay 正常"
else
  info "探针 ① 跳过: 无 Xvfb(xvfb-run 或系统 Xvfb 也可;windowed 测试依赖之)"
fi

info "全部完成。使用前执行: source $USER_DEPS/env.sh"
info "env.sh 内容概览: PATH/CMAKE_PREFIX_PATH/LD_LIBRARY_PATH/VK_DRIVER_FILES 已指向用户级部署"
