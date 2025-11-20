/*
 * Written in October 2022 by Niklas Ekström.
 *
 * Runs on RP2040 microcontroller instead of AVR as before,
 * but uses the same protocol and Amiga software.
 *
 * Partly rewritten by jbilander for RP2350 (Pico 2 W) using pio
 *
 */
#include "main.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/pio.h"
#include "par_amiga_rx.pio.h"   // auto-generated header from par_amiga_rx.pio
#include <ctype.h> // for isprint()

// ---------- DEBUG SWITCH ----------
#define PAR_DEBUG 1             // <- set to 0 to disable all debug
#define PAR_DUMP_BYTES 1        // <- set to 0 to only log count, not each byte
#define PAR_DEBUG_LIMIT 2048    // max bytes to print in one request (safety)

// Safe debug macro
#if PAR_DEBUG
#define PAR_LOG(...) printf(__VA_ARGS__)
#else
#define PAR_LOG(...)
#endif

// PIO selection
#define AMIGA_PIO pio1
#define AMIGA_SM  0

// PIO instance used for Amiga parallel capture
#define AMIGA_PIO         pio1

static uint32_t prev_cdet;

/* Shared SPI mutex declared in main.h */
extern mutex_t spi_mutex;
extern volatile bool amiga_wrote_to_card;

const char* get_pio_name(PIO pio) {
    if (pio == pio0) {
        return "pio0";
    } else if (pio == pio1) {
        return "pio1";
    } else if (pio == pio2) {
        return "pio2";
    } else {
        return "unknown_pio";
    }
}

// Initialize and configure PIO program for parallel read capture
static uint par_amiga_pio_offset;

static void par_amiga_pio_setup(void)
{
    PIO pio = AMIGA_PIO;
    int sm = AMIGA_SM;

    printf("Setup PIO for Amiga read capture (using %s SM%d)\n",
           get_pio_name(pio), sm);

    par_amiga_pio_offset = pio_add_program(pio, &par_amiga_rx_program);
    printf("PIO program loaded, offset=%u\n", par_amiga_pio_offset);

    pio_sm_config c = par_amiga_rx_program_get_default_config(par_amiga_pio_offset);

    // *** CRITICAL! Tell PIO which GPIO corresponds to IN PINS ***
    sm_config_set_in_pins(&c, PIN_D(0));   // maps IN PINS to D0..D7

    // Jump pin for REQ
    sm_config_set_jmp_pin(&c, PIN_REQ);

    // Shift left, autopush 8 bits
    sm_config_set_in_shift(&c, false, true, 8);

    // Set pin directions for the state machine
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_D(0), 8, false);  // D0..D7 input
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_CLK, 1, false);   // CLK input
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_REQ, 1, false);   // REQ input

    // Claim GPIO for PIO usage
    for (int i = 0; i < 8; i++)
        pio_gpio_init(pio, PIN_D(i));

    pio_gpio_init(pio, PIN_CLK);
    pio_gpio_init(pio, PIN_REQ);

    // Initialize SM (but leave disabled)
    pio_sm_init(pio, sm, par_amiga_pio_offset, &c);
    pio_sm_set_enabled(pio, sm, false);
}

/* Prototype (if handle_request is above this function in file) */
static void par_amiga_read_and_forward_to_spi(size_t byte_count);

/* Implementation */
static void par_amiga_read_and_forward_to_spi(size_t byte_count)
{
    PIO pio = AMIGA_PIO;
    int sm  = AMIGA_SM;        // Only one state machine now

#if PAR_DEBUG
    printf("[PIO] Starting read of %u bytes\n", (unsigned)byte_count);
#endif

    // Enable SM and clear FIFO
    pio_sm_clear_fifos(pio, sm);
    pio_sm_set_enabled(pio, sm, true);

    for (size_t i = 0; i < byte_count; i++)
    {
        uint32_t w = pio_sm_get_blocking(pio, sm);
        uint8_t b = w & 0xFF;

#if PAR_DEBUG
        printf("[PIO] DATA[%03u] = 0x%02lX (%c)\n",
               (unsigned)i,
               (unsigned long)b,
               (b >= 32 && b < 127) ? (int)b : '.');
#endif

        // Forward byte to SPI
        spi_get_hw(spi0)->dr = b;

        // Wait for SPI read to complete
        while (!spi_is_readable(spi0))
            tight_loop_contents();

        (void)spi_get_hw(spi0)->dr;
    }

    // Disable SM after reading
    pio_sm_set_enabled(pio, sm, false);

#if PAR_DEBUG
    printf("[PIO] Transfer complete\n");
#endif

    amiga_wrote_to_card = true;
}

