/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * Modified for the ESP32-S31 Camera Lab project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_CAMERA_H
#define USB_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_CAMERA_STATE_STOPPED = 0,
    USB_CAMERA_STATE_WAITING,
    USB_CAMERA_STATE_OPENING,
    USB_CAMERA_STATE_STREAMING,
    USB_CAMERA_STATE_ERROR,
} usb_camera_state_t;

typedef enum {
    USB_CAMERA_FORMAT_NONE = 0,
    USB_CAMERA_FORMAT_MJPEG,
    USB_CAMERA_FORMAT_YUY2,
} usb_camera_format_t;

/**
 * @brief A decoded frame leased to the UI.
 *
 * Pixels are row-major native/little-endian RGB565, suitable for an LVGL image
 * descriptor with LV_COLOR_FORMAT_RGB565. Use @c stride when advancing between
 * rows because hardware JPEG output may include MCU padding. Fields beginning
 * with an underscore are an opaque lease token and must be returned unchanged.
 */
typedef struct {
    const uint8_t *data;
    size_t data_size;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint32_t sequence;
    uint8_t _slot;
    uint32_t _generation;
} usb_camera_frame_t;

typedef struct {
    usb_camera_state_t state;
    usb_camera_format_t source_format;
    esp_err_t last_error;
    float fps;
    uint16_t width;
    uint16_t height;
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t frames_received;
    uint32_t frames_decoded;
    uint32_t callback_drops;
    uint32_t display_drops;
    uint32_t decode_errors;
    uint32_t transfer_errors;
    uint32_t frame_overflows;
    uint32_t frame_underflows;
    uint32_t manager_stack_min_free_bytes;
    uint32_t decode_stack_min_free_bytes;
    bool service_ready;
    bool device_connected;
    bool streaming;
    bool frame_available;
} usb_camera_stats_t;

/**
 * @brief Start UVC support on the application's existing USB Host Library.
 *
 * usb_keyboard_start() must have installed the one shared USB Host Library
 * instance before this function is called. The service remains active across
 * camera unplug/replug cycles.
 */
esp_err_t usb_camera_start(void);

/** Copy a consistent, non-blocking snapshot of camera state and counters. */
bool usb_camera_get_stats(usb_camera_stats_t *out_stats);

/**
 * @brief Lease the newest decoded frame without blocking.
 *
 * At most one unpresented lease is allowed. Older waiting frames are discarded
 * automatically. On success, call usb_camera_present_frame() after changing
 * the LVGL image source, or usb_camera_release_frame() if the frame is unused.
 */
bool usb_camera_acquire_latest_frame(usb_camera_frame_t *out_frame);

/**
 * @brief Make a leased frame the stable displayed buffer.
 *
 * This atomically releases the previous displayed buffer. The new buffer stays
 * immutable until a later frame is presented or it is explicitly released.
 */
void usb_camera_present_frame(const usb_camera_frame_t *frame);

/** Release an acquired or displayed frame when the UI no longer references it. */
void usb_camera_release_frame(const usb_camera_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* USB_CAMERA_H */
