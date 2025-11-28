/* main.c - Core 0: WiFi management and mode switching */

#include "main.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "lwip/netif.h"

// ============================================================================
// Boot Mode Magic Values (stored in watchdog scratch registers)
// ============================================================================

#define BOOT_MODE_WIFI_MAGIC  0x57494649  // "WIFI" in ASCII
#define BOOT_MODE_AMIGA_MAGIC 0x414D4947  // "AMIG" in ASCII

// ============================================================================
// Shared State Variables
// ============================================================================

volatile system_mode_t current_mode = MODE_AMIGA;  // Start in Amiga mode
volatile led_pattern_t current_led_pattern = LED_AMIGA_MODE;
volatile bool amiga_wrote_to_card = false;
volatile bool mode_button_pressed = false;
volatile bool core1_done = true;  // Core 1 ready initially
volatile bool card_detect_override = false;  // Override card detect (force "not present")

mutex_t spi_mutex;

// ============================================================================
// Mode Toggle Button (GPIO 13) with Debouncing
// ============================================================================

bool button_timer_callback(struct repeating_timer *t) {
    static bool initialized = false;
    static bool last_state = true;  // Default to high (pulled up)
    static uint32_t last_change_time = 0;
    
    bool current_state = gpio_get(PIN_MODE_SW);  // Active low (pulled up)
    uint32_t now = to_ms_since_boot(get_absolute_time());
    
    // On first call, initialize last_state from actual GPIO
    if (!initialized) {
        last_state = current_state;
        initialized = true;
        return true;  // Continue timer
    }
    
    // Detect state change
    if (current_state != last_state) {
        // Check if enough time has passed since last change (debounce)
        if ((now - last_change_time) >= BUTTON_DEBOUNCE_MS) {
            // Valid state change - update timestamp
            last_change_time = now;
            
            // Trigger on button press (falling edge - goes LOW)
            if (!current_state && last_state) {
                mode_button_pressed = true;
            }
            
            last_state = current_state;
        }
        // If not enough time passed, ignore (still bouncing)
    }
    
    return true;  // Keep timer running
}

// ============================================================================
// LED Update Timer Callback
// ============================================================================

bool led_update_timer_callback(struct repeating_timer *t) {
    update_led_pattern();
    return true;  // Keep timer running
}

// ============================================================================
// LED Pattern Update (using CYW43_WL_GPIO_LED_PIN)
// ============================================================================

void update_led_pattern(void) {
    // Only update LED if in WiFi mode (CYW43 initialized)
    if (current_mode != MODE_WIFI && current_mode != MODE_SWITCHING) {
        return;  // Amiga mode - CYW43 not initialized, skip LED updates
    }
    
    static uint32_t last_toggle = 0;
    static bool led_state = false;
    uint32_t now = time_us_32();
    
    switch (current_led_pattern) {
        case LED_STARTUP:
            // Solid ON
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            break;
            
        case LED_WIFI_CONNECTING:
            // Fast blink
            if (now - last_toggle > (LED_WIFI_CONNECTING_MS * 1000)) {
                led_state = !led_state;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
                last_toggle = now;
            }
            break;
            
        case LED_WIFI_CONNECTED:
            // Slow blink
            if (now - last_toggle > (LED_WIFI_CONNECTED_MS * 1000)) {
                led_state = !led_state;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
                last_toggle = now;
            }
            break;
            
        case LED_WIFI_FAILED:
            // Very fast blink
            if (now - last_toggle > (LED_WIFI_FAILED_MS * 1000)) {
                led_state = !led_state;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
                last_toggle = now;
            }
            break;
            
        case LED_AMIGA_MODE:
            // OFF (but we shouldn't get here in Amiga mode)
            break;
            
        case LED_MODE_SWITCHING:
            // Handled separately
            break;
    }
}

// ============================================================================
// Mode Switch Visual Feedback
// ============================================================================

void indicate_mode_switch(void) {
    // Only flash LED if CYW43 is initialized (switching FROM WiFi mode)
    // If switching FROM Amiga mode, CYW43 not initialized yet - skip LED flashes
    if (current_mode == MODE_WIFI) {
        for (int i = 0; i < LED_MODE_SWITCH_COUNT; i++) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            sleep_ms(LED_MODE_SWITCH_FLASH_MS);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            sleep_ms(LED_MODE_SWITCH_FLASH_MS);
        }
    } else {
        // Amiga mode - CYW43 not initialized, just delay
        printf("Core 0: Mode switching (LED not available in Amiga mode)\n");
        sleep_ms(LED_MODE_SWITCH_FLASH_MS * LED_MODE_SWITCH_COUNT * 2);
    }
}

// ============================================================================
// Card Detection Signaling
// ============================================================================

void signal_card_change(void) {
    // Pulse IRQ to notify Amiga of card status change
    // Amiga will check card detect line to determine current state
    gpio_put(PIN_IRQ, false);
    gpio_set_dir(PIN_IRQ, true);   // Drive low
    busy_wait_us(10);               // 10μs pulse
    gpio_set_dir(PIN_IRQ, false);  // Back to input (pulled up externally)
}