// The rest of your original handle_request code, with the write path replaced to use PIO read
static void handle_request() {
    uint32_t pins;

    while (1) {
        pins = gpio_get_all();
        if (!(pins & (1 << PIN_REQ)))
            break;

        if ((pins & (1 << PIN_CDET)) != prev_cdet) {
            gpio_put(PIN_IRQ, false);
            gpio_set_dir(PIN_IRQ, true);
            prev_cdet = pins & (1 << PIN_CDET);
        }
    }

    uint32_t prev_clk = pins & (1 << PIN_CLK);

    if ((pins & 0xc0) != 0xc0) {
        uint32_t byte_count = 0;
        bool read = false;

        if (!(pins & 0x80)) { // READ1 or WRITE1
            read = !!(pins & 0x40);
            byte_count = pins & 0x3f;

            gpio_put(PIN_ACT, 0);
        } else { // READ2 or WRITE2
            byte_count = (pins & 0x3f) << 7;

            gpio_put(PIN_ACT, 0);

            while (1) {
                pins = gpio_get_all();
                if ((pins & (1 << PIN_CLK)) != prev_clk)
                    break;

                if (pins & (1 << PIN_REQ))
                    return;
            }

            read = !!(pins & 0x80);
            byte_count |= pins & 0x7f;
            prev_clk = pins & (1 << PIN_CLK);
        }

        if (read) {
            spi_get_hw(spi0)->dr = 0xff;

            uint32_t prev_ss = pins & (1 << PIN_SS);

            while (1) {
                while (!spi_is_readable(spi0))
                    tight_loop_contents();

                uint32_t value = spi_get_hw(spi0)->dr;

                while (1) {
                    pins = gpio_get_all();
                    if ((pins & (1 << PIN_CLK)) != prev_clk)
                        break;

                    if (pins & (1 << PIN_REQ))
                        return;
                }

                gpio_put_all(prev_ss | value);
                gpio_set_dir_out_masked(0xff);

                if (!byte_count)
                    break;

                spi_get_hw(spi0)->dr = 0xff;
                prev_clk = pins & (1 << PIN_CLK);
                byte_count--;
            }
        } else {
            // Amiga -> Pico write path — DELEGATE SAMPLING TO PIO1
            // The PIO captures bytes on PIN_CLK rising edge while PIN_REQ is active low.
            // We will read 'byte_count' bytes from the PIO RX FIFO and forward them to SPI.
            #if PAR_DEBUG
            printf("[PIO] Request to read %u bytes from Amiga\n", (unsigned)byte_count);
            #endif

            par_amiga_read_and_forward_to_spi(byte_count);
        }
    } else {
        switch ((pins & 0x3e) >> 1) {
            case 0: { // SPI_SELECT
                gpio_put(PIN_SS, !(pins & 1));
                gpio_put(PIN_ACT, 0);
                break;
            }
            case 1: { // CARD_PRESENT
                gpio_set_dir(PIN_IRQ, false);
                gpio_put(PIN_ACT, 0);

                while (1) {
                    pins = gpio_get_all();
                    if ((pins & (1 << PIN_CLK)) != prev_clk)
                        break;

                    if (pins & (1 << PIN_REQ))
                        return;
                }

                gpio_put(PIN_D(0), !gpio_get(PIN_CDET));
                gpio_set_dir_out_masked(0xff);
                break;
            }
            case 2: { // SPEED
                spi_set_baudrate(spi0, pins & 1 ?
                        SPI_FAST_FREQUENCY :
                        SPI_SLOW_FREQUENCY);

                gpio_put(PIN_ACT, 0);
                break;
            }
        }
    }

    while (1) {
        pins = gpio_get_all();
        if (pins & (1 << PIN_REQ))
            break;
    }
}

void par_spi_main() {
    PAR_LOG("Start Amiga SPI bridge\n");
    spi_init(spi0, SPI_SLOW_FREQUENCY);

    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_MISO);

    gpio_init(PIN_SS);
    gpio_put(PIN_SS, 1);
    gpio_set_dir(PIN_SS, GPIO_OUT);

    gpio_init(PIN_CDET);
    gpio_pull_up(PIN_CDET);

    for (int i = 0; i < 12; i++)
        gpio_init(i);

    gpio_put(PIN_ACT, 1);
    gpio_set_dir(PIN_ACT, GPIO_OUT);

    prev_cdet = gpio_get_all() & (1 << PIN_CDET);

    par_amiga_pio_setup();

    while (1) {
        handle_request();

        gpio_set_dir_in_masked(0xff);
        gpio_clr_mask(0xff);

        gpio_put(PIN_ACT, 1);

        while (spi_is_busy(spi0))
            tight_loop_contents();

        if (spi_is_readable(spi0))
            (void)spi_get_hw(spi0)->dr;
    }
}