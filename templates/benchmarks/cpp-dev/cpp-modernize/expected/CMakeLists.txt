cmake_minimum_required(VERSION 3.16)
project(cpp_modernize CXX)

# Starter is pinned to C++14; part of this task is bumping it to C++20.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

add_library(registry src/registry.cpp)
target_include_directories(registry PUBLIC include)

enable_testing()
add_executable(test_registry tests/test_registry.cpp)
target_link_libraries(test_registry PRIVATE registry)
add_test(NAME test_registry COMMAND test_registry)
