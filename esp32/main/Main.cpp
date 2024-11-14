#include <stdio.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <rom/ets_sys.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_sig_map.h>
#include "Main.h"

static const char *INFO_TAG = "LOG_INFO";
volatile int ISR0;
volatile int ISR1;
int card_present_n; // holds cp state, 0 = card present, 1 = card ejected

esp_timer_handle_t debounce_timer;

static void debounce_timer_callback(void *arg)
{
    if (card_present_n != gpio_get_level(CP_BIT_n))
    {
        card_present_n = gpio_get_level(CP_BIT_n);
        gpio_set_level(IRQ_BIT_n, 0);
    }
}

const esp_timer_create_args_t debounce_timer_args = {
    .callback = &debounce_timer_callback};

void set_data_direction_to_output()
{
    PIN_INPUT_DISABLE(IO_MUX_GPIO27_REG);
    PIN_INPUT_DISABLE(IO_MUX_GPIO26_REG);
    PIN_INPUT_DISABLE(IO_MUX_GPIO25_REG);
    PIN_INPUT_DISABLE(IO_MUX_GPIO23_REG);
    PIN_INPUT_DISABLE(IO_MUX_GPIO22_REG);
    PIN_INPUT_DISABLE(IO_MUX_GPIO21_REG);
    PIN_INPUT_DISABLE(IO_MUX_GPIO19_REG);
    PIN_INPUT_DISABLE(IO_MUX_GPIO18_REG);

    REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(D0_BIT) | BIT(D1_BIT) | BIT(D2_BIT) | BIT(D3_BIT) |
                                    BIT(D4_BIT) | BIT(D5_BIT) | BIT(D6_BIT) | BIT(D7_BIT));

    REG_WRITE(GPIO_FUNC27_OUT_SEL_CFG_REG, 0x100);
    REG_WRITE(GPIO_FUNC26_OUT_SEL_CFG_REG, 0x100);
    REG_WRITE(GPIO_FUNC25_OUT_SEL_CFG_REG, 0x100);
    REG_WRITE(GPIO_FUNC23_OUT_SEL_CFG_REG, 0x100);
    REG_WRITE(GPIO_FUNC22_OUT_SEL_CFG_REG, 0x100);
    REG_WRITE(GPIO_FUNC21_OUT_SEL_CFG_REG, 0x100);
    REG_WRITE(GPIO_FUNC19_OUT_SEL_CFG_REG, 0x100);
    REG_WRITE(GPIO_FUNC18_OUT_SEL_CFG_REG, 0x100);
}

void set_data_direction_to_input()
{
    REG_WRITE(GPIO_ENABLE_W1TC_REG, BIT(D0_BIT) | BIT(D1_BIT) | BIT(D2_BIT) | BIT(D3_BIT) |
                                    BIT(D4_BIT) | BIT(D5_BIT) | BIT(D6_BIT) | BIT(D7_BIT));

    PIN_INPUT_ENABLE(IO_MUX_GPIO27_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO26_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO25_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO23_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO22_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO21_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO19_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO18_REG);
}

void setup()
{
    gpio_config_t io_conf;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << LED_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << ACT_BIT_n);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << REQ_BIT_n);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << CP_BIT_n);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << CLK_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << IRQ_BIT_n);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D0_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D1_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D2_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D3_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D4_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D5_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D6_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << D7_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(ACT_BIT_n, 1);
    gpio_set_level(IRQ_BIT_n, 1);
    card_present_n = gpio_get_level(CP_BIT_n);

    int INTR_NUM = 31; // extern level 5, see table in file soc.h for details
    ESP_INTR_DISABLE(INTR_NUM);
    intr_matrix_set(xPortGetCoreID(), ETS_GPIO_INTR_SOURCE, INTR_NUM);
    ESP_INTR_ENABLE(INTR_NUM);
    ESP_LOGI(INFO_TAG, "Level 5 Interrupt set on core %d", xPortGetCoreID());

    ESP_ERROR_CHECK(esp_timer_create(&debounce_timer_args, &debounce_timer));
}

void start_command()
{
}

void loop()
{
    while (true)
    {
        if (ISR0) // handler for REQ signal changes
        {
            ISR0 = 0;

            if (REG_GET_BIT(GPIO_IN1_REG, (1 << 0))) // REQ_BIT_n
            {
                gpio_set_level(ACT_BIT_n, 1);
                set_data_direction_to_input();
            }
            else
            {
                start_command();
            }
        }
        if (ISR1) // handler for CP signal changes
        {
            if (!esp_timer_is_active(debounce_timer))
                ESP_ERROR_CHECK(esp_timer_start_once(debounce_timer, DEBOUNCE_TIME));
            ISR1 = 0;
        }
    }
}

extern "C" void app_main()
{
    setup();
    loop();
}