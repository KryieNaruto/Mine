cmake_minimum_required(VERSION 3.22)
project(EasyPainter CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # 源码含中文注释;MSVC 默认按系统代码页(如中文 Windows 的 936/GBK)解读源文件,
  # 非 ASCII 字节把字符串字面量拆散,报 C2001/C2143(且伴随 C4819 编码警告)。
  # /utf-8 强制源码与运行期字符集按 UTF-8 解析,跟 GCC/Clang 的默认行为对齐。
  add_compile_options(/utf-8)
endif()

# 池根:优先取缓存变量,否则推导 MINE_ROOT
set(MINE_ROOT "$ENV{MINE_ROOT}" CACHE PATH "Mine workspace root")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

# 变体解析:CMAKE_BUILD_TYPE(单配置生成器,如 Ninja)→ 池产物子目录(release/debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
if(NOT _variant)
  # 多配置生成器(VS)下 CMAKE_BUILD_TYPE 恒空;gen-projects.py 按 --variant 生成的
  # 单变体 .sln 会把 CMAKE_CONFIGURATION_TYPES 锁成唯一一个值(Debug 或 Release),
  # 借它识别 —— 否则 debug 变体的 .sln 也会算成 release,ink-stroke-modeler 手动
  # include 目录(见下)对不上实际链接的 debug 库。
  string(TOLOWER "${CMAKE_CONFIGURATION_TYPES}" _variant)
endif()
if(NOT _variant)
  set(_variant "release")
endif()
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})

# CONFIG 模式 find_package 除了上面已关掉的 PATH 派生前缀 + 包注册表,还有一路
# 没堵上:CMAKE_SYSTEM_PREFIX_PATH(NO_CMAKE_SYSTEM_PATH 控制)。若 gen-projects.py
# 实际调用到的 `cmake` 解析到 MSYS2 pacman 装的 mingw-w64-x86_64-cmake(即
# /mingw64/bin/cmake.exe,和给 win-deps.sh/msvc_env.py 用的原生 Windows CMake 是
# 两份不同的二进制),这份 cmake 自己编译时就把 /mingw64 焊进了内建的
# CMAKE_SYSTEM_PREFIX_PATH——跟 PATH 环境变量、CMakeCache 是否干净都无关,查的是
# cmake.exe 自身编译期常量。同样关掉;CMAKE_ENVIRONMENT_PATH(NO_CMAKE_ENVIRONMENT_PATH
# 控制的 CMAKE_PREFIX_PATH 环境变量,不同于上面关的 OS PATH 变量)一并收窄,不留
# 死角。这几个池自建的 CONFIG 包全部关掉,只认上面已显式收窄到池前缀的
# CMAKE_PREFIX_PATH cmake 变量 —— 只对这些 find_package(... CONFIG) 调用生效,不动
# 全局变量:Vulkan/PkgConfig 是 MODULE 模式(FindVulkan.cmake 内部靠
# find_path/find_library),走系统安装,不能一并关掉,否则 Linux 找不到系统
# Vulkan(本机已复现过一次这个更宽的误伤)。
set(_pool_only
  NO_CMAKE_ENVIRONMENT_PATH
  NO_SYSTEM_ENVIRONMENT_PATH
  NO_CMAKE_SYSTEM_PATH
  NO_CMAKE_PACKAGE_REGISTRY
  NO_CMAKE_SYSTEM_PACKAGE_REGISTRY)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(absl CONFIG REQUIRED ${_pool_only})
find_package(glm CONFIG REQUIRED ${_pool_only})
find_package(GTest CONFIG REQUIRED ${_pool_only})
find_package(InkStrokeModeler CONFIG REQUIRED ${_pool_only})
find_package(Vulkan REQUIRED)
# glslc:优先用 find_package(Vulkan) 自带探测到的 Vulkan_GLSLC_EXECUTABLE(CMake >= 3.24,
# 随 Vulkan SDK 一起找到);更早版本兜底 find_program 现场找。
if(NOT Vulkan_GLSLC_EXECUTABLE)
  find_program(Vulkan_GLSLC_EXECUTABLE glslc REQUIRED)
endif()
# glfw:仅 windowed 目标(easypainter)需要,在 Task 6 目标作用域 find_package;headless/CLI 构建不依赖

# --- shader:glslc 编译 GLSL → SPIR-V,再嵌入 C++ 头(运行期无文件路径依赖) ---
# 用 configure 期解析出的绝对路径(${Vulkan_GLSLC_EXECUTABLE}/${Python3_EXECUTABLE}),
# 不用裸命令 glslc/python3 —— VS 打开 .sln 直接生成时继承系统 PATH,不是 configure 那次
# (gen-projects.py 从已 source env.sh 的 shell 里跑)的 PATH,裸命令会在构建期报
# "不是内部或外部命令"(MSB8066/退出码 9009)。
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stroke.vert.spv ${CMAKE_CURRENT_BINARY_DIR}/stroke.frag.spv
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} -fshader-stage=vert -o ${CMAKE_CURRENT_BINARY_DIR}/stroke.vert.spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.vert
  COMMAND ${Vulkan_GLSLC_EXECUTABLE} -fshader-stage=frag -o ${CMAKE_CURRENT_BINARY_DIR}/stroke.frag.spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.frag
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.frag
  VERBATIM)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stroke_shaders.h
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_shaders.py
          ${CMAKE_CURRENT_BINARY_DIR}/stroke.vert.spv
          ${CMAKE_CURRENT_BINARY_DIR}/stroke.frag.spv
          ${CMAKE_CURRENT_BINARY_DIR}/stroke_shaders.h
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/stroke.vert.spv ${CMAKE_CURRENT_BINARY_DIR}/stroke.frag.spv
  VERBATIM)

