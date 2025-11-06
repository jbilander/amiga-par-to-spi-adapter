#include "main.h"

// --- FTP server main loop (runs on Core 1) ---
void ftp_server_main(void) {

    while (true) {
        // Simulate FTP activity for now

        //sleep_ms(10000);
        //gpio_set_dir(PIN_IRQ, true);
        //gpio_put(PIN_IRQ, 0);
        //sleep_ms(120);
        //gpio_set_dir(PIN_IRQ, false);

        // Later this will be your FTP command handler:
        // ftp_handle_client();
    }
}
