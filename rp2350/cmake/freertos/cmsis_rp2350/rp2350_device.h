#ifndef _RP2350_DEVICE_H_
#define _RP2350_DEVICE_H_

// Number of priority bits in NVIC (RP2350 has 8 priority levels)
#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS 3
#endif

/*
 * Minimal CMSIS IRQn_Type required by core_cm33.h
 * RP2350 SDK does not supply a device header yet.
 */
typedef enum IRQn
{
    /* Cortex-M33 core exceptions */
    Reset_IRQn              = -15,
    NonMaskableInt_IRQn     = -14,
    HardFault_IRQn          = -13,
    MemoryManagement_IRQn   = -12,
    BusFault_IRQn           = -11,
    UsageFault_IRQn         = -10,
    SVCall_IRQn             = -5,
    DebugMonitor_IRQn       = -4,
    PendSV_IRQn             = -2,
    SysTick_IRQn            = -1,

    /* Minimal placeholder peripheral IRQs */
    TIMER_IRQ_0_IRQn        = 0,
    TIMER_IRQ_1_IRQn        = 1,
    DMA_IRQ_0_IRQn          = 2,
    DMA_IRQ_1_IRQn          = 3,
    SIO_IRQ_0_IRQn          = 4,
    SIO_IRQ_1_IRQn          = 5,

} IRQn_Type;

#endif

