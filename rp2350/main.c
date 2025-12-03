#include "main.h"
#include <stdlib.h>
#include <pico/stdlib.h>
#include <pico/stdio.h>
#include <pico/multicore.h>
#include <pico/cyw43_arch.h>
#include <hardware/watchdog.h>
#include <hardware/sync.h>
#include <FreeRTOS.h>
#include <task.h>
#include <lwip/netif.h>

// Signal interrupt to Amiga before mode switch
// Sends single short IRQ pulse to notify Amiga that card state will change
void signal_interrupt_to_amiga(void) {
    printf("Signaling interrupt to Amiga...\n");
    
    // Initialize IRQ pin if not already done
    gpio_init(PIN_IRQ);
    
    // Send SINGLE SHORT pulse matching real card detect interrupt
    gpio_put(PIN_IRQ, false);      // IRQ low (active)
    gpio_set_dir(PIN_IRQ, true);   // Set to output
    
    busy_wait_us(10);              // Brief pulse (10μs) - matches real interrupt
    
    gpio_set_dir(PIN_IRQ, false);  // Release to input (pulled up externally)
    
    // Wait for Amiga to process and unmount
    printf("Waiting for Amiga to unmount...\n");
    sleep_ms(500);
}

void trigger_reboot_to_mode(uint32_t mode_flag) {
    // Signal interrupt BEFORE disabling interrupts
    // (This needs interrupts enabled for sleep_ms to work)
    signal_interrupt_to_amiga();
    
    // Disable interrupts while accessing critical hardware
    uint32_t status = save_and_disable_interrupts(); 
    
    // Write the desired boot mode flag to the non-volatile scratch register
    *BOOT_FLAG_ADDR = mode_flag;
    
    restore_interrupts(status);
    
    printf("Triggering watchdog reboot to mode 0x%lX...\n", mode_flag);
    
    // Set watchdog to trigger in 100ms
    watchdog_enable(100, 1); 
    while(1); // Wait for reboot
}

void monitor_button_for_mode_switch(uint32_t current_mode) {
    static bool button_pressed_prev = false;
    static absolute_time_t press_start_time;
    static bool reboot_triggered = false;  // Prevent multiple triggers

    // Read the current state of the button
    // Assuming the button connects GPIO 13 to GND when pressed (using internal pull-up)
    bool button_pressed_now = !gpio_get(PIN_MODE_SW); 

    if (button_pressed_now && !button_pressed_prev) {
        // Button just pressed: start the timer
        press_start_time = get_absolute_time();
        reboot_triggered = false;  // Reset flag on new button press
        
    } else if (button_pressed_now && button_pressed_prev) {
        // Button is being held down: check duration
        int64_t held_duration_ms = absolute_time_diff_us(press_start_time, get_absolute_time()) / 1000;

        if (held_duration_ms >= 3000 && !reboot_triggered) {
            // Button held for 3 seconds - trigger ONCE
            reboot_triggered = true;  // Set flag to prevent re-trigger
            
            printf("Button held for 3+ seconds! Invoking reboot.\n");
            uint32_t next_mode = (current_mode == BOOT_MODE_FREERTOS) ? BOOT_MODE_BARE_METAL : BOOT_MODE_FREERTOS;
            trigger_reboot_to_mode(next_mode);
            // Note: Should never return from trigger_reboot_to_mode (watchdog reboot)
        }
        
    } else if (!button_pressed_now && button_pressed_prev) {
        // Button released (for completeness - rarely used since reboot happens)
        reboot_triggered = false;
    }
    
    button_pressed_prev = button_pressed_now;
}

// -----------------------------------------------------------
// BARE METAL MODE (No WiFi needed here)
// -----------------------------------------------------------
void launch_bare_metal_mode() {
    printf("Entering bare metal mode (Core %d, WiFi Disabled).\n", get_core_num());
    
    // Launch Amiga SPI bridge
    par_spi_main();
    
    // Should never reach here
    while (1) {
        sleep_ms(1000);
    }
}

// -----------------------------------------------------------
// FREERTOS MODE (Uses WiFi/FTP/RAW LWIP API)
// -----------------------------------------------------------

// Updated ftp_server_application_task for main.c
// Replace the existing function with this version

