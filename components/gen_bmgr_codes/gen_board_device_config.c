/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated device configuration file
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_board_device.h"
#include "dev_audio_codec.h"
#include "dev_button.h"
#include "dev_camera.h"
#include "dev_custom.h"
#include "dev_display_lcd.h"
#include "dev_fs_fat.h"
#include "dev_lcd_touch.h"
#include "dev_led_strip.h"
#include "dev_ledc_ctrl.h"
#include "dev_power_ctrl.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "gen_board_device_custom.h"

// Device configuration structures
const static dev_power_ctrl_config_t esp_bmgr_sdcard_power_ctrl_cfg = {
    .name = "sdcard_power_ctrl",
    .sub_type = "gpio",
    .sub_cfg = {
        .gpio = {
            .gpio_name = "gpio_sd_power",
            .active_level = 0,
        },
    },
};

const static dev_button_config_t esp_bmgr_boot_button_cfg = {
    .sub_type = "gpio",
    .button_timing_cfg = {
        .long_press_time = 2000,
        .short_press_time = 100,
    },
    .events_cfg = {
        .enabled_events = {
            .press_down = 1,
            .press_up = 1,
            .single_click = 1,
            .double_click = 1,
            .multi_click = 0,
            .long_press_start = 1,
            .long_press_hold = 0,
            .long_press_up = 1,
            .press_repeat = 0,
            .press_repeat_done = 0,
            .press_end = 0,
        },
    },
    .sub_cfg = {
        .gpio = {
            .gpio_name = "gpio_boot_button",
            .active_level = 0,
            .enable_power_save = false,
            .disable_pull = false,
        },
    },
    .name = "boot_button",
};

const static dev_led_strip_config_t esp_bmgr_led_strip_cfg = {
    .name = "led_strip",
    .chip = "ws2812",
    .sub_type = "rmt",
    .strip_config = {
        .strip_gpio_num = 37,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    },
    .sub_cfg = {
        .rmt = {
            .rmt_config = {
                .clk_src = RMT_CLK_SRC_DEFAULT,
                .resolution_hz = 10000000,
                .mem_block_symbols = 0,
                .flags = {
                    .with_dma = false,
                },
            },
        },
    },
};

const static dev_button_config_t esp_bmgr_adc_button_group_cfg = {
    .sub_type = "adc_multi",
    .button_timing_cfg = {
        .long_press_time = 2000,
        .short_press_time = 100,
    },
    .events_cfg = {
        .enabled_events = {
            .press_down = 1,
            .press_up = 1,
            .single_click = 1,
            .double_click = 1,
            .multi_click = 0,
            .long_press_start = 1,
            .long_press_hold = 0,
            .long_press_up = 1,
            .press_repeat = 0,
            .press_repeat_done = 0,
            .press_end = 0,
        },
    },
    .sub_cfg = {
        .adc = {
            .adc_name = "adc_oneshot",
            .multi = {
                .button_num = 4,
                .voltage_range = {380, 820, 1340, 1870},
                .button_labels = {"SET", "MODE", "VOLUME_DOWN", "VOLUME_UP"},
                .max_voltage = 2000,
            },
        },
    },
    .name = "adc_button_group",
};

