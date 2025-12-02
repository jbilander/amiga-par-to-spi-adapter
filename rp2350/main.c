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

// Signal interrupt to Amiga before mode switch
// Sends IRQ pulses to notify Amiga that card state will change
void signal_interrupt_to_amiga(void) {
    printf("Signaling interrupt to Amiga...\n");
    
    // Initialize IRQ pin if not already done
    gpio_init(PIN_IRQ);
    gpio_set_dir(PIN_IRQ, GPIO_OUT);
    gpio_put(PIN_IRQ, 1);  // Start high (inactive)
    
    // Send IRQ pulses to notify Amiga
    for (int i = 0; i < 5; i++) {
        gpio_put(PIN_IRQ, 0);   // IRQ low (active)
        sleep_ms(100);
        gpio_put(PIN_IRQ, 1);   // IRQ high (inactive)
        sleep_ms(100);
    }
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

        if (held_duration_ms >= 3000 && !reboot_triggered) {  // NEW: Check flag
            // Button held for 3 seconds - trigger ONCE
            reboot_triggered = true;  // NEW: Set flag to prevent re-trigger
            
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

void ftp_server_application_task(void *pvParameters) {
    printf("Starting RAW FTP Server on Core: %d\n", get_core_num());
    
    while (1) {
        // Your RAW LWIP FTP Server implementation starts here.
        // Remember to use cyw43_arch_lwip_begin()/end() around network calls.

        monitor_button_for_mode_switch(BOOT_MODE_FREERTOS); // Monitor button in this task too

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// --- FTP Server/WiFi Management Task ---
void wifi_management_task(void *pvParameters) {
    // This runs within the FreeRTOS context (likely core 0)

    // *** Keep the short delay for stability with sys_freertos/threadsafe modes ***
    vTaskDelay(pdMS_TO_TICKS(10)); 

    // 1. Initialize Wi-Fi
    if (cyw43_arch_init()) {
        printf("Failed to init CYW43 on Core %d\n", get_core_num());
        // In a real app, handle this gracefully
        while(1) { vTaskDelay(1000); }
    }
    printf("CYW43 initialized on Core: %d.\n", get_core_num());

    // 2. Your RAW LWIP FTP Server task starts here (likely on core 1)
    xTaskCreateAffinitySet(
        ftp_server_application_task, 
        "FTPTaskCore1", 
        configMINIMAL_STACK_SIZE + 4096, 
        NULL,
        2,
        CORE_1_AFFINITY_MASK,
        NULL
    );

    while (1) {
        monitor_button_for_mode_switch(BOOT_MODE_FREERTOS);
        cyw43_arch_poll();

        vTaskDelay(pdMS_TO_TICKS(50)); // Yield slightly
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
        printf("Watchdog reboot detected. Boot flag read: 0x%lX\n", boot_flag); // Print the flag value

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
