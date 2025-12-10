cmake_minimum_required(VERSION 3.20)
set(PICO_BOARD pico2_w)
set(PICO_SDK_FETCH_FROM_GIT on)

include(pico_sdk_import.cmake)

set(PROJECT par_spi_ftp_server)
project(${PROJECT} C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_compile_options(-Wall)

set(FREERTOS_SMP ON CACHE BOOL "Enable FreeRTOS SMP mode" FORCE)

# === Fetch FreeRTOS Kernel (download-only; do NOT run its CMakeLists.txt) ===
include(FetchContent)

FetchContent_Declare(
    freertos
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG main
)

# Only download files; DO NOT process FreeRTOS CMakeLists
FetchContent_Populate(freertos)

add_library(freertos_config INTERFACE)

# The include path should be set to the directory where you have FreeRTOSConfig.h
target_include_directories(freertos_config SYSTEM INTERFACE include)
target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)
set(FREERTOS_KERNEL_PATH ${CMAKE_SOURCE_DIR}/build/_deps/freertos-src)
include(FreeRTOS_Kernel_Import.cmake)

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

# === Application ===
add_executable(${PROJECT}
    main.c
    par_spi.c
    ftp_server.c
)

pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/act_mirror.pio)

# --- Wi-Fi credentials from local wifi_credentials.cmake file ---
include(${CMAKE_SOURCE_DIR}/wifi_credentials.cmake OPTIONAL)

target_compile_definitions(${PROJECT} PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
    FTP_USER="${FTP_USER}"
    FTP_PASSWORD="${FTP_PASSWORD}"
)

target_include_directories(${PROJECT} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

# === Link libraries including FreeRTOS + CYW43 Wi-Fi ===
target_link_libraries(${PROJECT}
    pico_stdlib
    hardware_timer
    hardware_spi
    hardware_sync
    hardware_pio
    hardware_watchdog
    pico_unique_id
    pico_multicore
    fatfs
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    pico_cyw43_arch_lwip_threadsafe_background
)

pico_add_extra_outputs(${PROJECT})
pico_enable_stdio_usb(${PROJECT} 1)
pico_enable_stdio_uart(${PROJECT} 0)
