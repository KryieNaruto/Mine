cmake_minimum_required(VERSION 3.22)
project({{PROJECT_NAME}} CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 池根:优先取缓存变量,否则推导 MINE_ROOT
set(MINE_ROOT "$ENV{MINE_ROOT}" CACHE PATH "Mine workspace root")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

# 变体解析:CMAKE_BUILD_TYPE(Release/Debug)→ 池产物子目录(release/debug)
# 产物在 _install/<name>-<ver>/<variant>/ 下,故按 variant glob 出各库前缀加入 CMAKE_PREFIX_PATH
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
file(GLOB _pool_dirs LIST_DIRECTORIES true
     "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})

{{DEPS_FIND}}

add_executable({{PROJECT_NAME}} src/main.cpp)
{{DEPS_LINK}}
