#include <stdio.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <esp_vfs_fat.h>
#include <driver/spi_common.h>
#include <sdmmc_cmd.h>
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

spi_bus_config_t spi_bus_cfg;
spi_device_handle_t spi_device_handle;
spi_device_interface_config_t devcfg;

sdspi_device_config_t sdspi_device_config;
sdspi_dev_handle_t sdspi_dev_handle;

// esp_vfs_fat_sdmmc_mount_config_t sdmmc_mount_config;
// sdmmc_card_t *sdmmc_card;
// sdmmc_host_t sdmmc_host;

esp_timer_handle_t debounce_timer;

void loop();

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

    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << MOSI_BIT);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(ACT_BIT_n, 1);
    gpio_set_level(IRQ_BIT_n, 1);
    card_present_n = gpio_get_level(CP_BIT_n);

    // SPI
    spi_bus_cfg = {
        .mosi_io_num = MOSI_BIT,
        .miso_io_num = MISO_BIT,
        .sclk_io_num = SCK_BIT,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_DMA_LEN};

    // sdmmc_mount_config = {.format_if_mount_failed = false};
    // sdmmc_host = SDSPI_HOST_DEFAULT();

    sdspi_device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    sdspi_device_config.gpio_cs = SS_BIT_n;
    sdspi_device_config.host_id = HSPI_HOST;

    devcfg = {
        .mode = 0,                         // SPI mode 0: CPOL:-0 and CPHA:-0
        .clock_speed_hz = 500 * 1000,      // Clock out at 500 kHz
        .spics_io_num = SS_BIT_n,          // This field is used to specify the GPIO pin that is to be used as CS'
        .queue_size = 7,                   // We want to be able to queue 7 transactions at a time
    };

    ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &spi_bus_cfg, SDSPI_DEFAULT_DMA)); // Initialize the SPI bus
    ESP_ERROR_CHECK(spi_bus_add_device(HSPI_HOST, &devcfg, &spi_device_handle));
    // ESP_ERROR_CHECK(sdspi_host_init_device(&sdspi_device_config, &sdspi_dev_handle)); // Attach the SD card to the SPI bus

    if (!card_present_n)
    {
        // esp_vfs_fat_sdspi_mount(MOUNT_POINT, &sdmmc_host, &sdspi_device_config, &sdmmc_mount_config, &sdmmc_card);
        // sdmmc_card_print_info(stdout, sdmmc_card);
    }
    else
    {
        ESP_LOGI(INFO_TAG, "CARD NOT PRESENT!!!");
    }

    // Set up level 5 interrupt
    int INTR_NUM = 31; // extern level 5, see table in file soc.h for details
    ESP_INTR_DISABLE(INTR_NUM);
    intr_matrix_set(xPortGetCoreID(), ETS_GPIO_INTR_SOURCE, INTR_NUM);
    ESP_INTR_ENABLE(INTR_NUM);
    ESP_LOGI(INFO_TAG, "Level 5 Interrupt set on core %d", xPortGetCoreID());

    ESP_ERROR_CHECK(esp_timer_create(&debounce_timer_args, &debounce_timer));
}

uint8_t getCmdValFromReg()
{
    return (uint8_t)REG_READ(GPIO_IN1_REG);
}

uint8_t getDataFromReg()
{
    uint32_t in_reg = REG_READ(GPIO_IN_REG);
    return (bool)(in_reg & (1 << D7_BIT)) << 7 | (bool)(in_reg & (1 << D6_BIT)) << 6 | (bool)(in_reg & (1 << D5_BIT)) << 5 | (bool)(in_reg & (1 << D4_BIT)) << 4 |
           (bool)(in_reg & (1 << D3_BIT)) << 3 | (bool)(in_reg & (1 << D2_BIT)) << 2 | (bool)(in_reg & (1 << D1_BIT)) << 1 | (bool)(in_reg & (1 << D0_BIT));
}

void start_command()
{
    uint8_t data = getDataFromReg();
    uint8_t cmd_val = getCmdValFromReg();
    uint16_t byte_count;

    if (!(data & 0x80)) // READ1 or WRITE1
    {
        ets_printf("INSIDE READ1 or WRITE1\n");
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << 1)); // assert ACT_BIT_n
        byte_count = data & 0x3f;

        char buffer[33];
        itoa(byte_count, buffer, 2);
        printf("BYTE_COUNT: %s\n", buffer);

        if (data & 0x40)
            goto do_read;
        else
            goto do_write;
    }
    else if (!(data & 0x40)) // READ2 or WRITE2
    {
        printf("INSIDE READ2 or WRITE2\n");
    }
    else
    {
        uint8_t cmd = (data & 0x3e) >> 1;

        if (cmd == 0) // SPI_SELECT
        {
            REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << 1)); // assert ACT_BIT_n

            if ((data & 0x3f) & 1) // Select
            {
                REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << SS_BIT_n)); // assert SS_BIT_n
                REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << LED_BIT));  // LED on
            }
            else // Deselect
            {
                REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << SS_BIT_n)); // de-assert SS_BIT_n
                REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << LED_BIT));  // LED off
            }
        }
        else if (cmd == 1) // CARD_PRESENT
        {
            REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << IRQ_BIT_n)); // assert IRQ_BIT_n
            REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << 1));        // assert ACT_BIT_n

            if (cmd_val & (1 << 3)) // CLK_BIT is bit 3 in cmd_val
            {
                while (getCmdValFromReg() & (1 << 3))
                    ;
            }
            else
            {
                while (!(getCmdValFromReg() & (1 << 3)))
                    ;
            }

            set_data_direction_to_output();
            REG_WRITE(GPIO_OUT_W1TC_REG, 0xEEC0000ul); // clear D0-D7 (set them low)

            if (!(getCmdValFromReg() & (1 << 2)))              // CP_BIT_n is bit 2 in cmd_val
                REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << D0_BIT)); // Set D0 high

            REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << 1)); // de-assert ACT_BIT_n
        }
        else if (cmd == 2) // SPEED
        {
            if ((data & 0x3f) & 1)                       // Fast
                devcfg.clock_speed_hz = 8 * 1000 * 1000; // Clock out at 8 MHz
            else                                         // Slow
                devcfg.clock_speed_hz = 500 * 1000;      // Clock out at 500 kHz

            REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << 1)); // assert ACT_BIT_n
        }

        loop();
    }
do_read:
    ets_printf("DO_READ\n");

    while (true)
        ;

do_write:
    ets_printf("DO_WRITE\n");

    while (true)
        ;
}

void loop()
{
    while (true)
    {
        if (ISR0) // handler for REQ signal changes
        {
            if (REG_GET_BIT(GPIO_IN1_REG, (1 << 0))) // REQ_BIT_n
            {
                REG_WRITE(GPIO_OUT1_W1TS_REG, (1 << 1)); // de-assert ACT_BIT_n
                set_data_direction_to_input();
            }
            else
            {
                start_command();
            }

            ISR0 = 0;
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