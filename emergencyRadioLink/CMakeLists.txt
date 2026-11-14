cmake_minimum_required(VERSION 3.25)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(PICO_SDK_PATH "$ENV{HOME}/pico-sdk")
set(PROJECT "emergencyRadioLink")

file(GLOB SRC_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol/*.c
)

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
project(${PROJECT} C CXX ASM)

pico_sdk_init()

add_subdirectory(lib)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib/RadioLib" "${CMAKE_CURRENT_BINARY_DIR}/RadioLib")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib/merl" "${CMAKE_CURRENT_BINARY_DIR}/merl")

add_compile_options(-O2)
add_compile_options(
        -Wall
        -Wno-format
        -Wno-unused-function
)

set(BUILD_INFO_SRC ${CMAKE_CURRENT_BINARY_DIR}/build_info.c)

add_executable(${PROJECT}
        ${SRC_FILES}
        ${SRC_PROTOCOL_FILES}
        ${BUILD_INFO_SRC}
        "${CMAKE_CURRENT_SOURCE_DIR}/../lib/RadioLib/src/hal/RPiPico/PicoHal.cpp"
)
target_compile_definitions(${PROJECT_NAME} PUBLIC RADIOLIB_BUILD_RPI_PICO)

add_custom_target(generate_build_info ALL
        COMMAND ${CMAKE_COMMAND}
                -DBUILD_NUMBER_FILE=${CMAKE_CURRENT_SOURCE_DIR}/.build_number
                -DOUTPUT_FILE=${BUILD_INFO_SRC}
                -DCONFIG_DEVICE_MAJOR_VERSION=${CONFIG_DEVICE_MAJOR_VERSION}
                -DCONFIG_DEVICE_MINOR_VERSION=${CONFIG_DEVICE_MINOR_VERSION}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake
        BYPRODUCTS ${BUILD_INFO_SRC}
        COMMENT "Bumping build number and regenerating build_info.c"
)
add_dependencies(${PROJECT} generate_build_info)

get_cmake_property(cache_vars CACHE_VARIABLES)


# get all config params and define them
string(ASCII 27 Esc)
set(ColorReset  "${Esc}[m")
set(ColorBold   "${Esc}[1m")
set(Red         "${Esc}[31m")
set(Green       "${Esc}[32m")
get_cmake_property(cache_vars CACHE_VARIABLES)
foreach(var ${cache_vars})
    if(var STREQUAL "CONFIG_DEVICE_FEATURES")
        get_property(features CACHE ${var} PROPERTY VALUE)
        string(REPLACE "," " " features "${features}")
        separate_arguments(features_list UNIX_COMMAND "${features}")
        foreach(feature ${features_list})
            target_compile_definitions(${PROJECT} PRIVATE ${feature})
            message("${ColorBold}${Green}-- [FEATURE]: ${feature} is compiled in${ColorReset}")
        endforeach()
    elseif(var MATCHES "^CONFIG_")
        get_property(var_value CACHE ${var} PROPERTY VALUE)

        string(TOLOWER "${var_value}" var_value_lower)
        if(var_value_lower STREQUAL "off")
            message("${ColorBold}${Red}-- [AUTO-DEFINE]: ${var} is disabled!${ColorReset}")
        elseif(var_value_lower STREQUAL "on")
            target_compile_definitions(${PROJECT} PRIVATE ${var})
            message("${ColorBold}${Green}-- [AUTO-DEFINE]: ${var} is enabled${ColorReset}")
        else()
            target_compile_definitions(${PROJECT} PRIVATE ${var}=${var_value})
            message(STATUS "[AUTO-DEFINE]: ${var} = ${var_value}")
        endif()
    endif()
endforeach()

target_include_directories(${PROJECT} PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/include/protocol
)
target_link_libraries(${PROJECT}
        pico_stdlib
        pico_util
        pico_rand
        hardware_dma
        hardware_uart
        hardware_spi
        hardware_irq
        pico_multicore
        hardware_flash
        pico_flash
        hardware_pwm
        hardware_watchdog
        RadioLib
        ublox_gps
        ws2812
        qmc5883
        merl
)

pico_add_extra_outputs(${PROJECT})

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(DEBUG_MODE)
    # stdio over USB only: the default stdio UART (uart0, GP0/GP1) is UART_MAIN
    pico_enable_stdio_usb(${PROJECT} 1)
    pico_enable_stdio_uart(${PROJECT} 0)
endif()
