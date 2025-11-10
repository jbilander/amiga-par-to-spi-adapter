cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w)
set(PICO_SDK_FETCH_FROM_GIT on)
include(pico_sdk_import.cmake)
set(PROJECT par_spi_ftp_server)
project(${PROJECT} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_compile_options(-Wall) 

# === FatFs library ===
add_library(fatfs
    lib/fatfs/source/ff.c
    lib/fatfs/source/ffsystem.c
    lib/fatfs/source/ffunicode.c
    lib/fatfs/source/diskio.c
)

target_include_directories(fatfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/source
    ${CMAKE_CURRENT_SOURCE_DIR}/build/generated/pico_base
    include_directories(${PICO_SDK_PATH}/src/common/pico_sync/include)
    include_directories(${PICO_SDK_PATH}/src/common/pico_base/include)
    include_directories(${PICO_SDK_PATH}/src/common/pico_base_headers/include)
    include_directories(${PICO_SDK_PATH}/src/common/pico_stdlib_headers/include)
    include_directories(${PICO_SDK_PATH}/src/rp2350/pico_platform/include)
    include_directories(${PICO_SDK_PATH}/src/rp2350/hardware_regs/include)
    include_directories(${PICO_SDK_PATH}/src/rp2350/hardware_structs/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/pico_platform_compiler/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/pico_platform_sections/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/pico_platform_panic/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/pico_platform_common/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_timer/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_base/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_sync/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_sync_spin_lock/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_spi/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_gpio/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_irq/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/pico_stdio/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_uart/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_resets/include)
    include_directories(${PICO_SDK_PATH}/src/rp2_common/hardware_rtc/include)
    include_directories(${PICO_SDK_PATH}/src/common/pico_time/include)
)

add_executable(${PROJECT} ftp_server.c par_spi.c main.c)

pico_add_extra_outputs(${PROJECT})
pico_enable_stdio_usb(par_spi_ftp_server 1)
pico_enable_stdio_uart(par_spi_ftp_server 0)

target_link_libraries(${PROJECT} pico_stdlib hardware_spi hardware_sync pico_multicore fatfs pico_util)

if (PICO_CYW43_SUPPORTED)
	target_link_libraries(${PROJECT} pico_cyw43_arch_none)
endif()
