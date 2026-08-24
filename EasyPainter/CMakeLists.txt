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
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})

find_package(absl CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(InkStrokeModeler CONFIG REQUIRED)
# glfw:仅 windowed 目标(easypainter)需要,在 Task 6 目标作用域 find_package;headless/CLI 构建不依赖

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
  src/core/stroke/predictor.cpp)
target_include_directories(easypainter_core PUBLIC src)

# ink 导出 targets 不带 include dir(Task 1 实测),手动补池前缀 include(variant 无关)
set(_ink_inc "${MINE_ROOT}/third_party/_install/ink-stroke-modeler-main/${_variant}/include")
if(EXISTS "${_ink_inc}")
  target_include_directories(easypainter_core PUBLIC "${_ink_inc}")
endif()

target_link_libraries(easypainter_core PUBLIC
  InkStrokeModeler::stroke_modeler glm::glm)

# --- 测试 ---
enable_testing()
include(GoogleTest)
add_executable(stroke_test tests/stroke_test.cpp)
target_link_libraries(stroke_test PRIVATE easypainter_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(stroke_test)
