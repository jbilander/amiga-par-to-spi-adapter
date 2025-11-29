#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ---------------------------------------------------------------------------
 * Platform Includes (Pico SDK)
 * --------------------------------------------------------------------------- */
#include "rp2350_device.h"
#include "core_cm33.h"
#include "cmsis_gcc.h"
#include "pico.h"
#include "hardware/clocks.h"
//#include "rp2350_device.h"
//#include "rp2350_irq.h"
//#include "core_cm33.h"



/* ---------------------------------------------------------------------------
 * Cortex-M33 Mandatory Configuration (RP2350 ARM-S core)
 * --------------------------------------------------------------------------- */

/* RP2350 includes hardware FPU */
#define configENABLE_FPU                1

/* Pico SDK runs non-secure only */
#define configENABLE_TRUSTZONE          0

/* RP2350 does not use FreeRTOS MPU */
#define configENABLE_MPU                0

/* ---------------------------------------------------------------------------
 * Cortex-M33 Interrupt Priority Configuration (REQUIRED)
 * --------------------------------------------------------------------------- */

/* RP2350 Cortex-M33 implements 3 bits of priority */
#define configPRIO_BITS                             3

/* Lowest interrupt priority in the NVIC */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     7

/* Highest priority from which FreeRTOS API can be called */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

/* Derived FreeRTOS priority values */
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ---------------------------------------------------------------------------
 * Kernel Basics
 * --------------------------------------------------------------------------- */

#define configUSE_PREEMPTION            1
#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0

#define configCPU_CLOCK_HZ              ( clock_get_hz(clk_sys) )
#define configTICK_RATE_HZ              ( 1000U )

#define configMAX_PRIORITIES            7
#define configMINIMAL_STACK_SIZE        256
#define configTOTAL_HEAP_SIZE           (32 * 1024)
#define configMAX_TASK_NAME_LEN         16

#define configUSE_16_BIT_TICKS          0

/* ---------------------------------------------------------------------------
 * Memory Allocation / Hooks
 * --------------------------------------------------------------------------- */
#define configCHECK_FOR_STACK_OVERFLOW  0
#define configUSE_MALLOC_FAILED_HOOK    0

/* ---------------------------------------------------------------------------
 * Synchronization Objects
 * --------------------------------------------------------------------------- */
#define configUSE_MUTEXES               1
#define configUSE_RECURSIVE_MUTEXES     1
#define configUSE_COUNTING_SEMAPHORES   1
#define configUSE_QUEUE_SETS            1

/* ---------------------------------------------------------------------------
 * Software Timers
 * --------------------------------------------------------------------------- */
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       (configMAX_PRIORITIES - 1)
#define configTIMER_TASK_STACK_DEPTH    512
#define configTIMER_QUEUE_LENGTH        10

/* ---------------------------------------------------------------------------
 * Multicore Handling
 * --------------------------------------------------------------------------- */
#define configNUM_CORES                 1

/* ---------------------------------------------------------------------------
 * API Inclusions (required by Pico async_context_freertos)
 * --------------------------------------------------------------------------- */
#define INCLUDE_vTaskPrioritySet        1
#define INCLUDE_uxTaskPriorityGet       1
#define INCLUDE_vTaskDelete             1
#define INCLUDE_vTaskSuspend            1
#define INCLUDE_vTaskDelayUntil         1
#define INCLUDE_vTaskDelay              1
#define INCLUDE_xSemaphoreGetMutexHolder 1

/* ---------------------------------------------------------------------------
 * lwIP Compatibility
 * --------------------------------------------------------------------------- */
#define portTICK_RATE_MS                portTICK_PERIOD_MS

/* ---------------------------------------------------------------------------
 * ISR Detection (required by pico_async_context_freertos)
 * --------------------------------------------------------------------------- */
#define portCHECK_IF_IN_ISR()           (__get_IPSR() != 0)

/* ---------------------------------------------------------------------------
 * Diagnostics / Debug
 * --------------------------------------------------------------------------- */
#define configASSERT(x) if((x)==0) { taskDISABLE_INTERRUPTS(); for( ;; ); }
#define configQUEUE_REGISTRY_SIZE       8
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1

#endif /* FREERTOS_CONFIG_H */
