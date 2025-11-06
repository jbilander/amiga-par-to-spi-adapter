#include "main.h"

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
    mutex_init(&spi_mutex);
    multicore_launch_core1(core1_entry);
    core0_entry();  // never returns
    return 0;
}
