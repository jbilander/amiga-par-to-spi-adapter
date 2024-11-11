#ifndef MAIN_H_FILE
#define MAIN_H_FILE

#define IRQ_BIT_n GPIO_NUM_5 // Output, active low, open collector, external pull-up.
#define LED_BIT GPIO_NUM_2   // Output

#define CLK_BIT GPIO_NUM_35   // Input, external pull-up.
#define ACT_BIT_n GPIO_NUM_33 // Output, active low, internal pull-up enabled, external pull-up.
#define CP_BIT_n GPIO_NUM_34  // Input, active low, external pull-up.
#define REQ_BIT_n GPIO_NUM_32 // Input, active low, external pull-up.

#define DEBOUNCE_TIME 200000 // the MicroSD card change debounce time in microseconds

#endif /* MAIN_H_FILE */