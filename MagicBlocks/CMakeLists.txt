cmake_minimum_required(VERSION 3.22)
project(MagicBlocks C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # 源码含中文注释;MSVC 默认按系统代码页(中文 Windows 为 936/GBK)解读,非 ASCII 字节
  # 会把字符串字面量拆散(C2001/C2143)。/utf-8 与 GCC/Clang 默认行为对齐。
  add_compile_options(/utf-8)
endif()

# 池根:优先取缓存变量,否则推导 MINE_ROOT
set(MINE_ROOT "$ENV{MINE_ROOT}" CACHE PATH "Mine workspace root")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

# 变体解析:CMAKE_BUILD_TYPE(单配置生成器)→ 池产物子目录(release/debug)。
# 多配置生成器(VS)下 CMAKE_BUILD_TYPE 恒空,按 CMAKE_CONFIGURATION_TYPES 识别。
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
if(NOT _variant)
  string(TOLOWER "${CMAKE_CONFIGURATION_TYPES}" _variant)
endif()
if(NOT _variant)
  set(_variant "release")
endif()
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})

# CONFIG 模式 find_package 只认池前缀:关掉 PATH 派生/环境/系统/注册表几条旁路,
# 防止误命中系统里的同名包(与 EasyPainter 同口径)。
set(_pool_only
  NO_CMAKE_ENVIRONMENT_PATH
  NO_SYSTEM_ENVIRONMENT_PATH
  NO_CMAKE_SYSTEM_PATH
  NO_CMAKE_PACKAGE_REGISTRY
  NO_CMAKE_SYSTEM_PACKAGE_REGISTRY)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Android 交叉编译：不构建测试、不用 find_package(Vulkan)（Vulkan 头在 NDK sysroot 里）。
