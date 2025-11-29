/* main.c - Pico 2 W Amiga SPI Bridge with WiFi/FTP Mode Switching
 * VERSION: 2024-11-29-FREERTOS
 * 
 * Hardware: Raspberry Pi Pico 2 W (RP2350)
 * 
 * Two Modes (via watchdog reset):
 * - MODE_AMIGA: Core 1 runs Amiga SPI bridge (par_spi.c) - strict timing, no FreeRTOS
 * - MODE_WIFI:  Core 1 runs FTP server with FreeRTOS - socket API enabled
 * 
 * Mode switch: GPIO 13 button (50ms debounce)
 * 
 * Based on Niklas Ekström's October 2022 RP2040 version
 * Adapted for Pico 2 W by jbilander
 */

#include "main.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/mutex.h"
#include "hardware/watchdog.h"
#include "hardware/gpio.h"
#include "pico/cyw43_arch.h"
#include <stdio.h>
#include <string.h>

// FreeRTOS includes (only used in WiFi mode)
#include "FreeRTOS.h"
#include "task.h"

// FTP Server
#include "ftp_server.h"
#include "ff.h"

// ============================================================================
// Global Variables
// ============================================================================

// SPI Mutex (for FatFS library dependency)
mutex_t spi_mutex;

// Current operating mode (set at boot from watchdog scratch register)
volatile system_mode_t current_mode = MODE_AMIGA;

// Card detect override flag (used in Amiga mode to prevent corruption during mode switch)
volatile bool card_detect_override = false;

// ============================================================================
// Watchdog Magic Values (stored in scratch registers across reset)
// ============================================================================

#define BOOT_MODE_AMIGA_MAGIC   0xA3165A00
#define BOOT_MODE_WIFI_MAGIC    0xF1F00000

// ============================================================================
// Version String
// ============================================================================

#define VERSION_STRING "2024-11-29-FREERTOS"

// ============================================================================
// GPIO Button Debouncing
// ============================================================================

#define BUTTON_DEBOUNCE_MS 50

static uint32_t last_button_press = 0;

// ============================================================================
// Core 1 Entry Point (launched from main)
// ============================================================================

void core1_entry(void) {
    if (current_mode == MODE_AMIGA) {
        // Amiga mode: Run SPI bridge with exclusive IRQ handling
        // NO FreeRTOS - strict timing preserved (~200-300ns response)
        par_spi_main();
    } else {
        // WiFi mode: Run FTP server with FreeRTOS
        // FreeRTOS scheduler starts here (vTaskStartScheduler)
        ftp_server_main();
    }
}

// ============================================================================
// Mode Switching Functions
// ============================================================================

void switch_to_amiga_mode(void) {
    printf("\n=== Switching to Amiga Mode ===\n");
    
    if (current_mode == MODE_WIFI) {
        // Disconnect WiFi cleanly
        printf("Disconnecting WiFi...\n");
        cyw43_arch_deinit();
    }
    
    // Set card detect override to prevent Amiga from accessing card during reboot
    card_detect_override = true;
    printf("Card detect override: ENABLED (Amiga will see card as NOT PRESENT)\n");
    
    // Send multiple IRQ pulses to ensure Amiga unmounts
    printf("Sending IRQ pulses to Amiga to force unmount...\n");
    for (int i = 0; i < 5; i++) {
        gpio_put(PIN_IRQ, 0);
        sleep_ms(100);
        gpio_put(PIN_IRQ, 1);
        sleep_ms(100);
    }
    
    printf("Waiting for Amiga to unmount...\n");
    sleep_ms(500);
    
    // Store magic value in watchdog scratch register
    watchdog_hw->scratch[0] = BOOT_MODE_AMIGA_MAGIC;
    printf("Watchdog scratch set: AMIGA mode\n");
    
    // Trigger watchdog reset
    printf("Triggering watchdog reset...\n");
    watchdog_reboot(0, 0, 10);
    
    // Wait for reset (should not return)
    while (1) {
        tight_loop_contents();
    }
}