// ============================================================================
// Mode Switching Functions (called from Core 0)
// ============================================================================

void switch_to_amiga_mode(void) {
    printf("\n=================================\n");
    printf("Switching to Amiga mode...\n");
    printf("=================================\n");
    
    indicate_mode_switch();
    
    // Signal card change to Amiga (card removal)
    printf("Core 0: Signaling card removal to Amiga\n");
    signal_card_change();
    sleep_ms(100);
    
    // Save desired mode to watchdog scratch register
    printf("Core 0: Saving Amiga mode preference\n");
    watchdog_hw->scratch[0] = BOOT_MODE_AMIGA_MAGIC;
    
    // Give time for message to print
    sleep_ms(500);
    
    // Reboot the Pico
    printf("Core 0: Rebooting to Amiga mode...\n");
    sleep_ms(100);  // Let message print
    
    watchdog_reboot(0, 0, 100);  // Reboot in 100ms
    while(1) {
        tight_loop_contents();  // Wait for reboot
    }
}

void switch_to_wifi_mode(void) {
    printf("\n=================================\n");
    printf("Switching to WiFi mode...\n");
    printf("=================================\n");
    
    indicate_mode_switch();
    
    // Override card detect to report "not present" even if card is physically there
    card_detect_override = true;
    printf("Core 0: Card detect override enabled (will report 'not present')\n");
    
    // Signal card change to Amiga (card removal)
    printf("Core 0: Signaling card removal to Amiga\n");
    signal_card_change();
    sleep_ms(100);
    
    // Pulse IRQ a few more times to ensure Amiga sees it
    printf("Core 0: Sending additional card removal pulses...\n");
    for (int i = 0; i < 3; i++) {
        signal_card_change();
        sleep_ms(50);
    }
    
    // Save desired mode to watchdog scratch register
    printf("Core 0: Saving WiFi mode preference\n");
    watchdog_hw->scratch[0] = BOOT_MODE_WIFI_MAGIC;
    
    // Give time for Amiga to process card removal
    sleep_ms(500);
    
    // Reboot the Pico
    printf("Core 0: Rebooting to WiFi mode...\n");
    sleep_ms(100);  // Let message print
    
    watchdog_reboot(0, 0, 100);  // Reboot in 100ms
    while(1) {
        tight_loop_contents();  // Wait for reboot
    }
}

// ============================================================================
// Mode button indicator (LED flashes)
// ============================================================================
    

// ============================================================================
// Core 0 Entry Point - Mode Management
// ============================================================================

void core0_entry(void) {
    printf("Core 0: Mode manager ready\n");
    
    while (1) {
        // Handle mode button press
        if (mode_button_pressed) {
            mode_button_pressed = false;
            
            if (current_mode == MODE_AMIGA) {
                switch_to_wifi_mode();
            } else if (current_mode == MODE_WIFI) {
                switch_to_amiga_mode();
            }
        }
        
        // Check for Amiga writes (Core 1 sets this flag in either mode)
        if (amiga_wrote_to_card && current_mode == MODE_WIFI) {
            mutex_enter_blocking(&spi_mutex);
            
            printf("Core 0: Filesystem changed by Amiga, remounting...\n");
            // TODO: Remount filesystem
            // f_unmount("0:");
            // f_mount(&fs, "0:", 1);
            amiga_wrote_to_card = false;
            
            mutex_exit(&spi_mutex);
        }
        
        sleep_ms(50);  // Check every 50ms
    }
}

// ============================================================================
// Main - Initialize WiFi on Core 0, Launch Cores
// ============================================================================

