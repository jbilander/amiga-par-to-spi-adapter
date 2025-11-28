/*
 * par_spi.c - Core 1: Amiga SPI Bridge (Amiga Mode Only)
 * VERSION: 2024-11-28-CARD-FIX
 * 
 * Based on jbilander's verified working version
 * Runs on Core 1 when in MODE_AMIGA
 * Uses exclusive interrupt handler for fast response (~200-300ns)
 * PIO1 for ACT mirroring
 * 
 * Changes for mode switching:
 * - Uses __wfe() instead of __wfi() with timer alarm
 * - Core 0 sends __sev() to wake Core 1
 * - Supports card_detect_override flag for clean unmounting
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

                // Check override flag first - if set, always report "not present"
                bool card_present;
                if (card_detect_override) {
                    card_present = false;  // Force "not present" during mode switch
                } else {
                    card_present = !gpio_get(PIN_CDET);  // Normal: read actual GPIO
                }
                
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
// Core 1 Entry Point - Work Loop (Amiga OR FTP)
// ============================================================================

void core1_entry(void) {
    printf("Core 1: Starting (will switch between Amiga and FTP modes)\n");
    
    while (1) {
        if (current_mode == MODE_AMIGA) {
            // Amiga mode - run the bridge
            printf("Core 1: Entering Amiga mode\n");
            par_spi_main();
            printf("Core 1: Exited Amiga mode\n");
        } else if (current_mode == MODE_WIFI) {
            // WiFi mode - run FTP server
            printf("Core 1: Entering WiFi/FTP mode\n");
            ftp_server_main();
            printf("Core 1: Exited WiFi/FTP mode\n");
        } else {
            // MODE_SWITCHING - just wait
            sleep_ms(100);
        }
    }
}

// ============================================================================
// Amiga Bridge Main (runs on Core 1 in Amiga mode)
// ============================================================================

void par_spi_main(void) {
    printf("Core 1: Initializing Amiga bridge...\n");
    
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

    printf("Core 1: PIO1 ACT mirroring enabled\n");
    printf("Core 1: Exclusive handler installed (fast interrupts ~200-300ns)\n");
    printf("Core 1: Amiga bridge ready\n");
    
    // Main loop - runs until mode switches back to WiFi
    // Using __wfe() (Wait For Event) which wakes on interrupts OR SEV from Core 0
    while (current_mode == MODE_AMIGA) {
        req_triggered = false;
        
        // Wait for interrupt OR event from Core 0
        // This wakes on: REQ interrupt, card detect interrupt, OR __sev() from Core 0
        __wfe();
        
        // Check if it was a mode change
        if (current_mode != MODE_AMIGA) {
            printf("Core 1: DEBUG - Mode changed detected! current_mode=%d\n", current_mode);
            break;  // Exit cleanly
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

    // Exiting Amiga mode - cleanup
    printf("Core 1: Cleaning up Amiga bridge...\n");
    
    // Disable interrupts
    gpio_set_irq_enabled(PIN_REQ, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    gpio_set_irq_enabled(PIN_CDET, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    irq_set_enabled(IO_IRQ_BANK0, false);
    irq_remove_handler(IO_IRQ_BANK0, gpio_irq_exclusive_handler);
    
    // Disable PIO
    pio_sm_set_enabled(pio, sm, false);
    pio_remove_program(pio, &act_mirror_program, offset);
    
    printf("Core 1: PIO disabled\n");
    
    // Deinitialize SPI to avoid conflicts with WiFi
    // NOTE: Commenting this out for testing - may be causing issues
    // spi_deinit(spi0);
    printf("Core 1: SPI deinit skipped (for testing)\n");
    
    printf("Core 1: Cleanup complete\n");
}