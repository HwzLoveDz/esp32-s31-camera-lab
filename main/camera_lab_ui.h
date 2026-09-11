/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t usb_presentations;
    uint32_t onboard_presentations;
    uint32_t rejected_frames;
    uint32_t swap_count;
    uint32_t freeze_count;
    uint32_t clean_count;
    uint32_t held_gap_count;
    uint32_t usb_frame_age_ms;
    uint32_t onboard_frame_age_ms;
    uint32_t usb_sequence;
    uint32_t onboard_sequence;
    uint8_t session_max_faces;
    bool ready;
    bool frozen;
    bool clean_view;
    bool onboard_is_main;
    bool gallery_open;
    bool usb_frame_presented;
    bool onboard_frame_presented;
} camera_lab_ui_stats_t;

/** Initialize the display, touch and permanent dual-camera scene after board init. */
esp_err_t camera_lab_ui_start(void);

/** Queue a UI action from any task: swap, freeze, clean, gallery, add, cancel, delete.
 * delete requires a second delete within 4 seconds, matching the touch control. */
bool camera_lab_ui_command(const char *command);

/** Copy published UI diagnostics without touching LVGL from the caller's task. */
bool camera_lab_ui_get_stats(camera_lab_ui_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