const static dev_audio_codec_config_t esp_bmgr_audio_dac_cfg = {
    .name = "audio_dac",
    .chip = "es8389",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = false,
    .dac_enabled = true,
    .pa_peripheral = {
        .name = "gpio_pa_control",
        .port = 7,
    },
    .reset_peripheral = {
        .name = NULL,
        .port = -1,
    },
    .i2c_cfg = {
        .name = "i2c_master",
        .port = 0,
        .address = 32,
        .frequency = 400000,
    },
    .i2s_cfg = {
        .name = "i2s_audio_out",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_data_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .codec_sys_cfg = {
        .is_master = false,
        .no_mclk = true,
    },
    .codec_adc_cfg = {
        .digital_mic = false,
        .label = "",
    },
    .codec_dac_cfg = {
        .ref_enable = true,
        .ref_dac_ch = 0,
        .real_adc_data_ch = 0,
    },
    .codec_pa_cfg = {
        .pa_pin = 7,
        .pa_active_low = false,
        .hw_gain = {
            .pa_gain = 6.0,
        },
    },
    .codec_reset_cfg = {
        .reset_pin = -1,
        .reset_active_low = true,
    },
    .metadata = NULL,
    .metadata_size = 0,
};

const static dev_audio_codec_config_t esp_bmgr_audio_adc_cfg = {
    .name = "audio_adc",
    .chip = "es8389",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = true,
    .dac_enabled = false,
    .pa_peripheral = {
        .name = NULL,
        .port = -1,
    },
    .reset_peripheral = {
        .name = NULL,
        .port = -1,
    },
    .i2c_cfg = {
        .name = "i2c_master",
        .port = 0,
        .address = 32,
        .frequency = 400000,
    },
    .i2s_cfg = {
        .name = "i2s_audio_in",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_data_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .codec_sys_cfg = {
        .is_master = false,
        .no_mclk = true,
    },
    .codec_adc_cfg = {
        .digital_mic = false,
        .label = "FL,FR,RE,NA",
    },
    .codec_dac_cfg = {
        .ref_enable = true,
        .ref_dac_ch = 0,
        .real_adc_data_ch = 0,
    },
    .codec_pa_cfg = {
        .pa_pin = -1,
        .pa_active_low = true,
        .hw_gain = {
            .pa_gain = 0.0,
        },
    },
    .codec_reset_cfg = {
        .reset_pin = -1,
        .reset_active_low = true,
    },
    .metadata = NULL,
    .metadata_size = 0,
};

const static dev_fs_fat_config_t esp_bmgr_fs_sdcard_cfg = {
    .name = "fs_sdcard",
    .mount_point = "/sdcard",
    .frequency = SDMMC_FREQ_HIGHSPEED,
    .vfs_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16384,
    },
    .sub_type = "sdmmc",
    .sub_cfg = {
        .sdmmc = {
            .slot = SDMMC_HOST_SLOT_0,
            .bus_width = 4,
            .slot_flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
            .pins = {
                .clk = 24,
                .cmd = 25,
                .d0 = 20,
                .d1 = 21,
                .d2 = 22,
                .d3 = 23,
                .d4 = -1,
                .d5 = -1,
                .d6 = -1,
                .d7 = -1,
                .cd = -1,
                .wp = -1,
            },
            .ldo_chan_id = -1,
        },
    },
};

const static dev_display_lcd_config_t esp_bmgr_display_lcd_cfg = {
    .name = "display_lcd",
    .chip = "generic_rgb",
    .sub_type = "rgb",
    .lcd_width = 800,
    .lcd_height = 480,
    .swap_xy = false,
    .mirror_x = false,
    .mirror_y = false,
    .need_reset = true,
    .invert_color = false,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
    .frame_format = DEV_DISPLAY_LCD_FRAME_FORMAT_RGB565_LE,
    .sub_cfg = {
        .rgb = {
            .user_fbs_func = "",
            .panel_config = {
                .clk_src = LCD_CLK_SRC_DEFAULT,
                .timings = {
                    .pclk_hz = 18000000,
                    .h_res = 800,
                    .v_res = 480,
                    .hsync_pulse_width = 1,
                    .hsync_back_porch = 40,
                    .hsync_front_porch = 20,
                    .vsync_pulse_width = 1,
                    .vsync_back_porch = 10,
                    .vsync_front_porch = 5,
                    .flags = {
                        .hsync_idle_low = false,
                        .vsync_idle_low = false,
                        .de_idle_high = false,
                        .pclk_active_neg = true,
                        .pclk_idle_high = false,
                    },
                },
                .data_width = 16,
                .num_fbs = 1,
                .bounce_buffer_size_px = 0,
                .dma_burst_size = 64,
                .hsync_gpio_num = 44,
                .vsync_gpio_num = 45,
                .de_gpio_num = 43,
                .pclk_gpio_num = 40,
                .disp_gpio_num = -1,
                .data_gpio_nums = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 33, 34, 35, 36},
                .flags = {
                    .disp_active_low = false,
                    .refresh_on_demand = false,
                    .fb_in_psram = true,
                    .double_fb = false,
                    .no_fb = false,
                    .bb_invalidate_cache = false,
                },
                .in_color_format = LCD_COLOR_FMT_RGB565,
                .out_color_format = LCD_COLOR_FMT_RGB565,
            },
        },
    },
};

const static dev_lcd_touch_config_t esp_bmgr_lcd_touch_cfg = {
    .name = "lcd_touch",
    .chip = "gt1151",
    .type = "lcd_touch",
    .sub_type = "i2c",
    .touch_config = {
        .x_max = 800,
        .y_max = 480,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    },
    .sub_cfg = {
        .i2c = {
            .i2c_name = "i2c_master",
            .i2c_addr_count = 1,
            .i2c_addr = {0x28, 0x00, 0x00, 0x00},
            .io_i2c_config = {
                .dev_addr = 0,
                .control_phase_bytes = 1,
                .dc_bit_offset = 0,
                .lcd_cmd_bits = 16,
                .lcd_param_bits = 0,
                .scl_speed_hz = 400000,
                .flags = {
                    .dc_low_on_data = false,
                    .disable_control_phase = true,
                },
                .transaction_timeout_ms = 0,
            },
        },
    },
};

const static dev_ledc_ctrl_config_t esp_bmgr_camera_xclk_cfg = {
    .name = "camera_xclk",
    .type = "ledc_ctrl",
    .ledc_name = "ledc_camera_xclk",
    .default_percent = 100,
};

