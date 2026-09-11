/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * Modified for the ESP32-S31 Camera Lab project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FACE_RECOGNITION_SERVICE_H
#define FACE_RECOGNITION_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_RECOGNITION_INPUT_WIDTH       640U
#define FACE_RECOGNITION_INPUT_HEIGHT      480U
#define FACE_RECOGNITION_MAX_FACES         4U
#define FACE_RECOGNITION_MAX_IDENTITIES    4U
#define FACE_RECOGNITION_ENROLL_SAMPLES    3U
#define FACE_RECOGNITION_NAME_SIZE         16U
#define FACE_RECOGNITION_UNKNOWN_ID        0U

typedef enum {
    FACE_RECOGNITION_STATE_STOPPED = 0,
    FACE_RECOGNITION_STATE_STARTING,
    FACE_RECOGNITION_STATE_READY,
    FACE_RECOGNITION_STATE_RUNNING,
    FACE_RECOGNITION_STATE_ERROR,
} face_recognition_state_t;

/** One detected face, expressed in the original 640 x 480 source coordinates. */
typedef struct {
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
    float score;
    float similarity; /**< Best dot-product score, or -1 when recognition was not run. */
    uint8_t identity_id; /**< 1..4 when recognized, FACE_RECOGNITION_UNKNOWN_ID otherwise. */
    bool primary;       /**< The largest face, which is the only face recognized/enrolled. */
    char name[FACE_RECOGNITION_NAME_SIZE];
} face_recognition_face_t;

typedef struct {
    uint8_t id;
    bool valid;
    char name[FACE_RECOGNITION_NAME_SIZE];
} face_recognition_identity_t;

/**
 * Fixed-size POD snapshot. It can be copied by UI/console code without owning
 * any model, frame, or database memory.
 */
typedef struct {
    face_recognition_state_t state;
    esp_err_t last_error;
    uint32_t generation;
    uint32_t age_ms;
    uint32_t frame_sequence;
    uint32_t inference_ms;
    uint32_t inference_max_ms;
    uint32_t task_stack_min_free_bytes;

    uint8_t face_count;
    uint8_t identity_count;
    uint8_t enroll_target_id;
    uint8_t enroll_samples_collected;
    bool enrolling;
    bool service_ready;
    bool model_ready;

    face_recognition_face_t faces[FACE_RECOGNITION_MAX_FACES];
    face_recognition_identity_t identities[FACE_RECOGNITION_MAX_IDENTITIES];

    uint32_t frames_submitted;
    uint32_t frames_accepted;
    uint32_t frames_dropped_busy;
    uint32_t frames_dropped_throttled;
    uint32_t invalid_frames;
    uint32_t inference_count;
    uint32_t faces_detected;
    uint32_t recognition_count;
    uint32_t enroll_count;
    uint32_t command_drops;
    uint32_t error_count;
} face_recognition_snapshot_t;

/**
 * Start the low-priority face task and allocate its single 320 x 240 RGB565LE
 * work frame plus volatile feature database in PSRAM.
 */
esp_err_t face_recognition_service_start(void);

/**
 * Non-waiting USB-camera submission entry point.
 *
 * The source must remain readable only for the duration of this call. When the
 * service-owned PSRAM buffer is free and the 200 ms admission interval has
 * elapsed, this call synchronously copies a 640 x 480 frame at 2:1 or a
 * 320 x 240 frame at 1:1 into the tightly packed internal RGB565LE frame. It
 * honors the source stride, never retains the source pointer, and never waits
 * for the AI task. Call from task context, not an ISR.
 */
bool face_recognition_service_submit_rgb565le(const uint8_t *data,
                                               size_t data_size,
                                               uint16_t width,
                                               uint16_t height,
                                               uint16_t stride,
                                               uint32_t sequence);

/** Begin a three-sample enrollment into the next PERSON 01..PERSON 04 slot. */
bool face_recognition_service_request_enroll(void);

/** Cancel only the active, incomplete enrollment; keep existing identities. */
bool face_recognition_service_request_cancel_enroll(void);

/** Delete the most recently enrolled volatile identity. */
bool face_recognition_service_request_delete_last(void);

/** Clear every volatile identity and cancel an active enrollment. */
bool face_recognition_service_request_clear(void);

/** Copy the latest fixed-size result and current diagnostics. */
bool face_recognition_service_get_snapshot(face_recognition_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* FACE_RECOGNITION_SERVICE_H */
