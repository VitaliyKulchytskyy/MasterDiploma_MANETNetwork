# MERL protocol core: frame codec, parser, payload (de)serialization.
# Platform independent; used by the emergency unit firmware (RP2040) and the
# main unit software (Luckfox Lyra):
#   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib/merl" "${CMAKE_CURRENT_BINARY_DIR}/merl")
#   target_link_libraries(<target> merl)
add_library(merl STATIC src/merl.c)

target_include_directories(merl PUBLIC include)

# Host tests, only when configured on its own: cmake -S lib/merl -B build && cmake --build build && build/test_merl
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.13)
    project(merl C)
    add_executable(test_merl test/test_merl.c)
    target_compile_options(test_merl PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_merl merl)
endif ()
