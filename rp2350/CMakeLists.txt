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

target_link_libraries(fatfs PUBLIC
    pico_stdlib
    hardware_spi
)

target_include_directories(fatfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/source
)

add_executable(${PROJECT} ftp_server.c par_spi.c main.c util.c)

#pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/par_amiga_rx.pio)
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/act_mirror.pio)

# --- Wi-Fi credentials from local wifi_credentials.cmake file ---
include(${CMAKE_SOURCE_DIR}/wifi_credentials.cmake OPTIONAL)

target_compile_definitions(${PROJECT} PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
)

target_include_directories(${PROJECT} PRIVATE
    ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_arch/include
    ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
    ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
    ${PICO_SDK_PATH}/src/rp2_common/pico_async_context/include
    ${PICO_SDK_PATH}/lib/lwip/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

pico_add_extra_outputs(${PROJECT})
pico_enable_stdio_usb(${PROJECT} 1)
pico_enable_stdio_uart(${PROJECT} 0)

target_link_libraries(${PROJECT}
    pico_stdlib
    hardware_spi
    hardware_sync
    hardware_pio
    pico_multicore
    fatfs
    pico_util
    pico_cyw43_arch_lwip_threadsafe_background
)
