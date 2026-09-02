cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(main WIN32
    src/main.cpp
    src/focus_timer_widget.h
    src/focus_timer_widget.cpp
    src/frameless_window.h
    src/frameless_window.cpp
    src/settings_dialog.h
    src/settings_dialog.cpp
    src/plant/seed.h
    src/plant/plant_scene.h
    src/plant/plant_scene.cpp
    src/plant/painter_plant_renderer.h
    src/plant/painter_plant_renderer.cpp
)

target_include_directories(main PRIVATE src)
target_link_libraries(main PRIVATE Qt6::Widgets)

# Visual Studio 调试时自动把 Qt 的 bin 目录加入 PATH，避免提示缺少 Qt DLL
if(WIN32 AND MSVC)
    set_property(TARGET main PROPERTY VS_DEBUGGER_ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$(PATH)")
endif()

# 构建树里 F5/双击即用:POST_BUILD 用 windeployqt 把 Qt DLL + 平台插件拷到 exe 旁,不依赖 PATH。
# (install 期那处 windeployqt 只管 cmake --install 的产物,管不到 VS 直接运行的构建树 exe。)
if(WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/qt_windeploy.cmake")
    qt_windeploy(main)
endif()

# Vulkan 为可选：有 SDK（且能编 shader）则启用离屏渲染，没有也不要导致配置/编译失败
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    find_program(GLSLANG_VALIDATOR
        NAMES glslangValidator glslangValidator.exe
        HINTS
            "$ENV{VULKAN_SDK}/Bin"
            "$ENV{VULKAN_SDK}/bin"
            "${Vulkan_INCLUDE_DIR}/../Bin"
            "${Vulkan_INCLUDE_DIR}/../bin"
    )
    if(GLSLANG_VALIDATOR)
        message(STATUS "Vulkan found: ${Vulkan_LIBRARY}")
        message(STATUS "glslangValidator: ${GLSLANG_VALIDATOR}")
        target_compile_definitions(main PRIVATE HAS_VULKAN)
        target_link_libraries(main PRIVATE Vulkan::Vulkan)
        target_sources(main PRIVATE
            src/plant/vulkan_plant_renderer.h
            src/plant/vulkan_plant_renderer.cpp
        )

        set(_spirv_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
        file(MAKE_DIRECTORY "${_spirv_dir}")
        file(WRITE "${_spirv_dir}/plant_shaders.qrc"
"<RCC>
  <qresource prefix=\"/shaders\">
    <file alias=\"plant.vert.spv\">plant.vert.spv</file>
    <file alias=\"plant.frag.spv\">plant.frag.spv</file>
  </qresource>
</RCC>
")

        add_custom_command(
            OUTPUT "${_spirv_dir}/plant.vert.spv"
            COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/plant.vert"
                    -o "${_spirv_dir}/plant.vert.spv"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/plant.vert"
            COMMENT "Compiling plant.vert to SPIR-V"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${_spirv_dir}/plant.frag.spv"
            COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/plant.frag"
                    -o "${_spirv_dir}/plant.frag.spv"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/plant.frag"
            COMMENT "Compiling plant.frag to SPIR-V"
            VERBATIM
        )
        add_custom_target(plant_shaders DEPENDS
            "${_spirv_dir}/plant.vert.spv"
            "${_spirv_dir}/plant.frag.spv"
        )
        # rcc 必须在 SPIR-V 生成之后运行
        add_dependencies(main plant_shaders)
        set_source_files_properties("${_spirv_dir}/plant_shaders.qrc" PROPERTIES
            OBJECT_DEPENDS "${_spirv_dir}/plant.vert.spv;${_spirv_dir}/plant.frag.spv"
        )
        target_sources(main PRIVATE "${_spirv_dir}/plant_shaders.qrc")
    else()
        message(STATUS "Vulkan found but glslangValidator missing; building without HAS_VULKAN")
    endif()
else()
    message(STATUS "Vulkan SDK not found; building without HAS_VULKAN")
endif()

# 安装到前缀根目录，使验收能找到 _install/main.exe
install(TARGETS main RUNTIME DESTINATION .)

# Windows：安装后用 windeployqt 拷贝 Qt 运行依赖
if(WIN32)
    get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    cmake_path(GET _qmake_executable PARENT_PATH _qt_bin_dir)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}" REQUIRED)

    install(CODE "
        set(_exe \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/main.exe\")
        message(STATUS \"Running windeployqt on \${_exe}\")
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" --release --no-compiler-runtime --no-translations \"\${_exe}\"
            RESULT_VARIABLE _wd_result
        )
        if(NOT _wd_result EQUAL 0)
            message(FATAL_ERROR \"windeployqt failed with code \${_wd_result}\")
        endif()
    ")
endif()