option(MB_BUILD_TESTS "构建单元测试（Android 交叉编译时关掉）" ON)
if(ANDROID)
  set(MB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

# 池里 googletest 导出的是 GTest CONFIG 包,target 为 GTest::gtest / GTest::gtest_main
if(MB_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED ${_pool_only})
endif()
if(NOT ANDROID)
  find_package(Vulkan REQUIRED)
endif()
if(ANDROID)
  # NDK 自带 glslc：$NDK/shader-tools/<host>/glslc
  if(NOT Vulkan_GLSLC_EXECUTABLE)
    file(GLOB _ndk_glslc "$ENV{ANDROID_NDK_HOME}/shader-tools/*/glslc"
         "$ENV{ANDROID_NDK_HOME}/shader-tools/*/glslc.exe")
    if(_ndk_glslc)
      list(GET _ndk_glslc 0 Vulkan_GLSLC_EXECUTABLE)
    endif()
  endif()
endif()
if(NOT Vulkan_GLSLC_EXECUTABLE)
  find_program(Vulkan_GLSLC_EXECUTABLE glslc REQUIRED)
endif()

# --- VMA v3.5.0(spec C7 / M1 的唯一分配器实现) ---
# 纯头文件库:池里只产出 include/vk_mem_alloc.h,没有 CMake 包。
# 约定:只有 src/backend/vulkan/vulkan_allocator.cpp 可以 include 它(门控 G4 断言),
# 且 VMA_IMPLEMENTATION 也必须只在该 TU 定义 —— 这里只把 include 路径给出去。
set(MB_VMA_INCLUDE "${MINE_ROOT}/third_party/_install/vma-v3.5.0/${_variant}/include")
if(NOT EXISTS "${MB_VMA_INCLUDE}/vk_mem_alloc.h")
  message(FATAL_ERROR "缺少 VMA 头:${MB_VMA_INCLUDE}/vk_mem_alloc.h\n"
                      "先运行 tools/build-deps.py --variant ${_variant} 产出 vma-v3.5.0")
endif()
message(STATUS "MagicBlocks: variant=${_variant} glslc=${Vulkan_GLSLC_EXECUTABLE} vma=${MB_VMA_INCLUDE}")

# --- shader:GLSL → glslc → SPIR-V → 内嵌 C++ 头(运行期无文件路径依赖) ---
# 只有 Vertex / Fragment 两种 stage(G1:禁止几何着色器)。
set(MB_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(MB_SHADER_SPV)
# cube.* 是 demo 用着色器（不属于 SDK 的任何层），证明 L0~L3 已能渲染透视 3D。
# l3_scene_textured.* 是 P0 纹理采样通道用的一对（F-ST 印章：颜色+深度双贴图）。
set(MB_SHADER_NAMES fullscreen.vert fullscreen.frag ui_overlay.vert ui_overlay.frag l2_scene.vert l2_scene.frag cube.vert cube.frag stroke_ribbon.vert stroke_ribbon.frag l3_scene.vert l3_scene.frag l3_scene_textured.vert l3_scene_textured.frag pixel_regression.vert pixel_regression.frag overlay2d.vert overlay2d.frag sky.vert sky.frag solid.vert solid.frag)
foreach(_s ${MB_SHADER_NAMES})
  get_filename_component(_ext ${_s} EXT)
  string(SUBSTRING ${_ext} 1 -1 _stage)
  set(_spv "${CMAKE_CURRENT_BINARY_DIR}/${_s}.spv")
  add_custom_command(
    OUTPUT ${_spv}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} -fshader-stage=${_stage}
            -o ${_spv} ${MB_SHADER_DIR}/${_s}
    DEPENDS ${MB_SHADER_DIR}/${_s}
    VERBATIM)
  list(APPEND MB_SHADER_SPV ${_spv})
endforeach()
set(MB_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/magicblocks_shaders.h")
add_custom_command(
  OUTPUT ${MB_SHADER_HEADER}
  COMMAND ${Python3_EXECUTABLE} ${MB_SHADER_DIR}/embed_shaders.py
          ${MB_SHADER_HEADER} ${MB_SHADER_SPV}
  DEPENDS ${MB_SHADER_SPV} ${MB_SHADER_DIR}/embed_shaders.py
  VERBATIM)

# --- magicblocks:渲染引擎 SDK 核心库(本轮只有 L0 device/ + Vulkan 后端) ---
#
# 平台 surface TU:按 OS 选编实际实现;另一侧的 *_stub.cpp 提供"本平台不支持"的
# 同签名定义,这样 VulkanSwapchain::init() 不需要 #if,也不会出现未解析符号。
set(MB_SURFACE_SRC)
if(ANDROID)
  # Android：真实 Android surface + 另两个平台的 stub（保证符号齐全）
  set(MB_SURFACE_SRC
    src/backend/vulkan/vulkan_swapchain_android.cpp
    src/backend/vulkan/vulkan_swapchain_android.h
    src/backend/vulkan/vulkan_swapchain_win32_stub.cpp
    src/backend/vulkan/vulkan_swapchain_xcb_stub.cpp)
elseif(WIN32)
  set(MB_SURFACE_SRC
    src/backend/vulkan/vulkan_swapchain_win32.cpp
    src/backend/vulkan/vulkan_swapchain_win32.h
    src/backend/vulkan/vulkan_swapchain_xcb_stub.cpp)
elseif(UNIX AND NOT APPLE)
  set(MB_SURFACE_SRC
    src/backend/vulkan/vulkan_swapchain_xcb.cpp
    src/backend/vulkan/vulkan_swapchain_xcb.h
    src/backend/vulkan/vulkan_swapchain_win32_stub.cpp)
endif()

add_library(magicblocks STATIC
  include/magicblocks/device/types.h
  include/magicblocks/device/result.h
  include/magicblocks/device/events.h
  include/magicblocks/device/lifecycle.h
  include/magicblocks/device/render_device.h
  include/magicblocks/device/swapchain.h
  include/magicblocks/device/render_target.h
  include/magicblocks/device/frame_sync.h
  include/magicblocks/resources/handles.h
  include/magicblocks/resources/memory.h
  include/magicblocks/resources/buffer.h
  include/magicblocks/resources/texture.h
  include/magicblocks/resources/pipeline.h
  include/magicblocks/resources/command.h
  # --- L2 场景层(spec §4 L2 S1-S9):公开头 ---
  include/magicblocks/scene/types.h
  include/magicblocks/scene/scene.h
  include/magicblocks/scene/submit_queue.h
  include/magicblocks/scene/render_graph.h
  include/magicblocks/scene/command_stream.h
  include/magicblocks/scene/feature.h
  include/magicblocks/scene/recorder.h
  # --- UI 消费层(spec ui-consumer-design §2):公开头 ---
  include/magicblocks/ui/types.h
  include/magicblocks/ui/sdk_stub.h
  include/magicblocks/ui/scene_stub.h
  include/magicblocks/ui/overlay.h
  include/magicblocks/ui/frame_renderer.h
  include/magicblocks/ui/input.h
  include/magicblocks/ui/hud.h
  include/magicblocks/ui/router.h
  include/magicblocks/ui/perf.h
  include/magicblocks/ui/frame_renderer.h
  include/magicblocks/ui/ui_app.h
  src/backend/vulkan/vk_common.h
  src/backend/vulkan/vulkan_instance.h
  src/backend/vulkan/vulkan_instance.cpp
  src/backend/vulkan/vulkan_device.cpp
  src/backend/vulkan/vulkan_target.h
  src/backend/vulkan/vulkan_target.cpp
  src/backend/vulkan/vulkan_allocator.h
  src/backend/vulkan/vulkan_allocator.cpp
  # L1 资源与命令对象层(spec R1-R5)
  src/backend/vulkan/vulkan_resource_common.h
  src/backend/vulkan/vulkan_resource_common.cpp
  src/backend/vulkan/vulkan_buffer.h
  src/backend/vulkan/vulkan_buffer.cpp
  src/backend/vulkan/vulkan_texture.h
  src/backend/vulkan/vulkan_texture.cpp
  # L1 立方体贴图(R1 扩展):纹理侧能力,命名中立,不含任何使用方语义
  src/backend/vulkan/vulkan_cubemap.h
  src/backend/vulkan/vulkan_cubemap.cpp
  src/backend/vulkan/vulkan_pipeline.h
  src/backend/vulkan/vulkan_pipeline.cpp
  src/backend/vulkan/vulkan_command.h
  src/backend/vulkan/vulkan_command.cpp
  src/backend/vulkan/vulkan_frame_sync.h
  src/backend/vulkan/vulkan_frame_sync.cpp
  # L0 多帧并行同步核心:N 槽 image_available/render_finished 信号量 + submit fence +
  # 持久命令缓冲 + 帧内对象延迟销毁栈(取代 vulkan_render_device 里的 fence 轮转池)。
  src/backend/vulkan/vulkan_sync_core.h
  src/backend/vulkan/vulkan_sync_core.cpp
  src/backend/vulkan/vulkan_swapchain.h
  src/backend/vulkan/vulkan_swapchain.cpp
  src/backend/vulkan/vulkan_swapchain_android.cpp
  src/backend/vulkan/vulkan_swapchain_android.h
  src/backend/vulkan/vulkan_draw.h
  src/backend/vulkan/vulkan_draw.cpp
  src/backend/vulkan/vulkan_render_device.h
  src/backend/vulkan/vulkan_render_device.cpp
  # L3 设备帧提交器：把已录制的命令真正编码成 vkCmd* 并提交。
  src/backend/vulkan/vulkan_device_frame.h
  src/backend/vulkan/vulkan_device_frame.cpp
  # UI 叠加层后端(Vk* 只允许出现在 src/backend/**,门控 G3)
  src/backend/vulkan/vulkan_ui_overlay.h
  src/backend/vulkan/vulkan_ui_overlay.cpp
  # L2 场景层本体(不含任何 Vk* 符号;门控 G3 按 backend 之外零 Vk 扫)
  src/scene/scene_internal.h
  src/scene/command_stream.cpp
  src/scene/scene_impl.cpp
  src/scene/submit_queue_impl.cpp
  src/scene/render_graph_impl.cpp
  src/scene/feature_registry.cpp
  src/scene/recorder.cpp
  # UI 消费层本体(不含任何 Vk* 符号)
  src/ui/overlay_impl.h
  src/ui/overlay_renderer.cpp
  src/ui/hud.cpp
  src/ui/input_router.cpp
  src/ui/router.cpp
  src/ui/perf_governor.cpp
  src/ui/scene_frame.cpp
  src/ui/scene_sink_host.h
  src/ui/scene_sink_host.cpp
  src/ui/scene_offscreen.cpp
  src/ui/offscreen_target.h
  src/ui/ui_app_internal.h
  src/ui/ui_app.cpp
  ${MB_SURFACE_SRC}
  # L3 primitives 层本体（不含任何 Vk* 符号；门控 G3/G7/G8 按此目录扫）
  src/primitives/types.cpp
  src/primitives/camera.cpp
  src/primitives/shader_library.cpp
  src/primitives/material.cpp
  src/primitives/alloc.cpp
  src/primitives/batch.cpp
  src/primitives/mesh.cpp
  src/primitives/mesh_factory.cpp
  src/primitives/mesh_loader.cpp
  src/primitives/world_storage.h
  src/primitives/world.cpp
  src/primitives/painter.cpp
  src/primitives/primitive_renderer.cpp
  src/primitives/overlay2d.cpp
  src/primitives/picker.cpp
  src/primitives/gizmo.cpp
  # L3 方向背景(sky):相机朝向相关的通用原语(不含玩法语义)
  include/magicblocks/primitives/sky.h
  src/primitives/sky.cpp
  # L3 预设模型目录:扫描/加载/上传(模型名是数据,UI 不认识)
  include/magicblocks/primitives/model_catalog.h
  src/primitives/model_catalog.cpp
  src/core/device_factory.cpp
  src/core/image_io.h
  src/core/image_io.cpp
  # L1: pipeline-factory indirection (G8) - plain value table, no backend/Vk symbols
  src/core/pipeline_factory_table.cpp
  # ---- L4 玩法层：魔法涂鸦(F-SK) ----
  src/features/sketch/sketch_document.cpp
  src/features/sketch/sketch_pipeline.cpp
  src/features/sketch/sketch_canvas.cpp
  # ---- L4 玩法层：魔法叠叠乐(F-VX) ----
  src/features/stack/block_set.cpp
  src/features/stack/block_brush.cpp
  src/features/stack/stack_render.cpp
  src/features/stack/grid_overlay.cpp
  src/features/stack/color_mode.cpp
  src/features/stack/shape_template.cpp
  src/features/stack/stack_command.cpp
  src/features/stack/stack_session.cpp
  ${MB_SHADER_HEADER})
target_include_directories(magicblocks PUBLIC include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${MB_VMA_INCLUDE})
# Android：池里的 tess2 是 Windows 二进制，不能链进 arm64。直接从源码编。
if(ANDROID)
  set(_tess2_root "${MINE_ROOT}/third_party/_src/libtess2-v1.0.2")
  if(NOT EXISTS "${_tess2_root}/Source/tess.c")
    message(FATAL_ERROR "缺少 tess2 源码：${_tess2_root}/Source/tess.c")
  endif()
  add_library(tess2 STATIC
    "${_tess2_root}/Source/bucketalloc.c"
    "${_tess2_root}/Source/dict.c"
    "${_tess2_root}/Source/geom.c"
    "${_tess2_root}/Source/mesh.c"
    "${_tess2_root}/Source/priorityq.c"
    "${_tess2_root}/Source/sweep.c"
    "${_tess2_root}/Source/tess.c")
  target_include_directories(tess2 PUBLIC "${_tess2_root}/Include")
  set(_tess2_inc "${_tess2_root}/Include")
  set(_tess2_lib tess2)
endif()

# tess2：池里的 config 导出的 INTERFACE_INCLUDE_DIRECTORIES 是错的
#   <prefix>/lib/cmake/tess2/../../include == <prefix>/lib/include
# 而头文件实际在 <prefix>/include，CMake 会因 "non-existent path" 直接失败。
# 改为按池目录直接拼路径，不用 find_package。
# 另：mesh_factory.cpp 用 `#include <tesselator.h>`（尖括号），而池里的包把 include
# 目录标成 SYSTEM（生成 /external:I），MSVC 只在加 /external:anglebrackets 时
# 才搜 /external:I。所以这里加的是**普通 -I**。
if(NOT ANDROID)
  set(_tess2_inc "${MINE_ROOT}/third_party/_install/libtess2-v1.0.2/${_variant}/include")
  set(_tess2_lib "${MINE_ROOT}/third_party/_install/libtess2-v1.0.2/${_variant}/lib/tess2${CMAKE_STATIC_LIBRARY_SUFFIX}")
  if(NOT EXISTS "${_tess2_inc}/tesselator.h")
    message(FATAL_ERROR "缺少 tess2 头：${_tess2_inc}/tesselator.h\n")
  endif()
  if(NOT EXISTS "${_tess2_lib}")
    message(FATAL_ERROR "缺少 tess2 库：${_tess2_lib}\n")
  endif()
endif()

if(ANDROID)
  target_link_libraries(magicblocks PUBLIC vulkan PRIVATE ${_tess2_lib})
else()
  target_link_libraries(magicblocks PUBLIC Vulkan::Vulkan PRIVATE ${_tess2_lib})
endif()
target_include_directories(magicblocks PRIVATE ${_tess2_inc})

# 平台 surface:VK_USE_PLATFORM_* 决定实例层追加哪个 surface 扩展
if(ANDROID)
  # ANativeWindow 来自 NDK sysroot；surface 扩展在 backend/vulkan/vulkan_swapchain_android.cpp
  # MB_ANDROID_USE_NDK 是**编译宏**（决定走真实 NDK 路径还是 stub 声明），
  # 与同名 CMake 变量是两回事：只传 -D 变量不会让源码看到它。
  target_compile_definitions(magicblocks PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
    MB_ANDROID_USE_NDK)
elseif(WIN32)
  target_compile_definitions(magicblocks PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks PRIVATE VK_USE_PLATFORM_XCB_KHR)
else()
  message(FATAL_ERROR "MagicBlocks 本轮只支持 Windows / Linux / Android(iOS 只做设计考虑)")
endif()

# --- Android TU 编译校验(A7 / Swapchain.AndroidTuCompiles) ---
# vulkan_swapchain_android.cpp **始终**参与 magicblocks 的编译(见上面的源码列表),
# 因此在任何平台上 "编译过" == "Android 分支的 ABI 形状是对的":
#   - 有 NDK(-DMB_ANDROID_USE_NDK=ON 或 ANDROID 平台)→ 真实 <android/native_window.h>
#   - 无 NDK → src/backend/vulkan/stub/anativewindow.h 提供 ANativeWindow 前置声明
# 不需要额外的 object 目标:该 TU 已经在库里,编译失败会直接让整个 SDK 构建失败。
# 非 Android 平台若显式 -DMB_ANDROID_USE_NDK=ON，也切到真实 NDK 声明路径。
if(MB_ANDROID_USE_NDK AND NOT ANDROID)
  target_compile_definitions(magicblocks PRIVATE MB_ANDROID_USE_NDK)
endif()

option(MB_ANDROID_TU_COMPILE_CHECK "断言 vulkan_swapchain_android.cpp 参与编译校验" ON)
if(NOT MB_ANDROID_TU_COMPILE_CHECK)
  message(FATAL_ERROR "MB_ANDROID_TU_COMPILE_CHECK=OFF 会让 Android 分支失去编译校验(违反 A7),本轮不允许关闭")
endif()

# 下面 6 个都是**桌面端**可执行程序（CLI / demo / 验证器），它们依赖
# VK_USE_PLATFORM_WIN32_KHR / XCB_KHR 和屏幕环境。Android 上没有这些平台
# 宏（只能是 ANDROID_KHR），强行编译会因 vulkan.h 反向 include <xcb/xcb.h> 而失败，
# 所以整段在 Android 构建里跳过。
if(NOT ANDROID)

# --- CLI:无头离屏渲染(不依赖窗口/显示服务) ---
add_executable(magicblocks-l0-cli src/cli/main.cpp)
target_link_libraries(magicblocks-l0-cli PRIVATE magicblocks)
# src/ 给 CLI 与测试用(它们要 include core/image_io.h 这类后端内头)
target_include_directories(magicblocks-l0-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# CLI 走 VK_USE_PLATFORM_* 才能看到后端私有头里间接引用的平台枚举(仅编译期)
if(WIN32)
  target_compile_definitions(magicblocks-l0-cli PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-l0-cli PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

# --- CLI:UI 消费层离屏出图 / 统计(spec §5.5、§10 门 5 的基线产出工具) ---
add_executable(magicblocks-ui-cli
    src/ui_cli/main.cpp
    src/ui_cli/render_backend.cpp
)
target_link_libraries(magicblocks-ui-cli PRIVATE magicblocks)
# ${CMAKE_CURRENT_BINARY_DIR} 提供内嵌着色器头 magicblocks_shaders.h(UI 布局 v2 渲染缩略图要用)
target_include_directories(magicblocks-ui-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-ui-cli PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-ui-cli PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

# --- CLI:L2 场景层录制/回放/复现(spec §5 命令模式)---
add_executable(magicblocks-l2-cli src/l2_cli/main.cpp)
target_link_libraries(magicblocks-l2-cli PRIVATE magicblocks)
# src/ 给 CLI 用(要 include scene/scene_internal.h 与 core/image_io.h)
target_include_directories(magicblocks-l2-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-l2-cli PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-l2-cli PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()
# --- L3 CLI: paint / blocks / gizmo 子命令（验证"能画"与 ECS+合批整条链路）---
add_executable(magicblocks-l3-cli src/l3_cli/main.cpp)
target_link_libraries(magicblocks-l3-cli PRIVATE magicblocks)
target_include_directories(magicblocks-l3-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-l3-cli PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-l3-cli PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

# --- CLI:L3 方向背景(sky)离屏出图 —— 审阅/CI 的像素证据工具 ---
add_executable(magicblocks-sky-cli src/sky_cli/main.cpp)
target_link_libraries(magicblocks-sky-cli PRIVATE magicblocks)
target_include_directories(magicblocks-sky-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-sky-cli PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-sky-cli PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

# --- CLI:L3 预设测试模型离屏出图(每个模型单独一张,给 png_inspect 断言)---
add_executable(magicblocks-model-cli src/model_cli/main.cpp)
target_link_libraries(magicblocks-model-cli PRIVATE magicblocks)
target_include_directories(magicblocks-model-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-model-cli PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-model-cli PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

# --- Demo: 旋转立方体（证明 L0~L3 已能渲染透视 3D；不属于 SDK 的任何层）---
add_executable(magicblocks-cube-demo src/cube_demo/main.cpp)
target_link_libraries(magicblocks-cube-demo PRIVATE magicblocks)
# src/ 与 build/（要 core/image_io.h 与内嵌着色器 magicblocks_shaders.h）
target_include_directories(magicblocks-cube-demo PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-cube-demo PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-cube-demo PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

# --- F-SK CLI：魔法涂鸦的离屏出图 / 录制回放(设计 §12) ---
# 独立可执行文件而不是塞进 l3-cli 的一个子命令，理由见 docs 里 SK 设计 §12：
#   - --camera ortho|perspective 是 A3 的判据，必须有自己的参数表；
#   - record/replay/reproduce 是 A7/A8 的判据，l3-cli 没有；
#   - 与 L3 的 paint/blocks/gizmo 共用参数表会让跨子命令参数被静默接受(S-8)。
add_executable(magicblocks-fsk-cli src/fsk_cli/main.cpp)
target_link_libraries(magicblocks-fsk-cli PRIVATE magicblocks)
target_include_directories(magicblocks-fsk-cli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-fsk-cli PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-fsk-cli PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

# --- 验证：Windows 上用真实 Vulkan 把一帧场景渲染到外部 surface ---
# 这是新 UI 接口（ISceneFrameSink）的可运行证据，不是单元测试里的 fake。
add_executable(magicblocks-sink-verify src/sink_verify/main.cpp)
target_link_libraries(magicblocks-sink-verify PRIVATE magicblocks)
target_include_directories(magicblocks-sink-verify PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks-sink-verify PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks-sink-verify PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

endif()  # NOT ANDROID（桌面可执行程序）

# --- Android：JNI 层 .so（给 pad 上的 Compose/SurfaceView 用）---
if(ANDROID)
  add_library(magicblocks_jni SHARED
    # scene_renderer.cpp 由 magicblocks_android_scene 提供（见下方“Android 壳层
    # 平台无关部分”）：JNI .so 与宿主测试链接的是**同一份实现**，不会出现
    # “测试测的是另一份代码”。
    src/android/jni_bridge.cpp)
  target_include_directories(magicblocks_jni PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(magicblocks_jni PRIVATE magicblocks magicblocks_android_scene android log)
  # MB_ANDROID_USE_NDK 必须给到 JNI target：jni_bridge.cpp 里
  # nativeWindowFromSurface 用 #if defined(MB_ANDROID_USE_NDK) 才调用真的
  # ANativeWindow_fromSurface；上层的同名编译定义只作用于 magicblocks target，
  # 不会传染到顶层可执行/库，所以这里必须重复一次。
  target_compile_definitions(magicblocks_jni PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
    MB_ANDROID_USE_NDK)
  set_target_properties(magicblocks_jni PROPERTIES OUTPUT_NAME "magicblocks")
endif()

# --- Android 壳层里“平台无关”的那部分（宿主机也编）---
#
# src/android/scene_renderer.cpp 里住着两种东西：
#   1. 纯值语义的管线描述（cube_pipeline_desc / model_pipeline_desc）——宿主机可测；
#   2. SceneRenderer 本体——依赖 Vulkan 设备、交换链、ANativeWindow，只能在 pad 上跑。
#
# 第 1 部分是“36 字节步长读 48 字节顶点 → 三角面片全炸”这条回归的唯一守门人，
# 而它编译期看不出来、运行期也不报错。所以把它单独编成一个目标，让 gtest 能断言：
# JNI .so 与 magicblocks_test 链接同一份源文件，测的就是上真机的那份代码。
add_library(magicblocks_android_scene
  src/android/scene_renderer.cpp)
target_include_directories(magicblocks_android_scene PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(magicblocks_android_scene PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR})
target_link_libraries(magicblocks_android_scene PUBLIC magicblocks)
if(ANDROID)
  target_compile_definitions(magicblocks_android_scene PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
    MB_ANDROID_USE_NDK)
else()
  # 宿主机上没有 <android/log.h>。src/android/host/ 提供**仅用于测试编译**的替身；
  # 真实 NDK 路径（ANativeWindow + MB_ANDROID_USE_NDK）仍然只在 Android 构建里启用。
  target_include_directories(magicblocks_android_scene PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/android/host)
  target_compile_definitions(magicblocks_android_scene PRIVATE
    MB_ANDROID_HOST_STUB=1)
endif()

if(MB_BUILD_TESTS)

# --- 测试 ---
enable_testing()
include(GoogleTest)

add_executable(magicblocks_test
  tests/magicblocks_test.cpp
  tests/magicblocks_l1_test.cpp
  tests/ui_test.cpp
  tests/magicblocks_l2_test.cpp
  tests/magicblocks_l3_test.cpp
  tests/magicblocks_l3_pool_test.cpp
  tests/magicblocks_l3_batch_test.cpp
  tests/magicblocks_l3_world_test.cpp
  tests/magicblocks_l3_mesh_test.cpp
  tests/magicblocks_l3_painter_test.cpp
  tests/magicblocks_l3_renderer_test.cpp
  # 2026-11-11：L3 稳态零分配用例（R-SK-11 / A11）。
  # 钉住 draw_mesh_instanced 在暖机后连续 30 帧的 vma_alloc/vma_free 增量为 0。
  tests/magicblocks_l3_steady_state_test.cpp
  tests/magicblocks_l3_pixel_regression_test.cpp
  tests/magicblocks_l3_m3_test.cpp
  tests/magicblocks_l3_loader_test.cpp
  tests/magicblocks_l3_bvh_test.cpp
  tests/magicblocks_l3_outline_test.cpp
  tests/magicblocks_l3_pathstyle_test.cpp
  tests/magicblocks_l3_stroke_test.cpp
  # 2026-11-11：L3 方向背景(sky)通道 + L1 立方体贴图的像素级验收
  tests/magicblocks_l3_sky_test.cpp
  # 2026-11-11：L3 预设测试模型目录（扫描/加载/归一化/上传/失败语义）
  tests/magicblocks_l3_model_catalog_test.cpp
  tests/magicblocks_l4_sketch_test.cpp
  tests/magicblocks_l4_sketch_canvas_test.cpp
  tests/magicblocks_l4_stack_test.cpp
  tests/magicblocks_l4_stack_session_test.cpp
  tests/magicblocks_l4_stack_golden_test.cpp
  tests/magicblocks_l4_stack_lock_camera_test.cpp
  tests/magicblocks_l4_stack_command_test.cpp
  tests/magicblocks_msaa_test.cpp
  # 2026-11-11：像素级 MSAA 回归（真实光栅 + 真实提交 + 真实读回）。
  # 与上面的能力自证测试分工不同：那份验"caps 报得对不对"，这份验"像素软不软"。
  tests/magicblocks_msaa_pixel_test.cpp
  # backend 内的 MSAA 能力自证测试:它必须直接调 vkGetPhysicalDeviceImageFormatProperties
  # 才能验到"caps 报的采样等级是不是真的能用"(8x 静默降级的守门人),
  # 而 Vk* 类型只允许出现在 src/backend/**(G3),所以它住在 src 下。
  src/backend/vulkan/msaa_render_pass_test.cpp
  # 2026-11-12：Android 场景渲染器的两条顶点布局契约。
  # “36 字节步长读 48 字节模型数据 → 三角面片全炸”编译期看不出来、运行期
  # 也不报错，只能靠断言钉住（详见测试文件头注释）。
  tests/magicblocks_android_scene_renderer_test.cpp)
target_link_libraries(magicblocks_test PRIVATE magicblocks magicblocks_android_scene GTest::gtest GTest::gtest_main)
target_include_directories(magicblocks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_BINARY_DIR})
if(WIN32)
  target_compile_definitions(magicblocks_test PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks_test PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()
target_compile_definitions(magicblocks_test PRIVATE
  MB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
  MB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
  MB_CLI_PATH="$<TARGET_FILE:magicblocks-l0-cli>"
  MB_UI_CLI_PATH="$<TARGET_FILE:magicblocks-ui-cli>"
  MB_L3_CLI_PATH="$<TARGET_FILE:magicblocks-l3-cli>"
  # F-SK 自己的 CLI（render/replay/reproduce/dump）。B2 的 golden 比对、
  # A7/A8 的三态与回放像素一致都靠它，不能用 l3-cli 的 sketch 子命令替代。
  MB_FSK_CLI_PATH="$<TARGET_FILE:magicblocks-fsk-cli>"
  # 阶段 7 的 StackGates.* 用例要在 gtest 里直接跑门禁脚本，
  # 所以把解释器与仓根一并传进去（不靠 PATH 里的 python 名字）。
  MB_PYTHON_EXE="${Python3_EXECUTABLE}"
  MB_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
  MB_MINE_ROOT="${MINE_ROOT}")
gtest_discover_tests(magicblocks_test
  PROPERTIES RESOURCE_LOCK vulkan)
endif()

# --- 门控脚本(spec §7):G1/G3/G4 + 依赖回归 + 门控自测 ---
# 全部走解析后路径,不依赖 PATH 里的 python 名字。
set(MB_PY ${Python3_EXECUTABLE})
add_test(NAME magicblocks_gate_g1_no_geometry_shader
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_no_geometry_shader.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_g3_layer_purity
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_layer_purity.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_g4_no_raw_alloc
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_no_raw_alloc.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_g7_l3_no_gameplay_names
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_l3_no_gameplay_names.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_g8_l3_command_parity
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_l3_command_parity.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_deps_tests
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_deps_tests.py)
add_test(NAME magicblocks_gate_scripts_selftest
  COMMAND ${MB_PY} -m unittest discover -s ${MINE_ROOT}/tools/tests -p "test_check_scripts.py")

# --- UI 消费层门控(spec §5.4 / §10) ---
add_test(NAME magicblocks_gate_uig1_no_readback
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_ui_no_readback.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_uig2_single_submit
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_ui_single_submit.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_ui_scripts_selftest
  COMMAND ${MB_PY} -m unittest discover -s ${MINE_ROOT}/tools/tests -p "test_ui_check_scripts.py")

# --- CLI 级 CTest:端到端(无窗口离屏) ---
set(MB_CLI_OUT "${CMAKE_CURRENT_BINARY_DIR}/cli_out")
file(MAKE_DIRECTORY ${MB_CLI_OUT})
add_test(NAME magicblocks_cli_device
  COMMAND magicblocks-l0-cli device)
add_test(NAME magicblocks_cli_render_png
  COMMAND magicblocks-l0-cli render --width 320 --height 240 --frame-index 0
          --output ${MB_CLI_OUT}/out.png)

# --- CLI 级 CTest:UI 消费层(spec §10 门 1~5 的端到端旁证) ---
#
# ⚠️ 这三条必须显式给 WORKING_DIRECTORY = 源码根目录。
# ui-cli 在没有 --asset-root 时按 "assets/res" 兜底扫模型目录(见
# src/ui_cli/main.cpp 的资源根推导)。ctest 默认把 CWD 设在 build 目录,
# 那里没有 assets/,于是报 "目录不存在: assets/res/models" 并失败 ——
# 而同一个 exe 手工在仓库根跑是好的。不设 WORKING_DIRECTORY 会让这三条
# 永远红着,且看上去像 UI 功能坏了。
add_test(NAME magicblocks_cli_ui_render_png
  COMMAND magicblocks-ui-cli render --width 320 --height 240
          --output ${MB_CLI_OUT}/ui.png)
add_test(NAME magicblocks_cli_ui_stats
  COMMAND magicblocks-ui-cli stats --width 320 --height 240 --frames 3)
add_test(NAME magicblocks_cli_ui_present
  COMMAND magicblocks-ui-cli present --width 320 --height 240 --frames 5)
set_tests_properties(magicblocks_cli_ui_render_png magicblocks_cli_ui_stats
                     magicblocks_cli_ui_present
  PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
# CLI 旋钮必须真的起作用:未知参数 → 退出码 2(不静默忽略)
add_test(NAME magicblocks_cli_ui_bad_arg
  COMMAND magicblocks-ui-cli render --bogus 1)
set_tests_properties(magicblocks_cli_ui_bad_arg PROPERTIES WILL_FAIL TRUE)

# --- CLI 级 CTest:L2 场景层录制/回放/复现(spec §5 命令模式,plans-l2 §6.4) ---
# `record` 同时产出命令流(--record,二进制)与离屏 PNG(--output)。
# 后续三条用例都读这条流,因此用 FIXTURES 把它们排到 record 之后。
set(MB_L2_STREAM "${MB_CLI_OUT}/l2_scene.mbl2")
set(MB_L2_PNG "${MB_CLI_OUT}/l2_scene.png")

add_test(NAME magicblocks_l2_record
  COMMAND magicblocks-l2-cli record --scene demo --width 320 --height 240
          --frame-index 0 --record ${MB_L2_STREAM} --output ${MB_L2_PNG})
set_tests_properties(magicblocks_l2_record PROPERTIES FIXTURES_SETUP mb_l2_stream)

add_test(NAME magicblocks_l2_replay_verify
  COMMAND magicblocks-l2-cli replay --replay ${MB_L2_STREAM} --verify)
set_tests_properties(magicblocks_l2_replay_verify PROPERTIES
  FIXTURES_REQUIRED mb_l2_stream)

add_test(NAME magicblocks_l2_reproduce_verify
  COMMAND magicblocks-l2-cli reproduce --replay ${MB_L2_STREAM} --verify)
set_tests_properties(magicblocks_l2_reproduce_verify PROPERTIES
  FIXTURES_REQUIRED mb_l2_stream)

# dump 必须给出稳定的 JSON(含 format_version / backend / hash)
add_test(NAME magicblocks_l2_dump_json
  COMMAND magicblocks-l2-cli dump --replay ${MB_L2_STREAM} --json)
set_tests_properties(magicblocks_l2_dump_json PROPERTIES
  FIXTURES_REQUIRED mb_l2_stream)

# 读不存在的流 → 退出码 1(不是 0,也不是崩溃)
add_test(NAME magicblocks_l2_replay_missing_cap
  COMMAND magicblocks-l2-cli replay --replay ${MB_CLI_OUT}/does_not_exist.mbl2)
set_tests_properties(magicblocks_l2_replay_missing_cap PROPERTIES
  PASS_REGULAR_EXPRESSION "无法打开文件")

# 玩法预设属 L4:L2 的 --scene 只认 demo,其它必须显式拒绝(退出码 2)
add_test(NAME magicblocks_l2_scene_preset_rejected
  COMMAND magicblocks-l2-cli record --scene voxel --output ${MB_CLI_OUT}/l2_voxel.png)
set_tests_properties(magicblocks_l2_scene_preset_rejected PROPERTIES
  PASS_REGULAR_EXPRESSION "L4")

# 未知参数 → 退出码 2(不静默忽略)
add_test(NAME magicblocks_l2_cli_bad_arg
  COMMAND magicblocks-l2-cli record --bogus 1)
set_tests_properties(magicblocks_l2_cli_bad_arg PROPERTIES WILL_FAIL TRUE)


