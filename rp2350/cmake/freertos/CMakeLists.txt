#
# FreeRTOS Kernel wrapper for RP2350 / Cortex-M33 (non-secure)
#

cmake_minimum_required(VERSION 3.13)

add_library(FreeRTOS-Kernel STATIC
    ${FreeRTOS_SOURCE_DIR}/tasks.c
    ${FreeRTOS_SOURCE_DIR}/queue.c
    ${FreeRTOS_SOURCE_DIR}/list.c
    ${FreeRTOS_SOURCE_DIR}/timers.c
    ${FreeRTOS_SOURCE_DIR}/event_groups.c
    ${FreeRTOS_SOURCE_DIR}/stream_buffer.c

    # Heap allocator
    ${FreeRTOS_SOURCE_DIR}/portable/MemMang/heap_4.c

    # ARM CM33 non-secure port
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure/port.c
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure/portasm.c
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure/mpu_wrappers_v2_asm.c
)

#
# Include paths — REAL CMSIS, NOT stub version
#
target_include_directories(FreeRTOS-Kernel PUBLIC
    ${FreeRTOS_SOURCE_DIR}/include
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure
    ${CMAKE_CURRENT_LIST_DIR}/config

    # --- Ensure real CMSIS headers take priority ---
    BEFORE
    ${PICO_SDK_PATH}/src/rp2_common/cmsis/include
    ${PICO_SDK_PATH}/src/rp2_common/cmsis/include/core
    ${PICO_SDK_PATH}/src/rp2_common/cmsis/cores/cmsis_core_cm33

    # Needed by FreeRTOSConfig.h
    ${PICO_SDK_PATH}/src/common/pico_base/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_clocks/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_irq/include
)

#
# Link libraries
#
target_link_libraries(FreeRTOS-Kernel PUBLIC
    pico_stdlib
    hardware_clocks
    hardware_irq
)

