cmake_minimum_required(VERSION 3.22)
project(EasyPainter CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 池根:优先取缓存变量,否则推导 MINE_ROOT
set(MINE_ROOT "$ENV{MINE_ROOT}" CACHE PATH "Mine workspace root")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

# 变体解析:CMAKE_BUILD_TYPE(Release/Debug)→ 池产物子目录(release/debug)
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
if(NOT _variant)
  set(_variant "release")          # 多配置生成器(VS)下 CMAKE_BUILD_TYPE 恒空
endif()
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})

find_package(absl CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(InkStrokeModeler CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
# glfw:仅 windowed 目标(easypainter)需要,在 Task 6 目标作用域 find_package;headless/CLI 构建不依赖

# --- shader:glslc 编译 GLSL → SPIR-V,再嵌入 C++ 头(运行期无文件路径依赖) ---
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stroke.vert.spv ${CMAKE_CURRENT_BINARY_DIR}/stroke.frag.spv
  COMMAND glslc -fshader-stage=vert -o ${CMAKE_CURRENT_BINARY_DIR}/stroke.vert.spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.vert
  COMMAND glslc -fshader-stage=frag -o ${CMAKE_CURRENT_BINARY_DIR}/stroke.frag.spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.frag
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.frag
  VERBATIM)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stroke_shaders.h
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_shaders.py
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
find_package(glfw3 CONFIG REQUIRED)
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
