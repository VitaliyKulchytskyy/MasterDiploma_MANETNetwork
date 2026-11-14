cmake_minimum_required(VERSION 3.13)

# Main unit (Luckfox Lyra) software.
# Cross build for the board:  cmake -B build/lyra -DCMAKE_TOOLCHAIN_FILE=cmake/lyra.cmake
# Host build (tests):         cmake -B build/host
project(luckfoxLyra_mainPart C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib/merl" "${CMAKE_CURRENT_BINARY_DIR}/merl")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib/INA226" "${CMAKE_CURRENT_BINARY_DIR}/INA226")

# SQLite (chat history): the board's library + headers in sysroot/ for the cross build,
# the system one on the host
if (CMAKE_CROSSCOMPILING)
    add_library(sqlite3 SHARED IMPORTED)
    set_target_properties(sqlite3 PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/lib/libsqlite3.so"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/include"
            # the board's libsqlite3 needs glibc 2.34+ symbols; the board has them (2.35),
            # the SDK toolchain's sysroot (2.33) does not, so leave them to the runtime linker
            INTERFACE_LINK_OPTIONS "-Wl,--allow-shlib-undefined")
else ()
    find_package(SQLite3 REQUIRED)
    add_library(sqlite3 ALIAS SQLite::SQLite3)
endif ()

add_executable(merld merld/merld.c merld/chat.c merld/buf.c merld/alert.c)
target_link_libraries(merld merl sqlite3 m)

add_executable(merlctl merld/merlctl.c)

add_executable(ina226d ina226d/ina226d.c ina226d/INA226_linux.c)
target_link_libraries(ina226d INA226)
