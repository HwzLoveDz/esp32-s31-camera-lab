/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated peripheral configuration file
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_board_periph.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "hal/gpio_types.h"
#include "periph_adc.h"
#include "periph_gpio.h"
#include "periph_i2s.h"
#include "periph_ledc.h"

// Peripheral configuration structures
const static i2c_master_bus_config_t esp_bmgr_i2c_master_cfg = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = 0,
    .scl_io_num = 1,
    .glitch_ignore_cnt = 7,
    .intr_priority = 1,
    .trans_queue_depth = 0,
    .flags = {
        .enable_internal_pullup = true,
    },
    .clk_source = I2C_CLK_SRC_DEFAULT,
};

const static periph_gpio_config_t esp_bmgr_gpio_boot_button_cfg = {
    .gpio_config = {
        .pin_bit_mask = BIT64(61),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    },
    .default_level = 0,
};

const static periph_i2s_config_t esp_bmgr_i2s_audio_out_cfg = {
    .port = I2S_NUM_0,
    .role = I2S_ROLE_MASTER,
    .mode = I2S_COMM_MODE_TDM,
    .direction = I2S_DIR_TX,
    .i2s_cfg = {
        .tdm = {
            .clk_cfg = {
                .sample_rate_hz = 48000,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 8,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1,
                .ws_width = 16,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = false,
                .big_endian = false,
                .bit_order_lsb = false,
                .skip_mask = false,
                .total_slot = 2,
            },
            .gpio_cfg = {
                .mclk = -1,
                .bclk = 3,
                .ws = 4,
                .dout = 5,
                .din = 6,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        },
    },
};

const static periph_i2s_config_t esp_bmgr_i2s_audio_in_cfg = {
    .port = I2S_NUM_0,
    .role = I2S_ROLE_MASTER,
    .mode = I2S_COMM_MODE_TDM,
    .direction = I2S_DIR_RX,
    .i2s_cfg = {
        .tdm = {
            .clk_cfg = {
                .sample_rate_hz = 48000,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 8,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2,
                .ws_width = 16,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = false,
                .big_endian = false,
                .bit_order_lsb = false,
                .skip_mask = false,
                .total_slot = 4,
            },
            .gpio_cfg = {
                .mclk = -1,
                .bclk = 3,
                .ws = 4,
                .dout = 5,
                .din = 6,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        },
    },
};

const static periph_gpio_config_t esp_bmgr_gpio_pa_control_cfg = {
    .gpio_config = {
        .pin_bit_mask = BIT64(7),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    },
    .default_level = 0,
};

const static periph_gpio_config_t esp_bmgr_gpio_sd_power_cfg = {
    .gpio_config = {
        .pin_bit_mask = BIT64(39),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    },
    .default_level = 1,
};

const static periph_ledc_config_t esp_bmgr_ledc_camera_xclk_cfg = {
    .handle = {
        .channel = LEDC_CHANNEL_0,
        .speed_mode = LEDC_LOW_SPEED_MODE,
    },
    .gpio_num = 55,
    .duty = 0,
    .freq_hz = 20000000,
    .duty_resolution = LEDC_TIMER_1_BIT,
    .timer_sel = LEDC_TIMER_1,
    .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    .output_invert = false,
};

const static periph_adc_config_t esp_bmgr_adc_oneshot_cfg = {
    .role = ESP_BOARD_PERIPH_ROLE_ONESHOT,
    .cfg = {
        .oneshot = {
            .channel_id = 0,
            .unit_cfg = {
                .unit_id = ADC_UNIT_1,
                .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            },
            .chan_cfg = {
                .atten = ADC_ATTEN_DB_0,
                .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
            },
        },
    },
};

// Peripheral descriptor array
const esp_board_periph_desc_t g_esp_board_peripherals[] = {
    {
        .next = &g_esp_board_peripherals[1],
        .name = "i2c_master",
        .type = "i2c",
        .format = NULL,
        .role = ESP_BOARD_PERIPH_ROLE_MASTER,
        .cfg = &esp_bmgr_i2c_master_cfg,
        .cfg_size = sizeof(esp_bmgr_i2c_master_cfg),
        .id = 0,
    },
    {
        .next = &g_esp_board_peripherals[2],
        .name = "gpio_boot_button",
        .type = "gpio",
        .format = NULL,
        .role = ESP_BOARD_PERIPH_ROLE_IO,
        .cfg = &esp_bmgr_gpio_boot_button_cfg,
        .cfg_size = sizeof(esp_bmgr_gpio_boot_button_cfg),
        .id = 0,
    },
    {
        .next = &g_esp_board_peripherals[3],
        .name = "i2s_audio_out",
        .type = "i2s",
        .format = "tdm-out",
        .role = ESP_BOARD_PERIPH_ROLE_MASTER,
        .cfg = &esp_bmgr_i2s_audio_out_cfg,
        .cfg_size = sizeof(esp_bmgr_i2s_audio_out_cfg),
        .id = 0,
    },
    {
        .next = &g_esp_board_peripherals[4],
        .name = "i2s_audio_in",
        .type = "i2s",
        .format = "tdm-in",
        .role = ESP_BOARD_PERIPH_ROLE_MASTER,
        .cfg = &esp_bmgr_i2s_audio_in_cfg,
        .cfg_size = sizeof(esp_bmgr_i2s_audio_in_cfg),
        .id = 0,
    },
    {
        .next = &g_esp_board_peripherals[5],
        .name = "gpio_pa_control",
        .type = "gpio",
        .format = NULL,
        .role = ESP_BOARD_PERIPH_ROLE_IO,
        .cfg = &esp_bmgr_gpio_pa_control_cfg,
        .cfg_size = sizeof(esp_bmgr_gpio_pa_control_cfg),
        .id = 0,
    },
    {
        .next = &g_esp_board_peripherals[6],
        .name = "gpio_sd_power",
        .type = "gpio",
        .format = NULL,
        .role = ESP_BOARD_PERIPH_ROLE_IO,
        .cfg = &esp_bmgr_gpio_sd_power_cfg,
        .cfg_size = sizeof(esp_bmgr_gpio_sd_power_cfg),
        .id = 0,
    },
    {
        .next = &g_esp_board_peripherals[7],
        .name = "ledc_camera_xclk",
        .type = "ledc",
        .format = NULL,
        .role = ESP_BOARD_PERIPH_ROLE_NONE,
        .cfg = &esp_bmgr_ledc_camera_xclk_cfg,
        .cfg_size = sizeof(esp_bmgr_ledc_camera_xclk_cfg),
        .id = 0,
    },
    {
        .next = NULL,
        .name = "adc_oneshot",
        .type = "adc",
        .format = NULL,
        .role = ESP_BOARD_PERIPH_ROLE_ONESHOT,
        .cfg = &esp_bmgr_adc_oneshot_cfg,
        .cfg_size = sizeof(esp_bmgr_adc_oneshot_cfg),
        .id = 0,
    },
};
