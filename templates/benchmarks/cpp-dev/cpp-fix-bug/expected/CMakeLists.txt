cmake_minimum_required(VERSION 3.16)
project(cpp_fix_bug CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

add_library(vault src/vault.cpp)
target_include_directories(vault PUBLIC include)

enable_testing()
add_executable(test_vault tests/test_vault.cpp)
target_link_libraries(test_vault PRIVATE vault)
add_test(NAME test_vault COMMAND test_vault)
