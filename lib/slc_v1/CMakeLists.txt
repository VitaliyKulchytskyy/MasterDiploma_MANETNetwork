#cmake_minimum_required(VERSION 3.25)
#
#set(VERSION_MAJOR 00)
#set(VERSION_MINOR 01)
#set(CMAKE_C_STANDARD 23)
#set(CMAKE_C_STANDARD_REQUIRED ON)
#set(CMAKE_C_EXTENSIONS OFF)
#set(PICO_SDK_PATH "$ENV{HOME}/dev/dependency/pico-sdk")
#set(PROJECT "slc_v1")
#
#file(GLOB SRC_FILES
#        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
#)
#
#include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
#project(${PROJECT} C CXX ASM)
#
#pico_sdk_init()
#
##add_subdirectory(lib)
#add_compile_options(-O2)
#
#add_executable(${PROJECT}
#        ${SRC_FILES}
#        ${SRC_PROTOCOL_FILES}
#)
#
#get_cmake_property(cache_vars CACHE_VARIABLES)
#
#
#if(CALIBRATION_MODE)
#    add_compile_definitions(CALIBRATION_MODE)
#endif()
#
#if(READ_CONFIG_FROM_FLASH)
#    add_compile_definitions(READ_CONFIG_FROM_FLASH)
#endif()
#
## get all config params and define them
#foreach(var ${cache_vars})
#    if(var MATCHES "^CONFIG_")
#        get_property(var_value CACHE ${var} PROPERTY VALUE)
#        target_compile_definitions(${PROJECT} PRIVATE ${var}=${var_value})
#        message(STATUS "[AUTO-DEFINE] ${var} = ${var_value}")
#    endif()
#endforeach()
#
#target_include_directories(${PROJECT} PUBLIC
#        ${CMAKE_CURRENT_LIST_DIR}/include
#)
#target_link_libraries(${PROJECT}
#        pico_stdlib
#        pico_util
##        hardware_pio
##        hardware_dma
#        hardware_irq
#        hardware_adc
#        hardware_i2c
##        hardware_flash
##        hardware_pwm
#)
#
#pico_add_extra_outputs(${PROJECT})
#
#if (CMAKE_BUILD_TYPE STREQUAL "Debug")
#    pico_enable_stdio_usb(${PROJECT} 1)
#    pico_enable_stdio_uart(${PROJECT} 1)
#endif()

cmake_minimum_required(VERSION 3.25)
set(PROJECT "slc_v1")
set(CMAKE_C_STANDARD 23)
project(${PROJECT} C)

file(GLOB SRC_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)

#pkg_check_modules(GLIB REQUIRED glib-2.0)

add_executable(${PROJECT} ${SRC_FILES})
target_include_directories(${PROJECT} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(${PROJECT} PRIVATE ${GLIB_INCLUDE_DIRS})
target_link_libraries(${PROJECT} PRIVATE ${GLIB_LIBRARIES})
