/* ftp_server.c - FTP server with FatFS (runs on Core 1 in WiFi mode) */

#include "main.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "ff.h"
#include <string.h>
#include "util.h"

// ============================================================================
// FatFS Objects
// ============================================================================

static FATFS fs;       // Filesystem object
static FIL file;       // File object
static FRESULT fr;     // FatFs return code

// ============================================================================
// FatFS Timestamp Function (required by FatFs)
// ============================================================================

DWORD get_fattime(void) {
    // Return a dummy timestamp: 2025-01-01 00:00:00
    DWORD year = (2025 - 1980) << 25;
    DWORD month = 1 << 21;
    DWORD day = 1 << 16;
    return year | month | day;
}

// ============================================================================
// FTP Server Main (runs on Core 1 in WiFi mode)
// ============================================================================

void ftp_server_main(void) {
    printf("Core 1: Initializing FTP server...\n");
    
    // Note: stdio_init_all() already called in main.c
    // Note: PIN_LED (GPIO 28) already initialized in main.c
    
    // ========================================================================
    // Mount SD Card Filesystem
    // ========================================================================
    
    printf("Core 1: Mounting SD card filesystem...\n");
    fr = f_mount(&fs, "", 1);  // Mount with force option
    
    if (fr != FR_OK) {
        printf("Core 1: ERROR - Failed to mount SD card (FR=%d)\n", fr);
        printf("Core 1: FTP server cannot start without SD card\n");
        
        // Flash LED fast to indicate error
        while (current_mode == MODE_WIFI) {
            gpio_put(PIN_LED, 1);
            sleep_ms(100);
            gpio_put(PIN_LED, 0);
            sleep_ms(100);
        }
        
        // Cleanup and exit
        f_mount(NULL, "", 0);
        printf("Core 1: FTP server stopped (mount failed)\n");
        return;
    }
    
    printf("Core 1: SD card mounted successfully\n");
    
    // ========================================================================
    // FatFS Test: Create a file with UTF-8 content
    // ========================================================================
    
    printf("Core 1: Running FatFS test...\n");
    
    const char *utf8_filename = "Nordic_ÅÄÖ.txt";
    fr = f_open(&file, utf8_filename, FA_WRITE | FA_CREATE_ALWAYS);
    
    if (fr == FR_OK) {
        printf("Core 1: Created test file: %s\n", utf8_filename);
        
        // Write UTF-8 content converted to Latin1 for Amiga
        const char *utf8_text = "Testing Nordic characters: Ångström, Äpple, Örebro.\n"
                                "This will display correctly on the Amiga side.\n"
                                "FTP server is ready!\n";
        
        char latin1_text[256];
        utf8_to_latin1(utf8_text, latin1_text, sizeof(latin1_text));
        
        UINT bytes_written;
        f_write(&file, latin1_text, strlen(latin1_text), &bytes_written);
        printf("Core 1: Wrote %u bytes to test file\n", bytes_written);
        
        fr = f_close(&file);
        if (fr != FR_OK) {
            printf("Core 1: WARNING - f_close failed (FR=%d)\n", fr);
        }
    } else {
        printf("Core 1: ERROR - Could not create test file (FR=%d)\n", fr);
        
        // Flash LED at medium speed to indicate file error
        for (int i = 0; i < 10 && current_mode == MODE_WIFI; i++) {
            gpio_put(PIN_LED, 1);
            sleep_ms(250);
            gpio_put(PIN_LED, 0);
            sleep_ms(250);
        }
    }
    
    printf("Core 1: FatFS test complete\n");
    
    // ========================================================================
    // TODO: Initialize actual FTP server library here
    // ========================================================================
    
    // ftpd_init();
    
    printf("Core 1: FTP server ready (placeholder mode)\n");
    
    // Signal to Core 0 that we're ready
    core1_done = true;
    printf("Core 1: Signaled Core 0 that FTP server is ready\n");
    
    // ========================================================================
    // Main FTP Loop - runs until mode switches back to Amiga
    // ========================================================================
    
    while (current_mode == MODE_WIFI) {
        // TODO: Process FTP requests
        // ftpd_process();
        
        // Placeholder: Toggle LED slowly to show FTP mode active
        gpio_put(PIN_LED, 1);
        sleep_ms(1000);
        
        // Check mode in case it changed during sleep
        if (current_mode != MODE_WIFI) break;
        
        gpio_put(PIN_LED, 0);
        sleep_ms(1000);
        
        // Check mode again
        if (current_mode != MODE_WIFI) break;
    }
    
    // ========================================================================
    // Cleanup when exiting WiFi mode
    // ========================================================================
    
    printf("Core 1: Cleaning up FTP server...\n");
    
    // TODO: Cleanup FTP server library
    // ftpd_deinit();
    
    // Unmount filesystem
    printf("Core 1: Unmounting SD card...\n");
    f_mount(NULL, "", 0);
    
    // Turn off LED
    gpio_put(PIN_LED, 0);
    
    printf("Core 1: FTP cleanup complete\n");
}