const static dev_custom_camera_sensor_bringup_config_t esp_bmgr_camera_sensor_bringup_cfg = {
    .name = "camera_sensor_bringup",
    .type = "custom",
    .chip = "unknown",
    .i2c_name = "i2c_master",
    .sccb_addr = 60,
    .reset_reg = 12296,
    .reset_value = 130,
    .xclk_stable_ms = 50,
    .reset_delay_ms = 20,
    .sccb_freq_hz = 200000,
    .timeout_ms = 100,
    .peripheral_count = 1,
    .peripheral_name = "i2c_master",
};

const static dev_camera_config_t esp_bmgr_camera_cfg = {
    .name = "camera",
    .type = "camera",
    .sub_type = "dvp",
    .sub_cfg = {
        .dvp = {
            .i2c_name = "i2c_master",
            .i2c_freq = 200000,
            .reset_io = -1,
            .pwdn_io = -1,
            .dvp_io = {
                .data_width = CAM_CTLR_DATA_WIDTH_8,
                .data_io = {46, 47, 48, 49, 50, 51, 52, 53, -1, -1, -1, -1, -1, -1, -1, -1},
                .vsync_io = 56,
                .de_io = 57,
                .pclk_io = 54,
                .xclk_io = -1,
            },
            .xclk_freq = 0,
        },
    },
};

// Device descriptor array
static const char* esp_bmgr_camera_sensor_bringup_deps[] = {
    "camera_xclk",
};
static const char* esp_bmgr_camera_deps[] = {
    "camera_sensor_bringup",
};

const esp_board_device_desc_t g_esp_board_devices[] = {
    {
        .next = &g_esp_board_devices[1],
        .name = "sdcard_power_ctrl",
        .chip = NULL,
        .type = "power_ctrl",
        .sub_type = "gpio",
        .cfg = &esp_bmgr_sdcard_power_ctrl_cfg,
        .cfg_size = sizeof(esp_bmgr_sdcard_power_ctrl_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[2],
        .name = "boot_button",
        .chip = NULL,
        .type = "button",
        .sub_type = "gpio",
        .cfg = &esp_bmgr_boot_button_cfg,
        .cfg_size = sizeof(esp_bmgr_boot_button_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[3],
        .name = "led_strip",
        .chip = "ws2812",
        .type = "led_strip",
        .sub_type = "rmt",
        .cfg = &esp_bmgr_led_strip_cfg,
        .cfg_size = sizeof(esp_bmgr_led_strip_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[4],
        .name = "adc_button_group",
        .chip = NULL,
        .type = "button",
        .sub_type = "adc_multi",
        .cfg = &esp_bmgr_adc_button_group_cfg,
        .cfg_size = sizeof(esp_bmgr_adc_button_group_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[5],
        .name = "audio_dac",
        .chip = "es8389",
        .type = "audio_codec",
        .sub_type = NULL,
        .cfg = &esp_bmgr_audio_dac_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_dac_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[6],
        .name = "audio_adc",
        .chip = "es8389",
        .type = "audio_codec",
        .sub_type = NULL,
        .cfg = &esp_bmgr_audio_adc_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_adc_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[7],
        .name = "fs_sdcard",
        .chip = NULL,
        .type = "fs_fat",
        .sub_type = "sdmmc",
        .cfg = &esp_bmgr_fs_sdcard_cfg,
        .cfg_size = sizeof(esp_bmgr_fs_sdcard_cfg),
        .init_skip = false,
        .power_ctrl_device = "sdcard_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[8],
        .name = "display_lcd",
        .chip = "generic_rgb",
        .type = "display_lcd",
        .sub_type = "rgb",
        .cfg = &esp_bmgr_display_lcd_cfg,
        .cfg_size = sizeof(esp_bmgr_display_lcd_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[9],
        .name = "lcd_touch",
        .chip = "gt1151",
        .type = "lcd_touch",
        .sub_type = "i2c",
        .cfg = &esp_bmgr_lcd_touch_cfg,
        .cfg_size = sizeof(esp_bmgr_lcd_touch_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[10],
        .name = "camera_xclk",
        .chip = NULL,
        .type = "ledc_ctrl",
        .sub_type = NULL,
        .cfg = &esp_bmgr_camera_xclk_cfg,
        .cfg_size = sizeof(esp_bmgr_camera_xclk_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[11],
        .name = "camera_sensor_bringup",
        .chip = NULL,
        .type = "custom",
        .sub_type = NULL,
        .cfg = &esp_bmgr_camera_sensor_bringup_cfg,
        .cfg_size = sizeof(esp_bmgr_camera_sensor_bringup_cfg),
        .init_skip = false,
        .depends_on = esp_bmgr_camera_sensor_bringup_deps,
        .depends_on_num = 1,
    },
    {
        .next = NULL,
        .name = "camera",
        .chip = NULL,
        .type = "camera",
        .sub_type = "dvp",
        .cfg = &esp_bmgr_camera_cfg,
        .cfg_size = sizeof(esp_bmgr_camera_cfg),
        .init_skip = false,
        .depends_on = esp_bmgr_camera_deps,
        .depends_on_num = 1,
    },
};
