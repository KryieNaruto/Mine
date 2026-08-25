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
