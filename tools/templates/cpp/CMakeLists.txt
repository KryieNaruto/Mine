cmake_minimum_required(VERSION 3.22)
project({{PROJECT_NAME}} CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 池 install 前缀(优先取缓存变量,默认按 MINE_ROOT 推导)
set(MINE_ROOT "$ENV{MINE_ROOT}" CACHE PATH "Mine workspace root")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
list(APPEND CMAKE_PREFIX_PATH "${MINE_ROOT}/third_party/_install")

{{DEPS_FIND}}

add_executable({{PROJECT_NAME}} src/main.cpp)
{{DEPS_LINK}}
