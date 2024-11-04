#include <stdio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <rom/ets_sys.h>
#include <esp_mac.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>
#include "Main.h"

static const char *INFO_TAG = "LOG_INFO";
volatile int ISR0;

void setup(void)
{
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << ACK_BIT_n);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << BUSY_BIT);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << STROBE_BIT_n);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    int INTR_NUM = 31; // extern level 5, see table in file soc.h for details
    ESP_INTR_DISABLE(INTR_NUM);
    intr_matrix_set(xPortGetCoreID(), ETS_GPIO_INTR_SOURCE, INTR_NUM);
    ESP_INTR_ENABLE(INTR_NUM);

    ESP_LOGI(INFO_TAG, "Level 5 Interrupt set on core %u", xPortGetCoreID());

    gpio_set_level(ACK_BIT_n, 1);
}

extern "C" void app_main()
{
    setup();

    while (true)
    {
        if (ISR0)
        {
            REG_WRITE(GPIO_OUT_W1TC_REG, BIT(ACK_BIT_n));
            REG_WRITE(GPIO_OUT_W1TS_REG, BIT(ACK_BIT_n));
            // REG_WRITE(GPIO_OUT_W1TS_REG, BIT(LED_BIT));
            ISR0 = 0;
        }
    }
}