# --- imgui:项目内 vendor,不走池(官方无稳定 install config) ---
add_library(imgui STATIC
  vendor/imgui/imgui.cpp
  vendor/imgui/imgui_draw.cpp
  vendor/imgui/imgui_tables.cpp
  vendor/imgui/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC vendor/imgui)

# --- easypainter_core:stroke 封装 + 渲染核心 + bench 的静态库 ---
add_library(easypainter_core STATIC
  src/core/stroke/types.h
  src/core/stroke/input_source.h
  src/core/stroke/input_source.cpp
  src/core/stroke/predictor.h
  src/core/stroke/predictor.cpp
  src/core/render/image_io.h
  src/core/render/image_io.cpp
  src/core/render/vulkan_context.h
  src/core/render/vulkan_context.cpp
  src/core/render/pipeline.h
  src/core/render/pipeline.cpp
  src/core/render/offscreen.h
  src/core/render/offscreen.cpp
  src/core/bench/bench.h
  src/core/bench/bench.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/stroke_shaders.h)
target_include_directories(easypainter_core PUBLIC src)
target_include_directories(easypainter_core PRIVATE vendor ${CMAKE_CURRENT_BINARY_DIR})

# ink 导出 targets 不带 include dir(Task 1 实测),手动补池前缀 include(variant 无关)
set(_ink_inc "${MINE_ROOT}/third_party/_install/ink-stroke-modeler-main/${_variant}/include")
if(EXISTS "${_ink_inc}")
  target_include_directories(easypainter_core PUBLIC "${_ink_inc}")
endif()

target_link_libraries(easypainter_core PUBLIC
  InkStrokeModeler::stroke_modeler glm::glm Vulkan::Vulkan)

# --- CLI:无头离屏渲染(不依赖 GLFW/窗口) ---
add_executable(easypainter-cli src/cli/main.cpp src/cli/example_points.h)
target_link_libraries(easypainter-cli PRIVATE easypainter_core)  # Vulkan 经 core 的 PUBLIC 传递

# --- windowed 应用(仅此目标依赖 GLFW;headless/CLI 构建不依赖) ---
find_package(glfw3 CONFIG REQUIRED ${_pool_only})
if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  # glfw 导出 target 不带 X11 链接(GLFW 把 X11 当 PRIVATE 依赖),手动经 pkg-config 补;仅 Linux
  pkg_check_modules(X11_REQ IMPORTED_TARGET x11 xrandr xinerama xcursor xi xext xrender xfixes)
endif()

add_executable(easypainter
  src/app/main.cpp
  src/app/fonts.cpp
  src/app/fonts.h
  src/app/gui.h
  src/app/gui.cpp
  vendor/imgui/backends/imgui_impl_glfw.cpp
  vendor/imgui/backends/imgui_impl_vulkan.cpp)
# imgui_impl_glfw 需 GLFW_INCLUDE_NONE 避免拉入缺失的 GL/gl.h(Vulkan 应用)
set_source_files_properties(vendor/imgui/backends/imgui_impl_glfw.cpp
  PROPERTIES COMPILE_DEFINITIONS GLFW_INCLUDE_NONE)
if(UNIX AND NOT APPLE)
  target_link_libraries(easypainter PRIVATE
    easypainter_core imgui glfw PkgConfig::X11_REQ)
else()
  target_link_libraries(easypainter PRIVATE
    easypainter_core imgui glfw)  # Windows: GLFW 自带 Win32 后端,无需 X11
endif()
target_compile_definitions(easypainter PRIVATE
  CJK_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansCJK-Regular.ttc")

# --- 测试 ---
enable_testing()
include(GoogleTest)
add_executable(stroke_test tests/stroke_test.cpp)
target_link_libraries(stroke_test PRIVATE easypainter_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(stroke_test)

add_executable(image_golden_test tests/image_golden_test.cpp)
target_link_libraries(image_golden_test PRIVATE easypainter_core GTest::gtest GTest::gtest_main)
target_include_directories(image_golden_test PRIVATE vendor)
target_compile_definitions(image_golden_test PRIVATE
  GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
gtest_discover_tests(image_golden_test)

add_executable(bench_test tests/bench_test.cpp)
target_link_libraries(bench_test PRIVATE easypainter_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(bench_test)

add_executable(golden_test tests/golden_test.cpp)
target_link_libraries(golden_test PRIVATE easypainter_core GTest::gtest GTest::gtest_main)
target_compile_definitions(golden_test PRIVATE
  GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
gtest_discover_tests(golden_test)

# --- 字体回归用例:ImGui 中文字形覆盖(先红后绿) ---
add_executable(font_test tests/font_test.cpp src/app/fonts.cpp)
target_link_libraries(font_test PRIVATE easypainter_core imgui GTest::gtest GTest::gtest_main)
target_include_directories(font_test PRIVATE src vendor/imgui vendor)
target_compile_definitions(font_test PRIVATE
  CJK_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansCJK-Regular.ttc")
gtest_discover_tests(font_test)
