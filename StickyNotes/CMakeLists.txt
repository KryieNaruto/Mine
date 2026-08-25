cmake_minimum_required(VERSION 3.22)
project(StickyNotes CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)

add_library(stickynotes_core STATIC
  src/core/note_store.cpp
  src/core/persistence.cpp
  src/core/palette.cpp
  src/core/geometry_util.cpp
  src/core/model.h
  src/core/note_store.h
  src/core/persistence.h
  src/core/palette.h
  src/core/geometry_util.h)
target_include_directories(stickynotes_core PUBLIC src)
# model.h 含 QColor(QtGui 头);core 虽以纯逻辑为主,仍需 Qt6::Gui 的 include 路径
target_link_libraries(stickynotes_core PUBLIC Qt6::Core Qt6::Gui)

# 字体资源：运行期从源码树定位（CLI/app 均用同一 helper）
add_library(stickynotes_assets INTERFACE)
target_include_directories(stickynotes_assets INTERFACE assets)
target_compile_definitions(stickynotes_assets INTERFACE
  STICKYNOTES_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")

# app 层（QtWidgets GUI）：便签控件/无边框窗/调色板
add_library(stickynotes_app STATIC
  src/app/palette_dialog.cpp
  src/app/note_widget.cpp
  src/app/note_window.cpp
  src/app/palette_dialog.h
  src/app/note_widget.h
  src/app/note_window.h)
target_include_directories(stickynotes_app PUBLIC src)
target_link_libraries(stickynotes_app PUBLIC stickynotes_core Qt6::Widgets Qt6::Gui)

# CLI：无头入口（CRUD + offscreen 渲染 PNG）
add_executable(stickynotes-cli src/cli/main.cpp)
target_link_libraries(stickynotes-cli PRIVATE stickynotes_app stickynotes_assets)

enable_testing()
add_subdirectory(tests)
