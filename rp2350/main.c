#include "main.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

mutex_t spi_mutex;
volatile bool amiga_wrote_to_card = false;

extern void par_spi_main(void);
extern void ftp_server_main(void);

// --- Core 1 entry (FTP server) ---
void core1_entry() {
    ftp_server_main();
}

// --- Core 0 entry (Amiga SPI bridge) ---
void core0_entry() {
    par_spi_main();
}

int main() {
    stdio_init_all();
    sleep_ms(2000); // allow time for macOS to enumerate USB
    printf("Pico USB serial active\n");
	
    mutex_init(&spi_mutex);
    multicore_launch_core1(core1_entry);
    core0_entry();  // never returns
    return 0;
}