void ftp_server_application_task(void *pvParameters) {
    printf("FTP Task: Starting on Core %d\n", get_core_num());
    
    // Brief delay to ensure WiFi is fully ready
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Initialize FTP server
    if (!ftp_server_init()) {
        printf("FTP Task: Failed to initialize FTP server!\n");
        // Stay in loop but don't process
        while (1) {
            monitor_button_for_mode_switch(BOOT_MODE_FREERTOS);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    printf("FTP Task: FTP server ready for connections\n");
    
    // Main FTP server loop
    while (1) {
        // Process FTP server (callbacks handle actual work)
        ftp_server_process();
        
        // Monitor button for mode switch
        monitor_button_for_mode_switch(BOOT_MODE_FREERTOS);
        
        // Small delay
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// --- FTP Server/WiFi Management Task ---
void wifi_management_task(void *pvParameters) {
    printf("WiFi Management Task: Starting on Core %d\n", get_core_num());
    
    // Brief delay for stability
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // ========================================================================
    // STEP 1: Initialize WiFi Chip
    // ========================================================================
    printf("WiFi: Initializing CYW43 chip...\n");
    
    if (cyw43_arch_init()) {
        printf("WiFi: ERROR - Failed to initialize CYW43 chip!\n");
        printf("WiFi: Fast blinking LED indicates initialization failure\n");
        
        // Fast blink = hardware failure
        while (1) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
            
            // Still monitor button even during error
            monitor_button_for_mode_switch(BOOT_MODE_FREERTOS);
        }
    }
    
    printf("WiFi: CYW43 chip initialized successfully\n");
    
    // Turn LED ON (solid) to indicate FreeRTOS mode active
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    printf("WiFi: LED solid on - FreeRTOS mode active\n");
    
    // ========================================================================
    // STEP 2: Enable WiFi Station Mode
    // ========================================================================
    printf("WiFi: Enabling station mode...\n");
    cyw43_arch_enable_sta_mode();
    
    // Brief pause with LED solid before starting connection
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // ========================================================================
    // STEP 3: Connect to WiFi Network
    // ========================================================================
    printf("WiFi: Connecting to network '%s'...\n", WIFI_SSID);
    printf("WiFi: Medium blinking LED indicates connecting\n");
    
    // Blink while connecting (non-blocking with timeout)
    uint32_t connect_attempts = 0;
    const uint32_t max_attempts = 3;  // 30 seconds timeout
    
    while (connect_attempts < max_attempts) {
        // Try non-blocking connect (1 second timeout per attempt)
        int result = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, 
            WIFI_PASSWORD,
            CYW43_AUTH_WPA2_AES_PSK, 
            30000  // 30 second per attempt
        );
        
        if (result == 0) {
            // Connected successfully!
            break;
        }
        
        // Blink LED while connecting
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_CONNECT_MS));
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_CONNECT_MS));
        
        connect_attempts++;
        
        if (connect_attempts % 5 == 0) {
            printf("WiFi: Still connecting... (attempt %ld/%ld)\n", connect_attempts, max_attempts);
        }
    }
    
    // Check if connection succeeded
    if (connect_attempts >= max_attempts) {
        printf("WiFi: ERROR - Failed to connect after %ld attempts\n", max_attempts);
        printf("WiFi: Fast blinking LED indicates connection failure\n");
        printf("WiFi: Check SSID/password in wifi_credentials.cmake\n");
        
        // Fast blink = connection failure
        while (1) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
            
            // Still monitor button even during error
            monitor_button_for_mode_switch(BOOT_MODE_FREERTOS);
        }
    }
    
    // ========================================================================
    // STEP 4: WiFi Connected Successfully!
    // ========================================================================
    printf("WiFi: Connected successfully!\n");
    printf("WiFi: IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    printf("WiFi: Netmask:    %s\n", ip4addr_ntoa(netif_ip4_netmask(netif_list)));
    printf("WiFi: Gateway:    %s\n", ip4addr_ntoa(netif_ip4_gw(netif_list)));
    printf("WiFi: Slow blinking LED indicates connected\n");
    
    // ========================================================================
    // STEP 5: Create FTP Server Task (on Core 1)
    // ========================================================================
    printf("WiFi: Creating FTP server task on Core 1...\n");
    
    xTaskCreateAffinitySet(
        ftp_server_application_task, 
        "FTPTaskCore1", 
        configMINIMAL_STACK_SIZE + 4096, 
        NULL,
        2,
        CORE_1_AFFINITY_MASK,
        NULL
    );
    
    printf("WiFi: FTP server task created\n");
    
    // ========================================================================
    // STEP 6: Main Loop - Monitor Button + Slow Blink LED
    // ========================================================================
    bool led_state = false;
    uint32_t last_blink_time = 0;
    
    while (1) {
        // Check button for mode switch
        monitor_button_for_mode_switch(BOOT_MODE_FREERTOS);
        
        // Poll WiFi/lwIP
        cyw43_arch_poll();
        
        // Slow blink LED to indicate connected state
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_blink_time >= LED_BLINK_SLOW_MS) {
            led_state = !led_state;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
            last_blink_time = now;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// --- RTOS Launcher ---
void launch_freertos_mode() {
    printf("Entering FreeRTOS mode (Core %d, WiFi Enabled).\n", get_core_num());

    // Create the task that initializes WiFi and runs the server
    xTaskCreateAffinitySet(
        wifi_management_task, 
        "WiFiMgrCore0", 
        configMINIMAL_STACK_SIZE + 4096, 
        NULL,
        1,
        CORE_0_AFFINITY_MASK,
        NULL
    );

    // Start the scheduler, execution stops here.
    vTaskStartScheduler(); 
    // The scheduler takes over permanently.
}

int main() {
    stdio_init_all();
    
    // --- Initialize GPIO 13 for input with pull-up resistor ---
    gpio_init(PIN_MODE_SW);
    gpio_set_dir(PIN_MODE_SW, GPIO_IN);
    gpio_pull_up(PIN_MODE_SW);
    // ----------------------------------------------------------

    sleep_ms(3000);

    // Check if we just rebooted from the watchdog and a flag is set
    if (watchdog_enable_caused_reboot()) {
        uint32_t boot_flag = *BOOT_FLAG_ADDR;
        printf("Watchdog reboot detected. Boot flag read: 0x%lX\n", boot_flag);

        // Clear the flag immediately after reading it
        *BOOT_FLAG_ADDR = 0;

        if (boot_flag == BOOT_MODE_FREERTOS) {
            launch_freertos_mode(); // Calls vTaskStartScheduler()
        } else {
            // Unknown flag, or BOOT_MODE_BARE_METAL, launch bare metal
            launch_bare_metal_mode(); // Enters while(1) loop
        }
    } else {
        // First power-on (e.g., USB connected, not watchdog reboot), default to a safe mode
        printf("Normal power-on detected. Launching bare metal.\n");
        launch_bare_metal_mode(); // Or your preferred default mode
    }

    // Never reached
    return 0;
}