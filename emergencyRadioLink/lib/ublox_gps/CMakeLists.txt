add_library(ublox_gps STATIC
        src/ublox_gps.cpp
)

target_include_directories(ublox_gps PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

message("UBLOX GPS added to the build")