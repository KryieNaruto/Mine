cmake_minimum_required(VERSION 3.22)
project(MagicBlocks CXX)

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
# 池里 googletest 导出的是 GTest CONFIG 包,target 为 GTest::gtest / GTest::gtest_main
find_package(GTest CONFIG REQUIRED ${_pool_only})
find_package(Vulkan REQUIRED)
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
set(MB_SHADER_NAMES fullscreen.vert fullscreen.frag ui_overlay.vert ui_overlay.frag l2_scene.vert l2_scene.frag)
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
if(WIN32)
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
  include/magicblocks/ui/presenter.h
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
  src/backend/vulkan/vulkan_pipeline.h
  src/backend/vulkan/vulkan_pipeline.cpp
  src/backend/vulkan/vulkan_command.h
  src/backend/vulkan/vulkan_command.cpp
  src/backend/vulkan/vulkan_frame_sync.h
  src/backend/vulkan/vulkan_frame_sync.cpp
  src/backend/vulkan/vulkan_swapchain.h
  src/backend/vulkan/vulkan_swapchain.cpp
  src/backend/vulkan/vulkan_swapchain_android.cpp
  src/backend/vulkan/vulkan_swapchain_android.h
  src/backend/vulkan/vulkan_draw.h
  src/backend/vulkan/vulkan_draw.cpp
  src/backend/vulkan/vulkan_render_device.h
  src/backend/vulkan/vulkan_render_device.cpp
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
  src/ui/presenter.cpp
  src/ui/presenter_host.h
  src/ui/presenter_host.cpp
  src/ui/presenter_offscreen.cpp
  src/ui/offscreen_target.h
  src/ui/ui_app_internal.h
  src/ui/ui_app.cpp
  ${MB_SURFACE_SRC}
  src/core/device_factory.cpp
  src/core/image_io.h
  src/core/image_io.cpp
  ${MB_SHADER_HEADER})
target_include_directories(magicblocks PUBLIC include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${MB_VMA_INCLUDE})
target_link_libraries(magicblocks PUBLIC Vulkan::Vulkan)

# 平台 surface:VK_USE_PLATFORM_* 决定实例层追加哪个 surface 扩展
if(WIN32)
  target_compile_definitions(magicblocks PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(magicblocks PRIVATE VK_USE_PLATFORM_XCB_KHR)
else()
  message(FATAL_ERROR "MagicBlocks 本轮只支持 Windows 与 Linux(Android 走 ABI 编译校验;iOS 只做设计考虑)")
endif()

# --- Android TU 编译校验(A7 / Swapchain.AndroidTuCompiles) ---
# vulkan_swapchain_android.cpp **始终**参与 magicblocks 的编译(见上面的源码列表),
# 因此在任何平台上 "编译过" == "Android 分支的 ABI 形状是对的":
#   - 有 NDK(-DMB_ANDROID_USE_NDK=ON 或 ANDROID 平台)→ 真实 <android/native_window.h>
#   - 无 NDK → src/backend/vulkan/stub/anativewindow.h 提供 ANativeWindow 前置声明
# 不需要额外的 object 目标:该 TU 已经在库里,编译失败会直接让整个 SDK 构建失败。
option(MB_ANDROID_TU_COMPILE_CHECK "断言 vulkan_swapchain_android.cpp 参与编译校验" ON)
if(NOT MB_ANDROID_TU_COMPILE_CHECK)
  message(FATAL_ERROR "MB_ANDROID_TU_COMPILE_CHECK=OFF 会让 Android 分支失去编译校验(违反 A7),本轮不允许关闭")
endif()

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
add_executable(magicblocks-ui-cli src/ui_cli/main.cpp)
target_link_libraries(magicblocks-ui-cli PRIVATE magicblocks)
target_include_directories(magicblocks-ui-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
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
# --- 临时探针(用完删除)---
add_executable(mb_rt_probe build/rt_probe.cpp)
target_link_libraries(mb_rt_probe PRIVATE magicblocks)
target_include_directories(mb_rt_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})

add_executable(mb_rec_probe build/rec_probe.cpp)
target_link_libraries(mb_rec_probe PRIVATE magicblocks)
target_include_directories(mb_rec_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})

# --- 测试 ---
enable_testing()
include(GoogleTest)

add_executable(magicblocks_test tests/magicblocks_test.cpp tests/magicblocks_l1_test.cpp tests/ui_test.cpp tests/magicblocks_l2_test.cpp)
target_link_libraries(magicblocks_test PRIVATE magicblocks GTest::gtest GTest::gtest_main)
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
  MB_UI_CLI_PATH="$<TARGET_FILE:magicblocks-ui-cli>")
gtest_discover_tests(magicblocks_test)

# --- 门控脚本(spec §7):G1/G3/G4 + 依赖回归 + 门控自测 ---
# 全部走解析后路径,不依赖 PATH 里的 python 名字。
set(MB_PY ${Python3_EXECUTABLE})
add_test(NAME magicblocks_gate_g1_no_geometry_shader
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_no_geometry_shader.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_g3_layer_purity
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_layer_purity.py --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME magicblocks_gate_g4_no_raw_alloc
  COMMAND ${MB_PY} ${MINE_ROOT}/tools/check_no_raw_alloc.py --root ${CMAKE_CURRENT_SOURCE_DIR})
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
add_test(NAME magicblocks_cli_ui_render_png
  COMMAND magicblocks-ui-cli render --width 320 --height 240
          --output ${MB_CLI_OUT}/ui.png)
add_test(NAME magicblocks_cli_ui_stats
  COMMAND magicblocks-ui-cli stats --width 320 --height 240 --frames 3)
add_test(NAME magicblocks_cli_ui_present
  COMMAND magicblocks-ui-cli present --width 320 --height 240 --frames 5)
# CLI 旋钮必须真的起作用:未知参数 → 退出码 2(不静默忽略)
add_test(NAME magicblocks_cli_ui_bad_arg
  COMMAND magicblocks-ui-cli render --bogus 1)
set_tests_properties(magicblocks_cli_ui_bad_arg PROPERTIES WILL_FAIL TRUE)
