#include "main.h"
#include "pico/stdlib.h"
//#include "pico/util/datetime.h"
//#include "hardware/rtc.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "ff.h"        // FatFs main header
#include <string.h>

// FatFs objects (global or static)
static FATFS fs;       // Filesystem object
static FIL file;       // File object
static FRESULT fr;     // FatFs return code

DWORD get_fattime(void) {
    DWORD year = (2024 - 1980) << 25;
    DWORD month = 1 << 21;
    DWORD day = 1 << 16;
    return year | month | day;
}

void ftp_server_main(void) {

    stdio_init_all();
    printf("Debug: FatFs test started\n");

    // --- Initialize GPIO LED (status indicator) ---
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

    // --- Mount SD card filesystem ---
    fr = f_mount(&fs, "", 1);

    if (fr != FR_OK) {
        // Mount failed, blink LED fast
        while (1) {
            gpio_put(PIN_LED, 1);
            sleep_ms(100);
            gpio_put(PIN_LED, 0);
            sleep_ms(100);
        }
    } else {
        fr = f_setcp(850); // Set the OEM code page to Windows-1252 (ANSI)
    }

    // --- Example: Create a new file with long Latin-1 filename ---
    const TCHAR *filename = "Testfil_åäö_longname.txt";  // UTF-8 works; Latin-1 chars accepted
    fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK) {

        UINT bytes_written;
        fr = f_write(&file, "Hello from FatFs!\r\n", 19, &bytes_written);
        printf("Debug: f_write result=%d, bytes_written=%u\n", fr, bytes_written);

        fr = f_sync(&file);
        printf("Debug: f_sync result=%d\n", fr);

        fr = f_close(&file);
        printf("Debug: f_close result=%d\n", fr);

        /*
        const char *text = "Hej världen!\r\nThis is a FatFs test file with long filename.\r\n";
        UINT bw;
        f_write(&file, text, strlen(text), &bw);
        f_close(&file);

        // Optional: verify success
        if (bw == strlen(text)) {
            gpio_put(PIN_LED, 1); // success indicator
        }
        */
    } else {
        // Could not create file, blink LED slowly
        while (1) {
            gpio_put(PIN_LED, 1);
            sleep_ms(500);
            gpio_put(PIN_LED, 0);
            sleep_ms(500);
        }
    }

    // --- Unmount filesystem ---
    f_mount(NULL, "", 1);

    // --- Main FTP loop placeholder ---
    while (1) {
        // Here you will later handle FTP commands like STOR, RETR, LIST, etc.
        // For now, toggle LED every 2s
        gpio_put(PIN_LED, 1);
        sleep_ms(2000);
        gpio_put(PIN_LED, 0);
        sleep_ms(2000);
    }
}

