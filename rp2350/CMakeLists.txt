cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)
set(PICO_SDK_FETCH_FROM_GIT on)
set(PICO_USE_CMSIS 1)

include(pico_sdk_import.cmake)

set(PROJECT par_spi_ftp_server)
project(${PROJECT} C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_compile_options(-Wall)

# === Fetch FreeRTOS Kernel (download-only; do NOT run its CMakeLists.txt) ===
include(FetchContent)

FetchContent_Declare(
    freertos
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG main
)

# Only download files; DO NOT process FreeRTOS CMakeLists
FetchContent_Populate(freertos)

# Make directory path available for our wrapper
set(FreeRTOS_SOURCE_DIR ${freertos_SOURCE_DIR})

# === FATFS ===
add_library(fatfs
    lib/fatfs/source/ff.c
    lib/fatfs/source/ffsystem.c
    lib/fatfs/source/ffunicode.c
    lib/fatfs/source/diskio.c
)

target_link_libraries(fatfs PUBLIC
    pico_stdlib
    hardware_spi
)

target_include_directories(fatfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/source
)

# === FreeRTOS Kernel wrapper module (correct ARM_CM0 paths) ===
add_subdirectory(cmake/freertos)

# === Application ===
add_executable(${PROJECT}
    main.c
    par_spi.c
    ftp_utils.c
    ftp_server.c
    util.c
)

pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/act_mirror.pio)

# --- Wi-Fi credentials from local wifi_credentials.cmake file ---
include(${CMAKE_SOURCE_DIR}/wifi_credentials.cmake OPTIONAL)

target_compile_definitions(${PROJECT} PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
)

target_include_directories(FreeRTOS-Kernel PUBLIC
    ${PICO_SDK_PATH}/src/rp2_common/cmsis/stub/CMSIS/Core/Include
)

# === Link libraries including FreeRTOS + CYW43 Wi-Fi ===
target_link_libraries(${PROJECT}
    pico_stdlib
    hardware_spi
    hardware_sync
    hardware_pio
    pico_multicore
    fatfs
    pico_util
    FreeRTOS-Kernel
    pico_cyw43_arch_lwip_sys_freertos
)

pico_add_extra_outputs(${PROJECT})
pico_enable_stdio_usb(${PROJECT} 1)
pico_enable_stdio_uart(${PROJECT} 0)
