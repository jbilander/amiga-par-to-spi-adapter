#
# FreeRTOS Kernel build for RP2350 (Cortex-M33, non-secure)
#

add_library(FreeRTOS-Kernel STATIC
    ${FreeRTOS_SOURCE_DIR}/tasks.c
    ${FreeRTOS_SOURCE_DIR}/queue.c
    ${FreeRTOS_SOURCE_DIR}/list.c
    ${FreeRTOS_SOURCE_DIR}/timers.c
    ${FreeRTOS_SOURCE_DIR}/event_groups.c
    ${FreeRTOS_SOURCE_DIR}/stream_buffer.c

    # Heap
    ${FreeRTOS_SOURCE_DIR}/portable/MemMang/heap_4.c

    # Cortex-M33 NTZ (non-secure) port sources
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure/port.c
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure/portasm.c
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure/mpu_wrappers_v2_asm.c
)

#
# Include paths required for FreeRTOS + RP2350
#
target_include_directories(FreeRTOS-Kernel PUBLIC
    ${FreeRTOS_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/cmsis_rp2350
    ${FreeRTOS_SOURCE_DIR}/portable/GCC/ARM_CM33_NTZ/non_secure
    ${CMAKE_CURRENT_SOURCE_DIR}/config

    # REAL CMSIS (ARM official)
    ${CMAKE_CURRENT_SOURCE_DIR}/cmsis_rp2350/Core/Include

    # Pico headers still needed for clocks, IRQs, etc.
    ${PICO_SDK_PATH}/src/common/pico_base/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_clocks/include
    ${PICO_SDK_PATH}/src/rp2_common/hardware_irq/include
)

#
# Link with Pico SDK platform drivers
#
target_link_libraries(FreeRTOS-Kernel PUBLIC
    pico_stdlib
    hardware_clocks
    hardware_irq
)

#
# RP2350 Cortex-M33 compile flags
#
target_compile_options(FreeRTOS-Kernel PUBLIC
    -mcpu=cortex-m33
    -mthumb
    -march=armv8-m.main+fp+dsp
    -mfloat-abi=softfp
    -mcmse
)

#
# Required CMSIS + FreeRTOS architecture feature macros
#
target_compile_definitions(FreeRTOS-Kernel PUBLIC
    __FPU_PRESENT=1
    __DSP_PRESENT=1
    __ARM_FEATURE_DSP=1

    # Required by FreeRTOS CM33 NTZ ports
    configENABLE_FPU=1
    configENABLE_MPU=0
    configENABLE_TRUSTZONE=0
)

