#ifndef EMERGENCYRADIOLINK_BUILD_INFO_H
#define EMERGENCYRADIOLINK_BUILD_INFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Defined in the generated build_info.c (see CMakeLists.txt / cmake/GenerateBuildInfo.cmake),
// which is regenerated and recompiled on every build, so these always reflect that build,
// not whenever build_info.c last happened to change.

/** @brief Compiler's __DATE__ from the last build, e.g. "Sep 27 2026" (fixed width, NUL-terminated). */
extern const char* const g_build_date;

/** @brief Compiler's __TIME__ from the last build, e.g. "14:23:01" (fixed width, NUL-terminated). */
extern const char* const g_build_time;

/** @brief Build counter, persisted in .build_number at the project root, incremented once per build. */
extern const uint32_t g_build_number;

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_BUILD_INFO_H
