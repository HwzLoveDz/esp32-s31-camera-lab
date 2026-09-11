/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * Modified for the ESP32-S31 Camera Lab project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ONBOARD_CAMERA_H
#define ONBOARD_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ONBOARD_CAMERA_STATE_STOPPED = 0,
    ONBOARD_CAMERA_STATE_STARTING,
    ONBOARD_CAMERA_STATE_STREAMING,
    ONBOARD_CAMERA_STATE_ERROR,
} onboard_camera_state_t;

/**
 * @brief RGB565 frame leased to the LVGL task.
 *
 * The lease token fields are private to the service and must be returned
 * unchanged to onboard_camera_present_frame() or onboard_camera_release_frame().
 */
typedef struct {
    const uint8_t *data;
    size_t data_size;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint32_t sequence;
    bool byte_swapped;
    uint8_t _slot;
    uint32_t _generation;
} onboard_camera_frame_t;

typedef struct {
    onboard_camera_state_t state;
    esp_err_t last_error;
    float fps;
    uint16_t width;
    uint16_t height;
    uint32_t frames_captured;
    uint32_t display_drops;
    uint32_t capture_errors;
    uint32_t copy_time_max_us;
    uint32_t task_stack_min_free_bytes;
    bool service_ready;
    bool streaming;
    bool frame_available;
} onboard_camera_stats_t;

/** Start the board-manager DVP camera capture service on /dev/video2. */
esp_err_t onboard_camera_start(void);

/** Copy a consistent, non-blocking diagnostics snapshot. */
bool onboard_camera_get_stats(onboard_camera_stats_t *out_stats);

/**
 * Lease the newest complete frame without blocking.
 *
 * This is a single-consumer API. Call it only from the LVGL task and keep at
 * most one acquired lease at a time.
 */
bool onboard_camera_acquire_latest_frame(onboard_camera_frame_t *out_frame);

/** Retain a leased frame for display and release the previous displayed slot. */
void onboard_camera_present_frame(const onboard_camera_frame_t *frame);

/** Release an acquired or displayed frame after LVGL no longer references it. */
void onboard_camera_release_frame(const onboard_camera_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* ONBOARD_CAMERA_H */
