# mb_capi 桌面验收的**独立**构建(不属于 SDK 主工程)
#
# 为什么单独一个 CMakeLists:主 CMakeLists.txt 归 t5 管(那轮才真正产出 SHARED 库、
# 把内核改 OBJECT、把 PUBLIC include 收窄)。在那之前，t3 需要一个**可运行**的证据:
# 真的把 scene_renderer.cpp / mb_capi.cpp / jni_bridge.cpp 编进去、真的链接、
# 真的调 MB_API 入口。本文件只做这件事,不碰主工程一行。
#
# 用法(在仓库根):
#   cmake -S MagicBlocks/tests/capi_smoke -B MagicBlocks/build-uisplit/capi_smoke \
#         -G Ninja -DCMAKE_BUILD_TYPE=Release -DMINE_ROOT=<Mine 根>
#   cmake --build MagicBlocks/build-uisplit/capi_smoke
#   MagicBlocks/build-uisplit/capi_smoke/mb_capi_smoke.exe
#
# 与主工程的区别(有意为之):
#   - 这里把 SDK 内核以**静态库**编进来,只为"能跑";主工程 t5 才产 SHARED。
#   - 仍然定义 MB_CAPI_BUILDING_LIBRARY:mb_capi.cpp 是**实现侧**,MB_API 必须是
#     dllexport 而不是 dllimport。若忘了定义,MSVC 在 C 里会报 C4232/链接期炸
#     —— 这正是我们想让它编译期就暴露的坑。
cmake_minimum_required(VERSION 3.22)
project(MagicBlocksCapiSmoke C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/utf-8 /W4)
endif()

set(MB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
get_filename_component(MB_ROOT "${MB_ROOT}" ABSOLUTE)
if(NOT MINE_ROOT)
  set(MINE_ROOT "${MB_ROOT}/..")
endif()

set(_variant "release")
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})
find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT Vulkan_GLSLC_EXECUTABLE)
  find_program(Vulkan_GLSLC_EXECUTABLE glslc REQUIRED)
endif()

set(MB_VMA_INCLUDE "${MINE_ROOT}/third_party/_install/vma-v3.5.0/${_variant}/include")
set(_tess2_inc "${MINE_ROOT}/third_party/_install/libtess2-v1.0.2/${_variant}/include")
set(_tess2_lib "${MINE_ROOT}/third_party/_install/libtess2-v1.0.2/${_variant}/lib/tess2${CMAKE_STATIC_LIBRARY_SUFFIX}")

# --- 着色器:主工程会用 embed_shaders.py 内嵌 cube.vert/cube.frag ---
set(MB_SHADER_SPV)
foreach(_s fullscreen.vert fullscreen.frag ui_overlay.vert ui_overlay.frag l2_scene.vert l2_scene.frag cube.vert cube.frag stroke_ribbon.vert stroke_ribbon.frag l3_scene.vert l3_scene.frag l3_scene_textured.vert l3_scene_textured.frag pixel_regression.vert pixel_regression.frag overlay2d.vert overlay2d.frag sky.vert sky.frag solid.vert solid.frag)
  get_filename_component(_ext ${_s} EXT)
  string(SUBSTRING ${_ext} 1 -1 _stage)
  set(_spv "${CMAKE_CURRENT_BINARY_DIR}/${_s}.spv")
  add_custom_command(OUTPUT ${_spv}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} -fshader-stage=${_stage} -o ${_spv}
            ${MB_ROOT}/shaders/${_s}
    DEPENDS ${MB_ROOT}/shaders/${_s} VERBATIM)
  list(APPEND MB_SHADER_SPV ${_spv})