void switch_to_wifi_mode(void) {
    printf("\n=== Switching to WiFi Mode ===\n");
    
    // Set card detect override to prevent Amiga from accessing card during reboot
    card_detect_override = true;
    printf("Card detect override: ENABLED (Amiga will see card as NOT PRESENT)\n");
    
    // Send multiple IRQ pulses to ensure Amiga unmounts
    printf("Sending IRQ pulses to Amiga to force unmount...\n");
    for (int i = 0; i < 5; i++) {
        gpio_put(PIN_IRQ, 0);
        sleep_ms(100);
        gpio_put(PIN_IRQ, 1);
        sleep_ms(100);
    }
    
    printf("Waiting for Amiga to unmount...\n");
    sleep_ms(500);
    
    // Store magic value in watchdog scratch register
    watchdog_hw->scratch[0] = BOOT_MODE_WIFI_MAGIC;
    printf("Watchdog scratch set: WIFI mode\n");
    
    // Trigger watchdog reset
    printf("Triggering watchdog reset...\n");
    watchdog_reboot(0, 0, 10);
    
    // Wait for reset (should not return)
    while (1) {
        tight_loop_contents();
    }
}

// ============================================================================
// Button Handler (Core 0 - Mode Manager)
// ============================================================================

static void handle_button_press(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    
    // Debounce
    if (now - last_button_press < BUTTON_DEBOUNCE_MS) {
        return;
    }
    last_button_press = now;
    
    // Read button state
    bool button_pressed = !gpio_get(PIN_MODE_SW);  // Active low (internal pull-up)
    
    if (!button_pressed) {
        return;  // Button not pressed
    }
    
    printf("\nButton pressed! Switching mode...\n");
    
    if (current_mode == MODE_AMIGA) {
        switch_to_wifi_mode();
    } else {
        switch_to_amiga_mode();
    }
}

// ============================================================================
// Core 0: Mode Manager (runs in both modes)
// ============================================================================

static void core0_mode_manager(void) {
    printf("Core 0: Mode manager starting...\n");
    
    // Main loop - monitor button
    while (1) {
        handle_button_press();
        
        if (current_mode == MODE_AMIGA) {
            // Amiga mode - just sleep (LED on GPIO 28 handled by par_spi.c)
            sleep_ms(1000);
        } else {
            // WiFi mode - blink WiFi LED
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            sleep_ms(100);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            sleep_ms(100);
        }
    }
}

// ============================================================================
// WiFi Connection (Core 0 - WiFi Mode Only)
// ============================================================================

static bool connect_wifi(void) {
    printf("Connecting to WiFi (SSID: %s)...\n", WIFI_SSID);
    
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, 
                                            CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("ERROR: Failed to connect to WiFi\n");
        return false;
    }
    
    printf("WiFi connected!\n");
    printf("IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    
    return true;
}

// ============================================================================
// FTP Server Main (Core 1 - WiFi Mode Only)
// ============================================================================

// FTP server instance
static ftp_server_t ftp_server;
static FATFS fs;

