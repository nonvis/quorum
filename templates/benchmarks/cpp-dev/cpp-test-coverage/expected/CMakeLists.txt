cmake_minimum_required(VERSION 3.16)
project(cpp_test_coverage CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

add_library(name_registry src/name_registry.cpp)
target_include_directories(name_registry PUBLIC include)

enable_testing()
add_executable(test_name_registry tests/test_name_registry.cpp)
target_link_libraries(test_name_registry PRIVATE name_registry)
add_test(NAME test_name_registry COMMAND test_name_registry)
