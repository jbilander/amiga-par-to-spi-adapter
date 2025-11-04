#include "main.h"

extern void par_spi_main(void);
extern void ftp_server_main(void);

// --- Core 0 entry (FTP server) ---
void core1_entry() {
    ftp_server_main();
}

// --- Core 1 entry (Amiga SPI bridge) ---
void core0_entry() {
    par_spi_main();
}

int main() {
    multicore_launch_core1(core1_entry);
    core0_entry();  // never returns
    return 0;
}
