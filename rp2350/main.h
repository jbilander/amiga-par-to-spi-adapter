#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

//      Pin name    GPIO    Direction   Comment     Description
#define PIN_D(x)    (0+x)   // In/out
#define PIN_IRQ     8       // Output   Active low
#define PIN_ACT     9       // Output   Active low
#define PIN_CLK     10      // Input
#define PIN_REQ     11      // Input    Active low
#define PIN_MISO    16      // Input    Pull-up
#define PIN_SS      17      // Output   Active low
#define PIN_SCK     18      // Output
#define PIN_MOSI    19      // Output
#define PIN_CDET    20      // Input    Pull-up     Card Detect
#define PIN_LED     28      // Output

void par_spi_main(void);
void ftp_server_main(void);

#endif