// FTP task (runs as FreeRTOS task)
static void ftp_task(__unused void *params) {
    printf("FTP Task: Starting FTP server...\n");
    
    // Mount SD card filesystem
    printf("FTP Task: Mounting SD card filesystem...\n");
    FRESULT fr = f_mount(&fs, "", 1);
    
    if (fr != FR_OK) {
        printf("FTP Task: ERROR - Failed to mount SD card (FR=%d)\n", fr);
        printf("FTP Task: FTP server cannot start without SD card\n");
        
        // Flash LED fast to indicate error
        while (current_mode == MODE_WIFI) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        f_mount(NULL, "", 0);
        vTaskDelete(NULL);
        return;
    }
    
    printf("FTP Task: SD card mounted successfully\n");
    
    // Initialize FTP server
    if (!ftp_server_init(&ftp_server, &fs)) {
        printf("FTP Task: ERROR - Failed to initialize FTP server\n");
        f_mount(NULL, "", 0);
        vTaskDelete(NULL);
        return;
    }
    
    // Add users
    ftp_server_add_user(&ftp_server, "amiga", "amiga");
    ftp_server_add_user(&ftp_server, "admin", "admin");
    
    // Start FTP server
    if (!ftp_server_begin(&ftp_server)) {
        printf("FTP Task: ERROR - Failed to start FTP server\n");
        f_mount(NULL, "", 0);
        vTaskDelete(NULL);
        return;
    }
    
    printf("FTP Task: FTP server ready on port 21\n");
    printf("FTP Task: Users: amiga/amiga, admin/admin\n");
    
    // Main FTP Loop (runs forever as FreeRTOS task)
    while (current_mode == MODE_WIFI) {
        ftp_server_handle(&ftp_server);
        
        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Cleanup when exiting WiFi mode (mode switch detected)
    printf("FTP Task: Cleaning up FTP server...\n");
    ftp_server_stop(&ftp_server);
    
    printf("FTP Task: Unmounting SD card...\n");
    f_mount(NULL, "", 0);
    
    printf("FTP Task: FTP task ending\n");
    vTaskDelete(NULL);
}

void ftp_server_main(void) {
    printf("Core 1: Initializing WiFi mode with FreeRTOS...\n");
    
    // CRITICAL: FreeRTOS is ONLY initialized here, in WiFi mode!
    // When in Amiga mode, par_spi_main() runs instead with no FreeRTOS.
    
    printf("Core 1: Creating FTP server task...\n");
    
    // Create FTP server task
    xTaskCreate(
        ftp_task,           // Task function
        "FTP_Server",       // Task name
        2048,              // Stack size (words)
        NULL,              // Parameters
        1,                 // Priority
        NULL               // Task handle
    );
    
    printf("Core 1: Starting FreeRTOS scheduler (WiFi mode only)...\n");
    
    // Start FreeRTOS scheduler - THIS NEVER RETURNS!
    // The scheduler will run the FTP task and handle lwIP
    vTaskStartScheduler();
    
    // Should never reach here
    printf("Core 1: ERROR - FreeRTOS scheduler returned!\n");
    while (1) {
        tight_loop_contents();
    }
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main() {
    // Initialize stdio
    stdio_init_all();
    sleep_ms(2000);  // Wait for USB serial

    // Initialize SPI mutex (for FatFS)
    mutex_init(&spi_mutex);
    
    printf("\n\n");
    printf("============================================================\n");
    printf("  Pico 2 W - Amiga SPI Bridge / WiFi FTP Server\n");
    printf("  VERSION: %s\n", VERSION_STRING);
    printf("============================================================\n");
    
    // Check watchdog scratch register to determine boot mode
    uint32_t mode_magic = watchdog_hw->scratch[0];
    
    if (mode_magic == BOOT_MODE_WIFI_MAGIC) {
        current_mode = MODE_WIFI;
        printf("Boot mode: WiFi (from watchdog scratch register)\n");
    } else {
        current_mode = MODE_AMIGA;
        printf("Boot mode: Amiga (default or from watchdog scratch register)\n");
    }
    
    // Clear watchdog scratch register
    watchdog_hw->scratch[0] = 0;
    
    // Initialize GPIO button (internal pull-up, active low)
    printf("Initializing GPIO %d button...\n", PIN_MODE_SW);
    gpio_init(PIN_MODE_SW);
    gpio_set_dir(PIN_MODE_SW, GPIO_IN);
    gpio_pull_up(PIN_MODE_SW);
    
    // Mode-specific initialization
    if (current_mode == MODE_WIFI) {
        printf("\n=== WiFi Mode ===\n");
        
        // Initialize WiFi chip (ONLY in WiFi mode - requires FreeRTOS)
        printf("Initializing WiFi chip...\n");
        if (cyw43_arch_init()) {
            printf("ERROR: Failed to initialize WiFi chip\n");
            return 1;
        }
        
        // Connect to WiFi
        if (!connect_wifi()) {
            printf("ERROR: WiFi connection failed, cannot start FTP server\n");
            // Flash LED rapidly to indicate error
            while (1) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
                sleep_ms(100);
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
                sleep_ms(100);
            }
        }
        
        printf("\nCore 0: WiFi mode manager starting...\n");
        printf("Press GPIO %d button to switch to Amiga mode\n\n", PIN_MODE_SW);
        
    } else {
        printf("\n=== Amiga Mode ===\n");
        printf("Core 0: Amiga mode manager starting...\n");
        printf("Press GPIO %d button to switch to WiFi mode\n\n", PIN_MODE_SW);
    }
    
    // Launch Core 1
    printf("Launching Core 1...\n");
    multicore_launch_core1(core1_entry);
    
    // Core 0 continues with mode manager
    core0_mode_manager();
    
    // Should never reach here
    return 0;
}