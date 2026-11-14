add_library(INA226 INA226.c)

target_include_directories(INA226 PUBLIC .)

# The driver itself is portable; the platform provides the register/timing hooks
# declared in INA226.h. Only the Pico SDK build pulls in the SDK libraries.
if (TARGET pico_stdlib)
    target_link_libraries(INA226
            pico_stdlib
            hardware_pio
    )
else ()
    target_link_libraries(INA226 m)
endif ()
