#include "main.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "ff.h"
#include <string.h>
#include "util.h"

static FATFS fs;       // Filesystem object
static FIL file;       // File object
static FRESULT fr;     // FatFs return code

DWORD get_fattime(void) {
    DWORD year = (2025 - 1980) << 25;
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
    }

    const char *utf8_filename = "Nordic_ÅÄÖ.txt";
    fr = f_open(&file, utf8_filename, FA_WRITE | FA_CREATE_ALWAYS);

    if (fr == FR_OK) {

        const char *utf8_text = "Testing Nordic characters: Ångström, Äpple, Örebro. This will display correctly on the Amiga side.\n";
        char latin1_text[256];
        utf8_to_latin1(utf8_text, latin1_text, sizeof(latin1_text));

        UINT bw;
        f_write(&file, latin1_text, strlen(latin1_text), &bw);

        //fr = f_sync(&file);
        //printf("Debug: f_sync result=%d\n", fr);

        fr = f_close(&file);
        printf("Debug: f_close result=%d\n", fr);

    } else {
        // Could not create file, blink LED
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
        // For now, toggle LED every 2s
        gpio_put(PIN_LED, 1);
        sleep_ms(2000);
        gpio_put(PIN_LED, 0);
        sleep_ms(2000);
    }
}

