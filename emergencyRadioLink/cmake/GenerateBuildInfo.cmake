# Run at build time (not configure time) by an always-run custom target in CMakeLists.txt,
# so this actually executes once per `cmake --build`/`ninja`, not just once per `cmake` reconfigure.
#
# Bumps the persistent build counter in BUILD_NUMBER_FILE and (re)writes OUTPUT_FILE
# (build_info.c). __DATE__/__TIME__ are left as literal, unexpanded macro tokens in the
# generated source: the C compiler fills them in when it compiles this file, which happens
# on every build because this script always rewrites it (giving it a fresh mtime), so they
# end up reflecting this build rather than whenever build_info.c last happened to change.

if(NOT DEFINED BUILD_NUMBER_FILE)
    message(FATAL_ERROR "GenerateBuildInfo.cmake: BUILD_NUMBER_FILE not set")
endif()
if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "GenerateBuildInfo.cmake: OUTPUT_FILE not set")
endif()
if(NOT DEFINED CONFIG_DEVICE_MAJOR_VERSION OR NOT DEFINED CONFIG_DEVICE_MINOR_VERSION)
    message(FATAL_ERROR "GenerateBuildInfo.cmake: CONFIG_DEVICE_MAJOR_VERSION/MINOR_VERSION not set")
endif()

set(build_number 0)
if(EXISTS "${BUILD_NUMBER_FILE}")
    file(READ "${BUILD_NUMBER_FILE}" build_number)
    string(STRIP "${build_number}" build_number)
endif()
math(EXPR build_number "${build_number} + 1")
file(WRITE "${BUILD_NUMBER_FILE}" "${build_number}\n")

file(WRITE "${OUTPUT_FILE}"
"// Auto-generated on every build by cmake/GenerateBuildInfo.cmake. Do not edit by hand.
#include \"build_info.h\"

const char* const g_build_date = __DATE__;
const char* const g_build_time = __TIME__;
const uint32_t g_build_number = ${build_number}u;
")

# Same format as __DATE__/__TIME__ (e.g. "Sep 27 2026", "14:23:01"), computed by CMake's own
# clock purely for this message; the values actually baked into build_info.c above come from
# the C compiler's __DATE__/__TIME__ when it compiles this file right after this script runs,
# so the two can differ by at most a moment (CMake's %d is zero-padded, __DATE__'s is space-padded).
string(TIMESTAMP display_date "%b %d %Y")
string(TIMESTAMP display_time "%H:%M:%S")

string(ASCII 27 esc)
message("${esc}[1m${esc}[32m-- [BUILD INFO]: v${CONFIG_DEVICE_MAJOR_VERSION}.${CONFIG_DEVICE_MINOR_VERSION}, "
        "build #${build_number}, ${display_date} ${display_time}${esc}[m")
