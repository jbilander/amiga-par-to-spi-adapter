#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* --------------------------------------------------------------------------
 * Minimal, safe FreeRTOSConfig for RP2350 (Cortex-M33 non-secure / NTZ)
 * - Do NOT include Pico SDK headers here (they get included elsewhere).
 * - Implement vApplicationGetIdleTaskMemory() and vApplicationGetTimerTaskMemory()
 *   in your application source (main.c). Do NOT prototype them here.
 * -------------------------------------------------------------------------- */

/* Basic kernel behaviour */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_16_BIT_TICKS                  0   /* REQUIRED: set to 0 for 32-bit tick */
#define configCPU_CLOCK_HZ                      ( 150000000UL ) /* fallback; you can override via clock_get_hz(clk_sys) */
#define configTICK_RATE_HZ                      1000U
#define configMAX_PRIORITIES                    7
#define configMINIMAL_STACK_SIZE                256
#define configTOTAL_HEAP_SIZE                   (64 * 1024)
#define configMAX_TASK_NAME_LEN                 16

/* Hooks */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     1
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2

/* Synchronization primitives */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    1

/* Timers */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                8
#define configTIMER_TASK_STACK_DEPTH            (configMINIMAL_STACK_SIZE * 2)

/* Allocation support */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* Cortex-M33 / RP2350 specifics (disabled features) */
#define configENABLE_MPU                        0
#define configENABLE_FPU                        0
#define configENABLE_TRUSTZONE                  0

/* Priority settings for NVIC / FreeRTOS */
#define configPRIO_BITS                         3
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 7
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 1

#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )
#define configKERNEL_INTERRUPT_PRIORITY         ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

/* Optional API functions */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xSemaphoreGetMutexHolder        1  /* required by pico async_context_freertos */

/* Helper macro expected by Pico's async_context_freertos */
#define portCHECK_IF_IN_ISR() (__get_IPSR() != 0)

/* Assertion handling: declare vAssertCalled here (implement it in main.c) */
extern void vAssertCalled(const char *file, int line);
#define configASSERT(x) if ((x) == 0) vAssertCalled(__FILE__, __LINE__)

/* Trace / stats (optional) */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0

#endif /* FREERTOS_CONFIG_H */
