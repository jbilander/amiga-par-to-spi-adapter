/* main.h - Shared definitions with dynamic mode switching */

#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdbool.h>
#include "pico/mutex.h"

//      Pin name    GPIO    Direction   Comment     Description
#define PIN_D(x)    (0+x)   // In/out
#define PIN_IRQ     8       // Output   Active low
#define PIN_ACT     9       // Output   Active low
#define PIN_CLK     10      // Input
#define PIN_REQ     11      // Input    Active low
#define PIN_MODE_SW 13      // Input    Pull-up     Mode switch button
#define PIN_MISO    16      // Input    Pull-up
#define PIN_SS      17      // Output   Active low
#define PIN_SCK     18      // Output
#define PIN_MOSI    19      // Output
#define PIN_CDET    20      // Input    Pull-up     Card Detect
#define PIN_LED     28      // Output   SPI activity indicator

#define SPI_SLOW_FREQUENCY (400*1000)
#define SPI_FAST_FREQUENCY (16*1000*1000)

// Button debouncing (50ms typical for mechanical switches)
#define BUTTON_DEBOUNCE_MS 50

// ============================================================================
// WiFi Configuration
// ============================================================================
// WiFi credentials are loaded from wifi_credentials.cmake during build:
//   WIFI_SSID and WIFI_PASSWORD are defined as compile-time defines
// 
// Create wifi_credentials.cmake in your project root:
//   set(WIFI_SSID "YourNetworkName")
//   set(WIFI_PASSWORD "YourPassword")

// ============================================================================
// System Modes (Dynamic Mode Switching)
// ============================================================================

typedef enum {
    MODE_WIFI,       // WiFi active on Core 0, FTP on Core 1
    MODE_AMIGA,      // Amiga active on Core 1, Core 0 monitoring (WiFi off)
    MODE_SWITCHING   // Transitioning between modes
} system_mode_t;

// ============================================================================
// LED Patterns (Using CYW43_WL_GPIO_LED_PIN - built-in WiFi LED)
// ============================================================================

typedef enum {
    LED_STARTUP,           // Solid ON - System initializing
    LED_WIFI_CONNECTING,   // Fast blink - Connecting to WiFi
    LED_WIFI_CONNECTED,    // Slow blink - WiFi connected, FTP ready
    LED_WIFI_FAILED,       // Very fast blink - Connection failed
    LED_AMIGA_MODE,        // OFF - Amiga mode active
    LED_MODE_SWITCHING     // Rapid flashes - Transitioning
} led_pattern_t;

// LED timing (milliseconds)
#define LED_WIFI_CONNECTING_MS   200    // Fast blink (5Hz)
#define LED_WIFI_CONNECTED_MS    1000   // Slow blink (1Hz)
#define LED_WIFI_FAILED_MS       100    // Very fast blink (10Hz)
#define LED_MODE_SWITCH_FLASH_MS 80     // Quick flashes
#define LED_MODE_SWITCH_COUNT    6      // Number of flashes

// ============================================================================
// Shared State Variables (volatile for multicore safety)
// ============================================================================

extern volatile system_mode_t current_mode;
extern volatile led_pattern_t current_led_pattern;
extern volatile bool mode_button_pressed;
extern volatile bool card_detect_override;  // Override card detect to report "not present"

// ============================================================================
// Synchronization
// ============================================================================

// Note: spi_mutex is only used by FatFS library (diskio.c) for SD card access
// Since only one mode runs at a time after watchdog reset, no actual locking needed in main code
extern mutex_t spi_mutex;

// ============================================================================
// Function Declarations
// ============================================================================

// Core entry points
void core0_entry(void);  // Mode management (WiFi control, button, LED)
void core1_entry(void);  // Work loop (Amiga OR FTP)

// Core 1 work functions
void par_spi_main(void);      // Amiga bridge (runs on Core 1 in Amiga mode)
void ftp_server_main(void);   // FTP server (runs on Core 1 in WiFi mode)

// Mode switching
void switch_to_amiga_mode(void);
void switch_to_wifi_mode(void);

// LED control
void update_led_pattern(void);
void indicate_mode_switch(void);

// Card detection
void signal_card_change(void);

#endif // MAIN_H