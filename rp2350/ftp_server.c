#include "main.h"

// --- FTP server main loop (runs on Core 1) ---
void ftp_server_main(void) {

    /*
    stdio_init_all();
    printf("Core0: FTP server main started.\n");

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, true);
    */

    while (true) {
        // Simulate FTP activity for now

        /*
        gpio_put(PIN_LED, 1);
        sleep_ms(1000);
        gpio_put(PIN_LED, 0);
        sleep_ms(1000);
        */

        // Later this will be your FTP command handler:
        // ftp_handle_client();
    }
}