endforeach()
set(MB_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/magicblocks_shaders.h")
add_custom_command(OUTPUT ${MB_SHADER_HEADER}
  COMMAND ${Python3_EXECUTABLE} ${MB_ROOT}/shaders/embed_shaders.py
          ${MB_SHADER_HEADER} ${MB_SHADER_SPV}
  DEPENDS ${MB_SHADER_SPV} ${MB_ROOT}/shaders/embed_shaders.py VERBATIM)

# --- SDK 内核(与主工程同源,只是少了大半 CLI/测试用的 TU) ---
set(MB_SURFACE_SRC
  ${MB_ROOT}/src/backend/vulkan/vulkan_swapchain_win32.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_swapchain_xcb_stub.cpp)

add_library(mbsdk_kernel STATIC
  ${MB_ROOT}/src/backend/vulkan/vulkan_instance.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_device.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_target.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_allocator.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_resource_common.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_buffer.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_texture.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_cubemap.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_pipeline.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_command.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_frame_sync.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_swapchain.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_swapchain_android.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_draw.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_render_device.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_device_frame.cpp
  ${MB_ROOT}/src/backend/vulkan/vulkan_ui_overlay.cpp
  ${MB_SURFACE_SRC}
  ${MB_ROOT}/src/core/device_factory.cpp
  # core 对后端私有能力的唯一转发面(按采样数建链/按交换链形状建管线/实际采样数/旋转角)。
  # t3 之前这些动作散在 android/scene_renderer.cpp 里,靠 dynamic_cast<backend::…> 完成;
  # G3 判红后收进这一个文件。
  ${MB_ROOT}/src/core/device_ops.cpp
  ${MB_ROOT}/src/core/image_io.cpp
  ${MB_ROOT}/src/core/pipeline_factory_table.cpp
  ${MB_ROOT}/src/primitives/types.cpp
  ${MB_ROOT}/src/primitives/camera.cpp
  ${MB_ROOT}/src/primitives/shader_library.cpp
  ${MB_ROOT}/src/primitives/material.cpp
  ${MB_ROOT}/src/primitives/alloc.cpp
  ${MB_ROOT}/src/primitives/batch.cpp
  ${MB_ROOT}/src/primitives/mesh.cpp
  ${MB_ROOT}/src/primitives/mesh_factory.cpp
  ${MB_ROOT}/src/primitives/mesh_loader.cpp
  ${MB_ROOT}/src/primitives/world.cpp
  ${MB_ROOT}/src/primitives/painter.cpp
  ${MB_ROOT}/src/primitives/primitive_renderer.cpp
  ${MB_ROOT}/src/primitives/overlay2d.cpp
  ${MB_ROOT}/src/primitives/picker.cpp
  ${MB_ROOT}/src/primitives/gizmo.cpp
  ${MB_ROOT}/src/primitives/sky.cpp
  ${MB_ROOT}/src/primitives/model_catalog.cpp
  ${MB_SHADER_HEADER})

target_include_directories(mbsdk_kernel PUBLIC ${MB_ROOT}/include)
target_include_directories(mbsdk_kernel PRIVATE
  ${MB_ROOT}/src ${CMAKE_CURRENT_BINARY_DIR} ${MB_VMA_INCLUDE} ${_tess2_inc})
target_link_libraries(mbsdk_kernel PUBLIC Vulkan::Vulkan PRIVATE ${_tess2_lib})
target_compile_definitions(mbsdk_kernel PRIVATE VK_USE_PLATFORM_WIN32_KHR)

# --- 待验的三件东西:新 C ABI + PIMPL 实现 + JNI 薄层 ---
# 注意:`mb_capi_layer` 是 STATIC 库,它的对象只在需要时才被拉进链接。
# 若某个可执行文件只用到 A(比如只调 mb_capi_*,不调任何 Java_* 入口),
# 链接器**不会**拉 jni_bridge.cpp.obj,也就不会发现它依赖的 JNIEnv 成员函数
# 没有被定义 —— 而真正链接进最终 .so 时才发现。所以这里显式要求:
# 凡是链 mb_capi_layer 的可执行文件,都必须一起链 jni_stub.cpp。
# (真机构建里 JNIEnv 由 libart 提供,不存在这个问题;这里是桌面替身才有的坑。)
add_library(mb_capi_layer STATIC
  ${MB_ROOT}/src/capi/mb_capi.cpp
  ${MB_ROOT}/src/android/scene_renderer.cpp
  ${MB_ROOT}/src/android/jni_bridge.cpp
  ${MB_ROOT}/tests/capistub/jni_stub.cpp)
target_include_directories(mb_capi_layer PUBLIC ${MB_ROOT}/include)
target_include_directories(mb_capi_layer PRIVATE ${MB_ROOT}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(mb_capi_layer PUBLIC mbsdk_kernel)
# mb_capi.cpp 是**实现侧**:MB_API 必须退化成 dllexport。
target_compile_definitions(mb_capi_layer PRIVATE
  MB_CAPI_BUILDING_LIBRARY
  VK_USE_PLATFORM_WIN32_KHR)
# 桌面替身 JNI 头:只用于本目标(真机构建用 NDK sysroot 的真 jni.h)
target_include_directories(mb_capi_layer PRIVATE ${MB_ROOT}/tests/capistub)

add_executable(mb_capi_smoke main.cpp)
target_include_directories(mb_capi_smoke PRIVATE ${MB_ROOT}/include ${MB_ROOT}/tests/capistub)
target_link_libraries(mb_capi_smoke PRIVATE mb_capi_layer)
