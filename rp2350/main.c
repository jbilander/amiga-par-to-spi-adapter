#include "main.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/pio.h"
#include "par_amiga_rx.pio.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"


mutex_t spi_mutex;
volatile bool amiga_wrote_to_card = false;

void par_spi_main(void);
//void par_spi_main_test(void);
void ftp_server_main(void);

// --- LED blink timer callback ---
/*
static bool wifi_led_timer_cb(repeating_timer_t *rt) {
    static bool state = false;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, state);
    state = !state;
    return true; // keep repeating
}
*/

// --- Core 1 entry (FTP server) ---
void core1_entry() {
    //printf("Start FTP Server\n");
    //ftp_server_main();
}

// --- Core 0 entry (Amiga SPI bridge) ---
void core0_entry() {
    par_spi_main();
    //printf("Start Amiga SPI bridge\n");
    //par_spi_main_test();
}

int main() {
    stdio_init_all();

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);

    sleep_ms(3000); // allow time for macOS to enumerate USB
    printf("Pico USB serial active\n");

    /*
    // --- Wi-Fi Initialization ---
    printf("Pico Wi-Fi test (threadsafe background)\n");

    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed!\n");
        while (true) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            sleep_ms(100);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            sleep_ms(100);
        }
    }

    cyw43_arch_enable_sta_mode();

    printf("Connecting to %s...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Connection failed.\n");
        while (true) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            sleep_ms(100);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            sleep_ms(100);
        }
    }

    // Start slow blink timer (connected indicator)
    static repeating_timer_t wifi_led_timer;
    add_repeating_timer_ms(1000, wifi_led_timer_cb, NULL, &wifi_led_timer);

    // --- End Wi-Fi Initialization ---
    printf("Connected!\n");

    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    printf("IP address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));
    */

    mutex_init(&spi_mutex);

    multicore_launch_core1(core1_entry);
    core0_entry();  // never returns
    return 0;
}

