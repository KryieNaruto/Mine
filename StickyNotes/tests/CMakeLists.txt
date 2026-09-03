find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(store_test store_test.cpp)
target_link_libraries(store_test PRIVATE stickynotes_core Qt6::Test)
add_test(NAME store_test COMMAND store_test)

add_executable(persistence_test persistence_test.cpp)
target_link_libraries(persistence_test PRIVATE stickynotes_core Qt6::Test)
add_test(NAME persistence_test COMMAND persistence_test)

add_executable(palette_test palette_test.cpp)
target_link_libraries(palette_test PRIVATE stickynotes_core Qt6::Test)
add_test(NAME palette_test COMMAND palette_test)

add_executable(geometry_test geometry_test.cpp)
target_link_libraries(geometry_test PRIVATE stickynotes_core Qt6::Test)
add_test(NAME geometry_test COMMAND geometry_test)

add_executable(widget_test widget_test.cpp)
target_link_libraries(widget_test PRIVATE stickynotes_app Qt6::Test Qt6::Widgets)
add_test(NAME widget_test COMMAND widget_test)

add_executable(startup_test startup_test.cpp)
target_link_libraries(startup_test PRIVATE stickynotes_app Qt6::Test Qt6::Widgets)
add_test(NAME startup_test COMMAND startup_test)

add_executable(group_test group_test.cpp)
target_link_libraries(group_test PRIVATE stickynotes_app Qt6::Test Qt6::Widgets)
add_test(NAME group_test COMMAND group_test)

add_executable(image_golden_test image_golden_test.cpp)
target_link_libraries(image_golden_test PRIVATE stickynotes_core Qt6::Test)
target_compile_definitions(image_golden_test PRIVATE
  STICKYNOTES_CLI="$<TARGET_FILE:stickynotes-cli>"
  STICKYNOTES_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../golden")
add_test(NAME image_golden_test COMMAND image_golden_test)

# Qt 测试 exe 同样部署运行库(qt_windeploy 由父目录 tools/cmake/qt_windeploy.cmake 提供)
if(WIN32)
  get_property(_st_tgts DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_t ${_st_tgts})
    get_target_property(_tt "${_t}" TYPE)
    if(_tt STREQUAL "EXECUTABLE")
      qt_windeploy("${_t}")
    endif()
  endforeach()
endif()
