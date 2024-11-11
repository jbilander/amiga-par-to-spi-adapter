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

    /* Check and clear interrupt GPIO32 and/or GPIO34, set corresponding ISR volatile int (ISR0, ISR1)  */

    movi	a13, GPIO_STATUS1_REG
    memw
    l32i.n	a13, a13, 0
    bbci	a13, 0, check_next_gpio34
    movi.n	a14, (1 << 0)
    movi	a15, GPIO_STATUS1_W1TC_REG
    memw
    s32i.n	a14, a15, 0
    movi	a15, ISR0
    memw
    s32i	a14, a15, 0

check_next_gpio34:

    bbci	a13, 2, all_done
    movi	a13, GPIO_STATUS1_W1TC_REG
    movi.n	a14, (1 << 2)
    memw
    s32i.n	a14, a13, 0
    movi	a13, ISR1
    movi.n	a14, 1
    memw
    s32i	a14, a13, 0

all_done:

    l32i    a15, a0, LX_INTR_A15_OFFSET
    l32i    a14, a0, LX_INTR_A14_OFFSET
    l32i    a13, a0, LX_INTR_A13_OFFSET

    rsr     a0, EXCSAVE_5 // restore a0
    rfi     5

    .global ld_include_xt_highint5
ld_include_xt_highint5:
