#include "main.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

//FATFS fs;
mutex_t spi_mutex;
volatile bool amiga_wrote_to_card = false;

void par_spi_main(void);
void ftp_server_main(void);

// --- Core 0 entry (Amiga SPI bridge) ---
void core1_entry() {
    printf("Start Amiga SPI bridge\n");
    par_spi_main();
}

// --- Core 1 entry (FTP server) ---
void core0_entry() {
    printf("Start FTP Server\n");
    ftp_server_main();
}

int main() {
    stdio_init_all();
    sleep_ms(2000); // allow time for macOS to enumerate USB
    printf("Pico USB serial active\n");
	
    mutex_init(&spi_mutex);

    // --- Wi-Fi Initialization ---
    printf("Pico Wi-Fi test (threadsafe background)\n");

    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed!\n");
        return -1;
    }

    cyw43_arch_enable_sta_mode();

    printf("Connecting to %s...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Connection failed.\n");
        cyw43_arch_deinit();
        return -1;
    }
    // --- End Wi-Fi Initialization ---
    printf("Connected!\n");

    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    printf("IP address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));

    multicore_launch_core1(core1_entry);
    core0_entry();  // never returns
    return 0;
}

