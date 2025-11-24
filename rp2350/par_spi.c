/*
 * par_spi_pio.c
 *
 * Uses PIO state machine to mirror REQ → ACT in hardware
 * ACT tracking is now hardware-based with ~8-16ns latency
 * Much simpler and more reliable than interrupt-based approach
 */

#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "act_mirror.pio.h"  // Generated from act_mirror.pio

#define PAR_SPI_VERSION "v33-pio"

#ifndef PAR_DEBUG
#define PAR_DEBUG 1
#endif

#define MAX_CAPTURE 4096
static volatile uint8_t cap_buf[MAX_CAPTURE];
static volatile uint32_t cap_count = 0;

static volatile bool req_low_flag = false;
static volatile bool req_high_flag = false;

// PIO state machine for ACT mirroring
static PIO act_pio = pio0;
static uint act_sm = 0;

/*
 * IRQ handler - SIMPLIFIED (no ACT management!)
 * PIO handles ACT automatically
 */
void __not_in_flash_func(par_amiga_clk_req_irq)(void)
{
    uint32_t events_clk = gpio_get_irq_event_mask(PIN_CLK);
    uint32_t events_req = gpio_get_irq_event_mask(PIN_REQ);

    // REQ events - just set flags, PIO handles ACT
    if (events_req) {
        if (!gpio_get(PIN_REQ)) {
            // REQ falling edge
            req_low_flag = true;
            cap_count = 0;
            
            // Sample first byte
            if (cap_count < MAX_CAPTURE) {
                uint32_t pins = gpio_get_all();
                cap_buf[cap_count++] = pins & 0xFF;
            }
        } else {
            // REQ rising edge
            req_high_flag = true;
        }
    }

    // CLK events - sample data
    if (events_clk) {
        if (!gpio_get(PIN_REQ)) {
            if (cap_count < MAX_CAPTURE) {
                uint32_t pins = gpio_get_all();
                cap_buf[cap_count++] = pins & 0xFF;
            }
        }
    }

    gpio_acknowledge_irq(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    gpio_acknowledge_irq(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
}

static inline void set_data_output(uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        gpio_put(PIN_D(i), (value >> i) & 1);
        gpio_set_dir(PIN_D(i), GPIO_OUT);
    }
}

static inline void set_data_input(void)
{
    for (int i = 0; i < 8; i++) {
        gpio_set_dir(PIN_D(i), GPIO_IN);
    }
}

static inline uint32_t wait_for_clk_edge_avr_style(uint32_t current_clk)
{
    if (current_clk) {
        while (gpio_get(PIN_CLK)) tight_loop_contents();
        return 0;
    } else {
        while (!gpio_get(PIN_CLK)) tight_loop_contents();
        return 1;
    }
}

static inline void setup_delay(void)
{
    __asm volatile(
        "nop\n nop\n nop\n nop\n nop\n"
        "nop\n nop\n nop\n nop\n nop\n"
        "nop\n nop\n nop\n nop\n nop\n"
        "nop\n nop\n nop\n nop\n nop\n"
    );
}

static bool handle_special_command(uint8_t first_byte)
{
    if ((first_byte & 0xC0) != 0xC0) return false;
    
    uint8_t cmd = (first_byte & 0x3E) >> 1;
    uint8_t param = first_byte & 0x01;
    
    if (PAR_DEBUG) printf("[CMD] %u param=%u\n", cmd, param);
    
    switch (cmd) {
        case 0:
            gpio_put(PIN_SS, param ? 0 : 1);
            gpio_put(PIN_LED, param ? 1 : 0);
            if (PAR_DEBUG) printf("[CMD] SD %s\n", param ? "selected" : "deselected");
            break;
        case 1:
            if (PAR_DEBUG) printf("[CMD] Card present query\n");
            uint32_t clk = gpio_get(PIN_CLK);
            clk = wait_for_clk_edge_avr_style(clk);
            setup_delay();
            set_data_output(!gpio_get(PIN_CDET) ? 0x01 : 0x00);
            clk = wait_for_clk_edge_avr_style(clk);
            set_data_input();
            break;
        case 2:
            spi_set_baudrate(spi0, param ? SPI_FAST_FREQUENCY : SPI_SLOW_FREQUENCY);
            if (PAR_DEBUG) printf("[CMD] SPI %s\n", param ? "fast" : "slow");
            break;
    }
    return true;
}

static void process_read_transfer(uint32_t byte_count)
{
    if (byte_count == 0) return;

    if (PAR_DEBUG) printf("[READ] Fetching %u bytes from SD\n", (unsigned)byte_count);

    uint8_t resp_buf[2048];
    uint32_t need = (byte_count > sizeof(resp_buf)) ? sizeof(resp_buf) : byte_count;

    mutex_enter_blocking(&spi_mutex);
    for (uint32_t i = 0; i < need; i++) {
        uint8_t dummy = 0xFF;
        spi_write_read_blocking(spi0, &dummy, &resp_buf[i], 1);
    }
    mutex_exit(&spi_mutex);

    if (PAR_DEBUG) printf("[READ] Presenting %u bytes\n", (unsigned)need);

    // Disable CLK interrupts during presentation
    // NOTE: No need to worry about ACT - PIO handles it automatically!
    gpio_set_irq_enabled(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

    uint32_t clk_state = gpio_get(PIN_CLK);

    for (uint32_t i = 0; i < need; i++) {
        clk_state = wait_for_clk_edge_avr_style(clk_state);
        setup_delay();
        set_data_output(resp_buf[i]);
        clk_state = wait_for_clk_edge_avr_style(clk_state);
    }

    set_data_input();
    
    // Re-enable CLK interrupts
    gpio_acknowledge_irq(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    gpio_set_irq_enabled(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    // No need to touch ACT - PIO is handling it!
    
    if (PAR_DEBUG) printf("[READ] Complete\n");
}

static void process_write_transfer(uint32_t byte_count, uint32_t header_bytes)
{
    uint32_t payload = (cap_count > header_bytes) ? (cap_count - header_bytes) : 0;
    
    if (payload < byte_count) {
        if (PAR_DEBUG) {
            printf("[WRITE] Warning: expected %u, got %u\n",
                   (unsigned)byte_count, (unsigned)payload);
        }
        byte_count = payload;
    }

    if (PAR_DEBUG) printf("[WRITE] Sending %u bytes\n", (unsigned)byte_count);

    mutex_enter_blocking(&spi_mutex);
    for (uint32_t i = 0; i < byte_count; i++) {
        uint8_t data = cap_buf[header_bytes + i];
        spi_write_blocking(spi0, &data, 1);
    }
    mutex_exit(&spi_mutex);

    amiga_wrote_to_card = true;
    if (PAR_DEBUG) printf("[WRITE] Complete\n");
}

static void process_captured_transfer(void)
{
    gpio_set_irq_enabled(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    gpio_set_irq_enabled(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

    uint32_t n = cap_count < MAX_CAPTURE ? cap_count : MAX_CAPTURE;
    /*
    uint8_t local_buf[MAX_CAPTURE];
    for (uint32_t i = 0; i < n; i++) {
        local_buf[i] = cap_buf[i];
    }
    */
    uint8_t *local_buf = malloc(n ? n : 1);
    if (!local_buf) {
        printf("[ERR] failed to malloc local_buf size=%lu\n", (unsigned long)n);
        gpio_acknowledge_irq(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
        gpio_acknowledge_irq(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
        gpio_set_irq_enabled(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
        gpio_set_irq_enabled(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
        return;
    }
    memcpy(local_buf, (const void *)cap_buf, n);

    gpio_acknowledge_irq(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    gpio_acknowledge_irq(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    gpio_set_irq_enabled(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    if (n == 0) return;

    if (handle_special_command(local_buf[0])) return;

    uint32_t byte_count = 0;
    bool is_read = false;
    uint32_t header_samples = 0;
    uint8_t first = local_buf[0];

    if (!(first & 0x80)) {
        is_read = !!(first & 0x40);
        //byte_count = (first & 0x3F) + 1;
        byte_count = (first & 0x3F);
        header_samples = 1;
        if (PAR_DEBUG) {
            printf("[%s1] %u bytes\n", is_read ? "READ" : "WRITE", (unsigned)byte_count);
        }
    } else if (!(first & 0x40)) {
        if (n < 2) return;
        uint8_t second = local_buf[1];
        //byte_count = (((uint32_t)(first & 0x3F) << 7) | (second & 0x7F)) + 1;
        byte_count = (((uint32_t)(first & 0x3F) << 7) | (second & 0x7F));
        is_read = !!(second & 0x80);
        header_samples = 2;
        if (PAR_DEBUG) {
            printf("[%s2] %u bytes\n", is_read ? "READ" : "WRITE", (unsigned)byte_count);
        }
    } else {
        return;
    }

    if (is_read) {
        process_read_transfer(byte_count);
    } else {
        process_write_transfer(byte_count, header_samples);
    }
    free(local_buf);
}

void par_spi_main(void)
{
    if (PAR_DEBUG) {
        printf("\n=== Pico 2 W Amiga SPI Bridge %s ===\n", PAR_SPI_VERSION);
        printf("Using PIO for hardware-based ACT mirroring\n\n");
    }

    // Initialize SPI
    spi_init(spi0, SPI_SLOW_FREQUENCY);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_MISO);

    // SS pin
    gpio_init(PIN_SS);
    gpio_put(PIN_SS, 1);
    gpio_set_dir(PIN_SS, GPIO_OUT);

    // Card detect
    gpio_init(PIN_CDET);
    gpio_pull_up(PIN_CDET);
    gpio_set_dir(PIN_CDET, GPIO_IN);

    // LED
    gpio_init(PIN_LED);
    gpio_put(PIN_LED, 0);
    gpio_set_dir(PIN_LED, GPIO_OUT);

    // D0..D7 as inputs
    for (int i = 0; i < 8; i++) {
        gpio_init(PIN_D(i));
        gpio_set_dir(PIN_D(i), GPIO_IN);
        gpio_disable_pulls(PIN_D(i));
    }

    // REQ as input with pull-up
    gpio_init(PIN_REQ);
    gpio_set_dir(PIN_REQ, GPIO_IN);
    gpio_pull_up(PIN_REQ);

    // CLK as input with pull-up
    gpio_init(PIN_CLK);
    gpio_set_dir(PIN_CLK, GPIO_IN);
    gpio_pull_up(PIN_CLK);

    // ACT pin - will be controlled by PIO, but initialize it first
    gpio_init(PIN_ACT);
    gpio_put(PIN_ACT, 1);
    gpio_set_dir(PIN_ACT, GPIO_OUT);

    // === Initialize PIO for ACT mirroring ===
    // Load PIO program
    uint offset = pio_add_program(act_pio, &act_mirror_program);
    
    // Initialize PIO state machine to mirror REQ → ACT
    act_mirror_program_init(act_pio, act_sm, offset, PIN_REQ, PIN_ACT);
    
    if (PAR_DEBUG) {
        printf("[PIO] ACT mirroring initialized (PIO%d SM%d)\n", 
               pio_get_index(act_pio), act_sm);
        printf("[PIO] REQ (GPIO%d) → ACT (GPIO%d) with ~8-16ns latency\n",
               PIN_REQ, PIN_ACT);
    }

    // Clear state
    cap_count = 0;
    req_low_flag = false;
    req_high_flag = false;

    // Setup GPIO interrupts for REQ and CLK (for data capture)
    gpio_acknowledge_irq(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    gpio_acknowledge_irq(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);

    irq_set_exclusive_handler(IO_IRQ_BANK0, par_amiga_clk_req_irq);
    irq_set_priority(IO_IRQ_BANK0, 0);
    irq_set_enabled(IO_IRQ_BANK0, true);

    gpio_set_irq_enabled(PIN_CLK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    if (PAR_DEBUG) printf("[INIT] Ready\n\n");

    while (true) {
        if (req_low_flag) {
            req_low_flag = false;
            req_high_flag = false;
            if (PAR_DEBUG) printf("[STATE] REQ asserted\n");
        }

        if (req_high_flag) {
            req_high_flag = false;
            if (PAR_DEBUG) printf("[STATE] REQ deasserted\n");
            
            process_captured_transfer();
            
            if (PAR_DEBUG) printf("[STATE] Ready\n");
        }

        tight_loop_contents();
    }
}