#include <soc/gpio_reg.h>
#include <xtensa/specreg.h>

#define L5_INTR_STACK_SIZE 12
#define LX_INTR_A15_OFFSET 0
#define LX_INTR_A14_OFFSET 4
#define LX_INTR_A13_OFFSET 8

    .data
_l5_intr_stack:
    .space      L5_INTR_STACK_SIZE

    .section    .iram1,"ax"
    .global     xt_highint5
    .type       xt_highint5,@function
    .align      4
xt_highint5:

    movi    a0, _l5_intr_stack // assuming a0 saved restored by EXCSAVE_5 at the end of this
    s32i    a15, a0, LX_INTR_A15_OFFSET
    s32i    a14, a0, LX_INTR_A14_OFFSET
    s32i    a13, a0, LX_INTR_A13_OFFSET

    
    /* Clear interrupt GPIO39, note name "STATUS1" for GPIO32-39 and "STATUS" for GPIO0-31, esp32_technical_reference_manual_en.pdf */
    movi    a14, GPIO_STATUS1_W1TC_REG
    movi    a15, (1 << 7)
    s32i    a15, a14, 0
    memw
    
    /* Raise ISR0 flag */
    movi    a13, ISR0
    movi    a14, 1
    s32i    a14, a13, 0
    memw

    l32i    a15, a0, LX_INTR_A15_OFFSET
    l32i    a14, a0, LX_INTR_A14_OFFSET
    l32i    a13, a0, LX_INTR_A13_OFFSET

    rsr     a0, EXCSAVE_5 // restore a0
    rfi     5

    .global ld_include_xt_highint5
ld_include_xt_highint5:
