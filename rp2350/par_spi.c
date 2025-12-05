/*
 * par_spi.c - Amiga SPI Bridge (Bare Metal Mode)
 * VERSION: 2025-12-02-WATCHDOG-REBOOT
 * 
 * Originally based on Niklas Ekström's October 2022 RP2040 version
 * Adapted for jbilander's Pico 2 W hardware with watchdog-based mode switching
 * 
 * Runs in BOOT_MODE_BARE_METAL (called from launch_bare_metal_mode)
 * Uses exclusive interrupt handler for fast response (~200-300ns)
 * PIO1 for ACT mirroring
 * Monitors GPIO 13 button for 3-second hold to trigger reboot to FreeRTOS mode
 * 
 * Watchdog reboot architecture:
 * - par_spi_main() called directly from launch_bare_metal_mode()
 * - Periodically checks button via monitor_button_for_mode_switch()
 * - 3-second button hold triggers watchdog reboot to FreeRTOS mode
 */

#include "main.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "act_mirror.pio.h"

static uint32_t prev_cdet;
static volatile bool req_triggered = false;
static volatile bool card_detect_enabled = true;

// Card detect debouncing (prevents spurious interrupts from mechanical bouncing)
#define CARD_DETECT_DEBOUNCE_MS 50  // 50ms debounce time
static volatile uint32_t last_card_detect_time = 0;

// Button monitoring interval (check button every 100ms when idle)
#define BUTTON_CHECK_INTERVAL_MS 100
static absolute_time_t last_button_check_time;

/*
 * EXCLUSIVE GPIO interrupt handler 
 * Handles both REQ (time-critical) and CDET (debounced)
 */
void __not_in_flash_func(gpio_irq_exclusive_handler)(void) {
    uint32_t events_req = gpio_get_irq_event_mask(PIN_REQ);
    uint32_t events_cdet = gpio_get_irq_event_mask(PIN_CDET);
    
    // Handle REQ interrupt (time-critical, no debouncing)
    if (events_req) {
        gpio_acknowledge_irq(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
        
        if (events_req & GPIO_IRQ_EDGE_FALL) {
            // REQ went low - transfer starting
            req_triggered = true;
            
            // Disable card detect during transfer (matches AVR)
            card_detect_enabled = false;
        }
        
        if (events_req & GPIO_IRQ_EDGE_RISE) {
            // REQ went high - transfer ending
            // Re-enable card detect when idle
            card_detect_enabled = true;
        }
    }
    
    // Handle card detect interrupt (not time-critical, debounced)
    if (events_cdet) {
        // Always acknowledge first
        gpio_acknowledge_irq(PIN_CDET, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
        
        // Only process if enabled (not during transfer)
        if (!card_detect_enabled) {
            return;  // Ignore during transfer (but acknowledged)
        }
        
        // Debouncing: Ignore if too soon after last event
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((now - last_card_detect_time) < CARD_DETECT_DEBOUNCE_MS) {
            return;  // Too soon, ignore (mechanical bouncing)
        }
        last_card_detect_time = now;
        
        // Card inserted or removed - signal Amiga
        gpio_put(PIN_IRQ, false);
        gpio_set_dir(PIN_IRQ, true);
        
        // Brief pulse (10μs)
        busy_wait_us(10);
        
        // Release (back to input, pulled up externally)
        gpio_set_dir(PIN_IRQ, false);
        
        // Update state
        prev_cdet = gpio_get_all() & (1 << PIN_CDET);
    }
}

static void handle_request() {
    uint32_t pins;

    // Wait for REQ low - but now just check flag instead of busy-wait
    // (We were woken by interrupt)
    while (1) {
        pins = gpio_get_all();
        if (!(pins & (1 << PIN_REQ)))
            break;
        
        // Shouldn't get here if interrupt woke us, but handle race
        tight_loop_contents();
    }

    uint32_t prev_clk = pins & (1 << PIN_CLK);

    if ((pins & 0xc0) != 0xc0) {
        uint32_t byte_count = 0;
        bool read = false;

        if (!(pins & 0x80)) { // READ1 or WRITE1
            read = !!(pins & 0x40);
            byte_count = pins & 0x3f;
        } else { // READ2 or WRITE2
            byte_count = (pins & 0x3f) << 7;

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
            // WRITE operation
            while (1) {
                while (1) {
                    pins = gpio_get_all();
                    if ((pins & (1 << PIN_CLK)) != prev_clk)
                        break;

                    if (pins & (1 << PIN_REQ))
                        return;  // Aborted - flag NOT set
                }

                spi_get_hw(spi0)->dr = pins & 0xff;

                while (!spi_is_readable(spi0))
                    tight_loop_contents();

                (void)spi_get_hw(spi0)->dr;

                if (!byte_count)
                    break;

                prev_clk = pins & (1 << PIN_CLK);
                byte_count--;
            }
        }
    } else {
        switch ((pins & 0x3e) >> 1) {
            case 0: { // SPI_SELECT
                gpio_put(PIN_SS, !(pins & 1));
                break;
            }
            case 1: { // CARD_PRESENT
                gpio_set_dir(PIN_IRQ, false);

                while (1) {
                    pins = gpio_get_all();
                    if ((pins & (1 << PIN_CLK)) != prev_clk)
                        break;

                    if (pins & (1 << PIN_REQ))
                        return;
                }

                // Read actual card detect GPIO
                bool card_present = !gpio_get(PIN_CDET);
                
                gpio_put(PIN_D(0), card_present);
                gpio_set_dir_out_masked(0xff);
                break;
            }
            case 2: { // SPEED
                spi_set_baudrate(spi0, pins & 1 ?
                        SPI_FAST_FREQUENCY :
                        SPI_SLOW_FREQUENCY);
                break;
            }
        }
    }

    // Wait for REQ high
    while (1) {
        pins = gpio_get_all();
        if (pins & (1 << PIN_REQ))
            break;
    }
    
    // Re-enable card detect when idle (just in case)
    card_detect_enabled = true;
}

// ============================================================================
// Amiga Bridge Main (runs in Bare Metal Mode)
// Called from launch_bare_metal_mode() in main.c
// ============================================================================

void par_spi_main(void) {
    printf("Amiga SPI Bridge: Initializing on Core %d...\n", get_core_num());
    
    // Initialize SPI
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

    gpio_init(PIN_ACT);
    gpio_put(PIN_ACT, 1);
    
    gpio_init(PIN_IRQ);
    gpio_set_dir(PIN_IRQ, GPIO_IN);  // Input by default, pulsed as output for signals
    gpio_pull_up(PIN_IRQ);  // External pull-up exists, but enable internal too

    // === Initialize PIO for ACT mirroring (use PIO1, PIO0 used by WiFi) ===
    PIO pio = pio1;
    uint sm = 0;
    uint offset = pio_add_program(pio, &act_mirror_program);
    act_mirror_program_init(pio, sm, offset, PIN_REQ, PIN_ACT);

    prev_cdet = gpio_get_all() & (1 << PIN_CDET);

    // === Setup exclusive interrupt handler ===
    
    // Enable GPIO interrupts for both REQ and CDET
    gpio_set_irq_enabled(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(PIN_CDET, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    
    // Use exclusive handler for maximum speed
    irq_set_exclusive_handler(IO_IRQ_BANK0, gpio_irq_exclusive_handler);
    irq_set_priority(IO_IRQ_BANK0, 0);  // Highest priority
    irq_set_enabled(IO_IRQ_BANK0, true);

    printf("Amiga SPI Bridge: PIO1 ACT mirroring enabled\n");
    printf("Amiga SPI Bridge: Exclusive handler installed (fast interrupts ~200-300ns)\n");
    printf("Amiga SPI Bridge: Ready - waiting for Amiga requests\n");
    printf("Amiga SPI Bridge: Hold GPIO 13 button for 3 seconds to switch to FreeRTOS mode\n");
    
    // Initialize button check timer
    last_button_check_time = get_absolute_time();
    
    // Check if SD card is present and signal Amiga to mount it
    // This is important when switching back from FreeRTOS mode
    sleep_ms(100);  // Brief delay for hardware to stabilize
    
    bool card_present = !gpio_get(PIN_CDET);  // Active low
    if (card_present) {
        printf("Amiga SPI Bridge: SD card detected, signaling Amiga...\n");
        
        // CRITICAL: Initialize prev_cdet to "no card" to simulate insertion event
        prev_cdet = (1 << PIN_CDET);  // Set bit = no card (active low)
        
        // Send SINGLE SHORT pulse matching real card detect interrupt
        // (Not multiple long pulses - that confuses the Amiga driver)
        gpio_put(PIN_IRQ, false);      // IRQ low (active)
        gpio_set_dir(PIN_IRQ, true);   // Set to output
        
        busy_wait_us(10);              // Brief pulse (10μs)
        
        gpio_set_dir(PIN_IRQ, false);  // Release to input (pulled up externally)
        
        // Update prev_cdet to current state (card present)
        prev_cdet = gpio_get_all() & (1 << PIN_CDET);
        
        printf("Amiga SPI Bridge: Card presence signal sent\n");
    } else {
        printf("Amiga SPI Bridge: No SD card detected\n");
    }
    
    // Main loop - runs forever in Bare Metal mode
    // Watchdog reboot is triggered by 3-second button hold
    while (1) {
        req_triggered = false;
        
        // Wait for interrupt with timeout for button checking
        // Using best_effort_wfe_or_timeout instead of __wfe() to allow periodic button checks
        absolute_time_t timeout_time = make_timeout_time_ms(BUTTON_CHECK_INTERVAL_MS);
        best_effort_wfe_or_timeout(timeout_time);
        
        // Check if it's time to monitor button (every 100ms)
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last_button_check_time, now) >= (BUTTON_CHECK_INTERVAL_MS * 1000)) {
            // Defined in main.c - checks for 3-second button hold
            // If button held for 3+ seconds, triggers watchdog reboot to BOOT_MODE_FREERTOS
            extern void monitor_button_for_mode_switch(uint32_t current_mode);
            // BOOT_MODE_BARE_METAL is a #define from main.c, available via main.h
            monitor_button_for_mode_switch(BOOT_MODE_BARE_METAL);
            
            last_button_check_time = now;
        }
        
        if (req_triggered) {
            gpio_put(PIN_LED, 1);  // SPI activity LED on (GPIO 28)
            
            // Process the Amiga request
            handle_request();

            // Cleanup after transfer
            gpio_set_dir_in_masked(0xff);
            gpio_clr_mask(0xff);

            // Wait for SPI to finish
            while (spi_is_busy(spi0))
                tight_loop_contents();

            if (spi_is_readable(spi0))
                (void)spi_get_hw(spi0)->dr;
            
            gpio_put(PIN_LED, 0);  // SPI activity LED off
        }
    }
    
    // NOTE: This code never executes - watchdog reboot switches modes
}