# Cross toolchain for the Luckfox Lyra (RK3506, armhf), from the Lyra SDK
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

get_filename_component(LYRA_TOOLCHAIN
        "${CMAKE_CURRENT_LIST_DIR}/../../Lyra-sdk/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin"
        ABSOLUTE)

set(CMAKE_C_COMPILER "${LYRA_TOOLCHAIN}/arm-none-linux-gnueabihf-gcc")
set(CMAKE_STRIP "${LYRA_TOOLCHAIN}/arm-none-linux-gnueabihf-strip")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
