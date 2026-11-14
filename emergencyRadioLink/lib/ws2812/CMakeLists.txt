add_library(ws2812 STATIC
        src/ws2812.c
)

pico_generate_pio_header(ws2812 ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

target_include_directories(ws2812 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ws2812 PUBLIC
        hardware_pio
        hardware_clocks
)
