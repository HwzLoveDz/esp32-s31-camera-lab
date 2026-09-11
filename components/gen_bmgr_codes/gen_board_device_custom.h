/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated custom device structure definitions
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Custom device structure definitions
// These structures are dynamically generated based on YAML configuration

// Structure definition for camera_sensor_bringup
typedef struct {
    const char *name;           /*!< Custom device name */
    const char *type;           /*!< Device type: "custom" */
    const char *chip;           /*!< Chip name */
    const char * i2c_name;
    int8_t       sccb_addr;
    int16_t      reset_reg;
    uint8_t      reset_value;
    int8_t       xclk_stable_ms;
    int8_t       reset_delay_ms;
    int32_t      sccb_freq_hz;
    int8_t       timeout_ms;
    uint8_t     peripheral_count;
    const char *peripheral_name;
} dev_custom_camera_sensor_bringup_config_t;