int main() {
    stdio_init_all();
    
    // VERSION IDENTIFIER
    printf("\n╔════════════════════════════════════════════════════════════╗\n");
    printf("║  Pico 2 W Amiga Bridge - VERSION: 2024-11-28-CARD-FIX   ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n\n");
    
    // ========================================================================
    // Check if we're rebooting for mode switch
    // ========================================================================
    
    uint32_t boot_mode = watchdog_hw->scratch[0];
    watchdog_hw->scratch[0] = 0;  // Clear the flag
    
    if (boot_mode == BOOT_MODE_WIFI_MAGIC) {
        printf("Boot: Mode switch detected → Starting in WiFi mode\n");
        current_mode = MODE_WIFI;
        current_led_pattern = LED_WIFI_CONNECTING;
        card_detect_override = false;  // Clear override (not in Amiga mode)
    } else if (boot_mode == BOOT_MODE_AMIGA_MAGIC) {
        printf("Boot: Mode switch detected → Starting in Amiga mode\n");
        current_mode = MODE_AMIGA;
        current_led_pattern = LED_AMIGA_MODE;
        card_detect_override = false;  // Clear override (normal card detection)
    } else {
        // Default mode (fresh boot)
        printf("Boot: Fresh boot → Starting in Amiga mode (default)\n");
        current_mode = MODE_AMIGA;
        current_led_pattern = LED_AMIGA_MODE;
        card_detect_override = false;  // Clear override (normal card detection)
    }
    
    // Initialize SPI activity LED (GPIO 28)
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);
    
    sleep_ms(3000);  // USB enumeration
    
    printf("\n");
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║   Pico 2 W Amiga SPI Bridge v2.0         ║\n");
    printf("║   Watchdog Reset Mode Switching          ║\n");
    printf("╚═══════════════════════════════════════════╝\n");
    printf("\n");
    
    // ========================================================================
    // Initialize WiFi if starting in WiFi mode
    // ========================================================================
    
    if (current_mode == MODE_WIFI) {
        printf("Core 0: Starting WiFi initialization...\n");
        
        if (cyw43_arch_init()) {
            printf("Core 0: WiFi init failed! Falling back to Amiga mode\n");
            current_mode = MODE_AMIGA;
            current_led_pattern = LED_AMIGA_MODE;
        } else {
            cyw43_arch_enable_sta_mode();
            printf("Core 0: Connecting to %s...\n", WIFI_SSID);
            
            if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                                   CYW43_AUTH_WPA2_AES_PSK, 30000)) {
                printf("Core 0: Connection failed\n");
                current_led_pattern = LED_WIFI_FAILED;
            } else {
                printf("Core 0: Connected!\n");
                struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
                printf("Core 0: IP address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));
                current_led_pattern = LED_WIFI_CONNECTED;
            }
        }
    } else {
        printf("Core 0: Amiga mode - WiFi OFF (CYW43 not initialized)\n");
    }
    
    // Display current mode
    printf("\n");
    if (current_mode == MODE_AMIGA) {
        printf("Core 0: AMIGA MODE active\n");
        printf("Core 0: Press GPIO 13 button to switch to WiFi\n");
    } else {
        printf("Core 0: WIFI MODE active\n");
        printf("Core 0: Press GPIO 13 button to switch to Amiga\n");
    }
    
    // Setup LED update timer
    static repeating_timer_t led_timer;
    add_repeating_timer_ms(50, led_update_timer_callback, NULL, &led_timer);
    
    // Setup mode toggle button (GPIO 13)
    gpio_init(PIN_MODE_SW);
    gpio_set_dir(PIN_MODE_SW, GPIO_IN);
    gpio_pull_up(PIN_MODE_SW);  // Pull-up, button connects to ground
    
    // Wait for GPIO to settle
    sleep_ms(100);
    
    // Initialize button state - read actual GPIO state
    // This prevents false trigger on startup
    mode_button_pressed = false;
    
    // Start button polling timer
    static repeating_timer_t button_timer;
    add_repeating_timer_ms(50, button_timer_callback, NULL, &button_timer);
    
    printf("\n");
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║          LED Status Indicators           ║\n");
    printf("╠═══════════════════════════════════════════╣\n");
    printf("║  WiFi LED (built-in):                    ║\n");
    printf("║    Not initialized in Amiga mode         ║\n");
    printf("║    Slow blink  = WiFi mode (FTP ready)   ║\n");
    printf("║    Fast blink  = WiFi connecting         ║\n");
    printf("║    6 flashes   = Mode switching (WiFi)   ║\n");
    printf("║                                           ║\n");
    printf("║  SPI LED (GPIO 28):                      ║\n");
    printf("║    Shows Amiga SPI activity              ║\n");
    printf("╠═══════════════════════════════════════════╣\n");
    printf("║  Press button (GPIO 13) to toggle modes  ║\n");
    printf("╚═══════════════════════════════════════════╝\n");
    printf("\n");
    
    // Initialize SPI mutex
    printf("Core 0: Initializing SPI mutex...\n");
    mutex_init(&spi_mutex);
    
    // Launch Core 1 (will run either Amiga bridge OR FTP server)
    printf("Core 0: Launching Core 1 (Amiga/FTP worker)...\n");
    multicore_launch_core1(core1_entry);
    sleep_ms(200);
    
    // If starting in Amiga mode (after mode switch), signal card insertion
    if (current_mode == MODE_AMIGA && boot_mode == BOOT_MODE_AMIGA_MAGIC) {
        printf("Core 0: Signaling card insertion to Amiga...\n");
        sleep_ms(500);  // Give Amiga bridge time to fully initialize
        signal_card_change();
        sleep_ms(100);
        // Send a few more pulses to ensure Amiga sees it
        for (int i = 0; i < 2; i++) {
            signal_card_change();
            sleep_ms(50);
        }
        printf("Core 0: Card insertion signaled\n");
    }
    
    // Display mode-specific startup message
    printf("\n");
    printf("═══════════════════════════════════════════\n");
    if (current_mode == MODE_AMIGA) {
        printf(" System Ready - Amiga Mode Active\n");
        printf(" Core 0: Mode management\n");
        printf(" Core 1: Amiga bridge\n");
        printf(" Press GPIO 13 button to enable WiFi\n");
    } else {
        printf(" System Ready - WiFi Mode Active\n");
        printf(" Core 0: WiFi management\n");
        printf(" Core 1: FTP server\n");
        printf(" Press GPIO 13 button to switch to Amiga\n");
    }
    printf("═══════════════════════════════════════════\n");
    printf("\n");
    
    // Run mode manager on Core 0
    core0_entry();  // Never returns
    
    return 0;
}