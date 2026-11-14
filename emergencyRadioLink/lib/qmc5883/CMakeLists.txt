add_library(qmc5883 STATIC
        src/qmc5883.c
)

target_include_directories(qmc5883 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(qmc5883 PUBLIC
        hardware_i2c
        hardware_gpio
)
