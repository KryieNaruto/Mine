# 独立 oracle:生成数值 golden 的基准点(直接驱动 ink 原生 API)。
# 用法: source .user-deps/env.sh
#   cmake -S EasyPainter/tools -B EasyPainter/tools/build -G Ninja
#   cmake --build EasyPainter/tools/build
#   ./EasyPainter/tools/build/generate_golden > EasyPainter/tests/data/golden_points.txt
cmake_minimum_required(VERSION 3.22)
project(OracleGolden CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MINE_ROOT "$ENV{MINE_ROOT}")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/release")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})

find_package(absl CONFIG REQUIRED)
find_package(InkStrokeModeler CONFIG REQUIRED)
# ink 导出 targets 不带 include dir(实测),手动补池前缀 include
set(_ink_inc "${MINE_ROOT}/third_party/_install/ink-stroke-modeler-main/release/include")

add_executable(generate_golden oracle_golden.cpp)
target_include_directories(generate_golden PRIVATE "${_ink_inc}")
target_link_libraries(generate_golden PRIVATE InkStrokeModeler::stroke_modeler)
