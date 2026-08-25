find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(store_test store_test.cpp)
target_link_libraries(store_test PRIVATE stickynotes_core Qt6::Test)
add_test(NAME store_test COMMAND store_test)
