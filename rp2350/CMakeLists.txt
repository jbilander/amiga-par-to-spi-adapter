cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2)
set(PICO_SDK_FETCH_FROM_GIT on)
include(pico_sdk_import.cmake)
set(PROJECT par_spi_ftp_server)
project(${PROJECT} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_compile_options(-Wall) 

add_executable(${PROJECT} ftp_server.c par_spi.c main.c)

pico_add_extra_outputs(${PROJECT})

target_link_libraries(${PROJECT} pico_stdlib hardware_spi pico_multicore)

if (PICO_CYW43_SUPPORTED)
	target_link_libraries(${PROJECT} pico_cyw43_arch_none)
endif()

