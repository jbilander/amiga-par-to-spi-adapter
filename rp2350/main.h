/* main.h - Shared definitions for watchdog-based dual-boot system */

#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdbool.h>
#include "pico/mutex.h"
#include "hardware/watchdog.h"  // For BOOT_FLAG_ADDR (watchdog_hw->scratch)

//      Pin name    GPIO    Direction   Comment     Description
#define PIN_D(x)    (0+x)   // In/out
#define PIN_IRQ     8       // Output   Active low
#define PIN_ACT     9       // Output   Active low
#define PIN_CLK     10      // Input
#define PIN_REQ     11      // Input    Active low
#define PIN_MODE_SW 13      // Input    Pull-up     Mode switch button (3-second hold)
#define PIN_MISO    16      // Input    Pull-up
#define PIN_SS      17      // Output   Active low
#define PIN_SCK     18      // Output
#define PIN_MOSI    19      // Output
#define PIN_CDET    20      // Input    Pull-up     Card Detect
#define PIN_LED     28      // Output   SPI activity indicator

#define SPI_SLOW_FREQUENCY (400*1000)
#define SPI_FAST_FREQUENCY (16*1000*1000)

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
// Boot Mode Magic Values (stored in watchdog scratch register)
// ============================================================================

#define BOOT_MODE_BARE_METAL 0xBEEF0001  // Amiga SPI Bridge mode
#define BOOT_MODE_FREERTOS   0xBEEF0002  // WiFi/FTP Server mode
#define BOOT_FLAG_ADDR       (watchdog_hw->scratch + 6)  // Use scratch register 6

// ============================================================================
// FreeRTOS Core Affinity Masks (for SMP mode)
// ============================================================================

// CORE_0_AFFINITY_MASK (1U << 0U) or just tskNO_AFFINITY if that task is the only one intended for core 0
#define CORE_0_AFFINITY_MASK (1U << 0U)
#define CORE_1_AFFINITY_MASK (1U << 1U)

// ============================================================================
// Function Declarations
// ============================================================================

// Main entry points (defined in main.c)
void launch_freertos_mode(void);
void launch_bare_metal_mode(void);
void trigger_reboot_to_mode(uint32_t mode_flag);
void monitor_button_for_mode_switch(uint32_t current_mode);

// ============================================================================
// Synchronization
// ============================================================================

// Note: spi_mutex is only used by FatFS library (diskio.c) for SD card access
extern mutex_t spi_mutex;

// ============================================================================
// Work Functions
// ============================================================================

// Amiga SPI Bridge (defined in par_spi.c)
// Runs in BOOT_MODE_BARE_METAL
void par_spi_main(void);

// FTP Server (defined in ftp_server.c)
// Runs in BOOT_MODE_FREERTOS
void ftp_server_main(void);

#endif // MAIN_H