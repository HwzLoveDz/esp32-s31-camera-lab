/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * Modified for the ESP32-S31 Camera Lab project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "face_recognition_service.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

#include "dl_image_define.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

namespace {

constexpr uint16_t kWorkWidth = FACE_RECOGNITION_INPUT_WIDTH / 2U;
constexpr uint16_t kWorkHeight = FACE_RECOGNITION_INPUT_HEIGHT / 2U;
constexpr size_t kWorkStride = static_cast<size_t>(kWorkWidth) * 2U;
constexpr size_t kWorkBytes = kWorkStride * kWorkHeight;
constexpr uint32_t kAdmissionIntervalMs = 200U;
constexpr uint32_t kTaskStackBytes = 32U * 1024U;
constexpr UBaseType_t kTaskPriority = 1U;
constexpr UBaseType_t kCommandQueueLength = 8U;
constexpr int kMaxFeatureLength = 4096;
constexpr float kRecognitionThreshold = 0.55F;
constexpr float kEnrollmentConsistencyThreshold = 0.60F;

#if CONFIG_FREERTOS_UNICORE
constexpr BaseType_t kTaskCore = tskNO_AFFINITY;
#elif CONFIG_IDF_TARGET_ESP32S31
// S31 PIE V2 is available on Core 1. Start there explicitly instead of relying
// on the first PIE instruction to migrate a task initially pinned to Core 0.
constexpr BaseType_t kTaskCore = 1;
#else
constexpr BaseType_t kTaskCore = 0;
#endif

enum FrameState : uint32_t {
    FRAME_FREE = 0,
    FRAME_WRITING,
    FRAME_READY,
    FRAME_READING,
};

enum class CommandType : uint8_t {
    DELETE_LAST = 0,
    CLEAR,
};

constexpr uint32_t kEnrollmentDesiredMask = 1U;

struct Command {
    CommandType type;
};

struct ServiceContext {
    uint8_t *frame;
    TaskHandle_t task;
    QueueHandle_t command_queue;
    StaticQueue_t command_queue_control;
    uint8_t command_storage[kCommandQueueLength * sizeof(Command)];

    volatile uint32_t frame_state;
    volatile uint32_t started;
    volatile uint32_t service_ready;
    volatile uint32_t model_ready;
    volatile uint32_t state;
    volatile int32_t last_error;
    volatile uint32_t frame_sequence;
    volatile uint32_t last_accepted_tick;
    volatile uint32_t snapshot_tick;
    volatile uint32_t enrollment_control;
    volatile uint32_t identity_count;

    volatile uint32_t frames_submitted;
    volatile uint32_t frames_accepted;
    volatile uint32_t frames_dropped_busy;
    volatile uint32_t frames_dropped_throttled;
    volatile uint32_t invalid_frames;
    volatile uint32_t inference_count;
    volatile uint32_t inference_max_ms;
    volatile uint32_t faces_detected;
    volatile uint32_t recognition_count;
    volatile uint32_t enroll_count;
    volatile uint32_t command_drops;
    volatile uint32_t error_count;
    volatile uint32_t task_stack_min_free_bytes;
};

static const char *const TAG = "face_service";
static ServiceContext s_service = {};
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static face_recognition_snapshot_t s_snapshot = {};

inline uint32_t atomic_load_u32(const volatile uint32_t *value, int order = __ATOMIC_ACQUIRE)
{
    return __atomic_load_n(value, order);
}

inline void atomic_store_u32(volatile uint32_t *value, uint32_t next, int order = __ATOMIC_RELEASE)
{
    __atomic_store_n(value, next, order);
}

inline uint32_t atomic_increment(volatile uint32_t *value)
{
    return __atomic_add_fetch(value, 1U, __ATOMIC_RELAXED);
}

inline int32_t atomic_load_i32(const volatile int32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

inline void atomic_store_i32(volatile int32_t *value, int32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

bool atomic_compare_exchange_u32(volatile uint32_t *value, uint32_t expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(value, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

void atomic_update_max(volatile uint32_t *value, uint32_t candidate)
{
    uint32_t previous = atomic_load_u32(value, __ATOMIC_RELAXED);
    while (candidate > previous &&
           !__atomic_compare_exchange_n(value,
                                        &previous,
                                        candidate,
                                        true,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

uint32_t enrollment_control_next(uint32_t current, bool desired)
{
    return ((current & ~kEnrollmentDesiredMask) + 2U) |
           (desired ? kEnrollmentDesiredMask : 0U);
}

bool enrollment_control_matches(uint32_t active_control)
{
    return (active_control & kEnrollmentDesiredMask) != 0U &&
           atomic_load_u32(&s_service.enrollment_control) == active_control;
}

uint32_t set_enrollment_intent(bool desired)
{
    uint32_t current = atomic_load_u32(&s_service.enrollment_control);
    for (;;) {
        const uint32_t next = enrollment_control_next(current, desired);
        if (__atomic_compare_exchange_n(&s_service.enrollment_control,
                                        &current,
                                        next,
                                        false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return next;
        }
    }
}

uint32_t elapsed_ms(TickType_t now, uint32_t before)
{
    return static_cast<uint32_t>(now - static_cast<TickType_t>(before)) * portTICK_PERIOD_MS;
}

uint16_t scale_coordinate(int coordinate, uint16_t maximum)
{
    if (coordinate <= 0) {
        return 0;
    }
    const uint32_t scaled = static_cast<uint32_t>(coordinate) * 2U;
    return static_cast<uint16_t>(scaled < maximum ? scaled : maximum);
}

void make_identity_name(uint8_t id, char *destination, size_t destination_size)
{
    std::snprintf(destination, destination_size, "PERSON %02u", static_cast<unsigned>(id));
}

void fill_identities(face_recognition_snapshot_t &snapshot, uint8_t identity_count)
{
    std::memset(snapshot.identities, 0, sizeof(snapshot.identities));
    snapshot.identity_count = identity_count;
    for (uint8_t i = 0; i < identity_count && i < FACE_RECOGNITION_MAX_IDENTITIES; ++i) {
        snapshot.identities[i].id = static_cast<uint8_t>(i + 1U);
        snapshot.identities[i].valid = true;
        make_identity_name(snapshot.identities[i].id,
                           snapshot.identities[i].name,
                           sizeof(snapshot.identities[i].name));
    }
}

void publish_snapshot(face_recognition_snapshot_t &snapshot)
{
    snapshot.state = static_cast<face_recognition_state_t>(atomic_load_u32(&s_service.state));
    snapshot.last_error = static_cast<esp_err_t>(atomic_load_i32(&s_service.last_error));
    snapshot.service_ready = atomic_load_u32(&s_service.service_ready) != 0U;
    snapshot.model_ready = atomic_load_u32(&s_service.model_ready) != 0U;
    ++snapshot.generation;

    const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount());
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = snapshot;
    atomic_store_u32(&s_service.snapshot_tick, now, __ATOMIC_RELAXED);
    portEXIT_CRITICAL(&s_snapshot_lock);
}

void record_error(esp_err_t error, bool fatal)
{
    atomic_store_i32(&s_service.last_error, static_cast<int32_t>(error));
    atomic_increment(&s_service.error_count);
    if (fatal) {
        atomic_store_u32(&s_service.model_ready, 0U);
        atomic_store_u32(&s_service.service_ready, 0U);
        atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_ERROR);
    }
}

bool valid_feature(dl::TensorBase *tensor, int feature_length)
{
    if (tensor == nullptr || tensor->data == nullptr || tensor->get_dtype() != dl::DATA_TYPE_FLOAT ||
        tensor->get_size() != feature_length) {
        return false;
    }
    const float *feature = static_cast<const float *>(tensor->data);
    for (int i = 0; i < feature_length; ++i) {
        if (!std::isfinite(feature[i])) {
            return false;
        }
    }
    return true;
}

void normalize_vector(float *values, int length)
{
    float sum_of_squares = 0.0F;
    for (int i = 0; i < length; ++i) {
        sum_of_squares += values[i] * values[i];
    }
    if (!(sum_of_squares > 0.0F) || !std::isfinite(sum_of_squares)) {
        std::memset(values, 0, static_cast<size_t>(length) * sizeof(float));
        return;
    }
    const float reciprocal_norm = 1.0F / std::sqrt(sum_of_squares);
    for (int i = 0; i < length; ++i) {
        values[i] *= reciprocal_norm;
    }
}

float dot_product(const float *left, const float *right, int length)
{
    float score = 0.0F;
    for (int i = 0; i < length; ++i) {
        score += left[i] * right[i];
    }
    return score;
}

bool enrollment_sample_matches(const float *feature,
                               const float *enrollment_sum,
                               int feature_length,
                               uint8_t samples_collected)
{
    if (samples_collected == 0U) {
        return true;
    }

    float dot = 0.0F;
    float sum_norm_squared = 0.0F;
    for (int i = 0; i < feature_length; ++i) {
        dot += feature[i] * enrollment_sum[i];
        sum_norm_squared += enrollment_sum[i] * enrollment_sum[i];
    }
    if (!(sum_norm_squared > 0.0F) || !std::isfinite(sum_norm_squared)) {
        return false;
    }
    const float similarity = dot / std::sqrt(sum_norm_squared);
    return std::isfinite(similarity) && similarity >= kEnrollmentConsistencyThreshold;
}

void clear_visible_faces(face_recognition_snapshot_t &snapshot)
{
    snapshot.face_count = 0U;
    std::memset(snapshot.faces, 0, sizeof(snapshot.faces));
}

void process_commands(face_recognition_snapshot_t &snapshot,
                      float *database,
                      float *enrollment_sum,
                      int feature_length,
                      uint8_t &identity_count,
                      bool &enrolling,
                      uint8_t &enroll_samples)
{
    Command command = {};
    bool changed = false;
    while (xQueueReceive(s_service.command_queue, &command, 0) == pdTRUE) {
        changed = true;
        switch (command.type) {
        case CommandType::DELETE_LAST:
            enrolling = false;
            enroll_samples = 0U;
            std::memset(enrollment_sum, 0, static_cast<size_t>(feature_length) * sizeof(float));
            if (identity_count == 0U) {
                record_error(ESP_ERR_INVALID_STATE, false);
            } else {
                --identity_count;
                std::memset(database + static_cast<size_t>(identity_count) * feature_length,
                            0,
                            static_cast<size_t>(feature_length) * sizeof(float));
            }
            atomic_store_u32(&s_service.identity_count, identity_count, __ATOMIC_RELAXED);
            clear_visible_faces(snapshot);
            break;

        case CommandType::CLEAR:
            identity_count = 0U;
            enrolling = false;
            enroll_samples = 0U;
            std::memset(database,
                        0,
                        static_cast<size_t>(FACE_RECOGNITION_MAX_IDENTITIES) * feature_length * sizeof(float));
            std::memset(enrollment_sum, 0, static_cast<size_t>(feature_length) * sizeof(float));
            atomic_store_u32(&s_service.identity_count, 0U, __ATOMIC_RELAXED);
            clear_visible_faces(snapshot);
            break;
        }
    }

    if (changed) {
        fill_identities(snapshot, identity_count);
        snapshot.enrolling = enrolling;
        snapshot.enroll_target_id = enrolling ? static_cast<uint8_t>(identity_count + 1U) : 0U;
        snapshot.enroll_samples_collected = enroll_samples;
        publish_snapshot(snapshot);
    }
}

bool apply_enrollment_intent(face_recognition_snapshot_t &snapshot,
                             float *enrollment_sum,
                             int feature_length,
                             uint8_t identity_count,
                             uint32_t &active_control,
                             bool &enrolling,
                             uint8_t &enroll_samples)
{
    const uint32_t requested_control = atomic_load_u32(&s_service.enrollment_control);
    if (requested_control == active_control) {
        return false;
    }

    active_control = requested_control;
    enroll_samples = 0U;
    std::memset(enrollment_sum, 0, static_cast<size_t>(feature_length) * sizeof(float));
    enrolling = (requested_control & kEnrollmentDesiredMask) != 0U &&
                identity_count < FACE_RECOGNITION_MAX_IDENTITIES;

    if (!enrolling && (requested_control & kEnrollmentDesiredMask) != 0U) {
        const uint32_t completed_control = requested_control & ~kEnrollmentDesiredMask;
        if (atomic_compare_exchange_u32(&s_service.enrollment_control,
                                        requested_control,
                                        completed_control)) {
            active_control = completed_control;
        }
    }

    snapshot.enrolling = enrolling;
    snapshot.enroll_target_id = enrolling ? static_cast<uint8_t>(identity_count + 1U) : 0U;
    snapshot.enroll_samples_collected = 0U;
    publish_snapshot(snapshot);
    return true;
}

size_t select_largest_faces(std::list<dl::detect::result_t> &detections,
                            const dl::detect::result_t **selected)
{
    size_t count = 0U;
    for (dl::detect::result_t &candidate : detections) {
        if (candidate.box.size() < 4U || candidate.keypoint.size() != 10U || candidate.box[2] <= candidate.box[0] ||
            candidate.box[3] <= candidate.box[1]) {
            continue;
        }

        const int candidate_area = candidate.box_area();
        size_t position = 0U;
        while (position < count && selected[position]->box_area() >= candidate_area) {
            ++position;
        }
        if (position >= FACE_RECOGNITION_MAX_FACES) {
            continue;
        }
        const size_t last = count < FACE_RECOGNITION_MAX_FACES ? count : FACE_RECOGNITION_MAX_FACES - 1U;
        for (size_t i = last; i > position; --i) {
            selected[i] = selected[i - 1U];
        }
        selected[position] = &candidate;
        if (count < FACE_RECOGNITION_MAX_FACES) {
            ++count;
        }
    }
    return count;
}

void copy_detected_faces(face_recognition_snapshot_t &snapshot,
                         const dl::detect::result_t *const *selected,
                         size_t selected_count)
{
    clear_visible_faces(snapshot);
    snapshot.face_count = static_cast<uint8_t>(selected_count);
    for (size_t i = 0; i < selected_count; ++i) {
        face_recognition_face_t &face = snapshot.faces[i];
        const dl::detect::result_t &detected = *selected[i];
        face.x1 = scale_coordinate(detected.box[0], FACE_RECOGNITION_INPUT_WIDTH - 1U);
        face.y1 = scale_coordinate(detected.box[1], FACE_RECOGNITION_INPUT_HEIGHT - 1U);
        face.x2 = scale_coordinate(detected.box[2], FACE_RECOGNITION_INPUT_WIDTH - 1U);
        face.y2 = scale_coordinate(detected.box[3], FACE_RECOGNITION_INPUT_HEIGHT - 1U);
        face.score = detected.score;
        face.similarity = -1.0F;
        face.identity_id = FACE_RECOGNITION_UNKNOWN_ID;
        face.primary = (i == 0U);
    }
}

void process_frame(face_recognition_snapshot_t &snapshot,
                   HumanFaceDetect &detector,
                   HumanFaceFeat &feature_model,
                   float *database,
                   float *enrollment_sum,
                   int feature_length,
                   uint8_t &identity_count,
                   uint32_t &active_enrollment_control,
                   bool &enrolling,
                   uint8_t &enroll_samples)
{
    atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_RUNNING);
    const int64_t started_us = esp_timer_get_time();

    dl::image::img_t image = {};
    image.data = s_service.frame;
    image.width = kWorkWidth;
    image.height = kWorkHeight;
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

    std::list<dl::detect::result_t> &detections = detector.run(image);
    const dl::detect::result_t *selected[FACE_RECOGNITION_MAX_FACES] = {};
    const size_t selected_count = select_largest_faces(detections, selected);
    copy_detected_faces(snapshot, selected, selected_count);
    atomic_store_u32(&s_service.faces_detected,
                     atomic_load_u32(&s_service.faces_detected, __ATOMIC_RELAXED) +
                         static_cast<uint32_t>(selected_count),
                     __ATOMIC_RELAXED);

    const bool enrollment_active = enrolling &&
                                   enrollment_control_matches(active_enrollment_control);
    if (selected_count > 0U && (identity_count > 0U || enrollment_active)) {
        dl::TensorBase *feature_tensor = feature_model.run(image, selected[0]->keypoint);
        if (valid_feature(feature_tensor, feature_length)) {
            const float *feature = static_cast<const float *>(feature_tensor->data);

            /* Only build a profile from one clearly isolated person. If the
             * face changes between samples, restart from the current sample
             * instead of averaging two people into one identity. */
            if (enrollment_active && selected_count == 1U &&
                    enrollment_control_matches(active_enrollment_control)) {
                if (!enrollment_sample_matches(feature, enrollment_sum,
                                               feature_length, enroll_samples)) {
                    enroll_samples = 0U;
                    std::memset(enrollment_sum, 0,
                                static_cast<size_t>(feature_length) * sizeof(float));
                }
                for (int i = 0; i < feature_length; ++i) {
                    enrollment_sum[i] += feature[i];
                }
                ++enroll_samples;
                if (!enrollment_control_matches(active_enrollment_control)) {
                    enrolling = false;
                    enroll_samples = 0U;
                    std::memset(enrollment_sum, 0,
                                static_cast<size_t>(feature_length) * sizeof(float));
                } else if (enroll_samples >= FACE_RECOGNITION_ENROLL_SAMPLES) {
                    float *destination = database + static_cast<size_t>(identity_count) * feature_length;
                    for (int i = 0; i < feature_length; ++i) {
                        destination[i] = enrollment_sum[i] / static_cast<float>(FACE_RECOGNITION_ENROLL_SAMPLES);
                    }
                    normalize_vector(destination, feature_length);

                    /* The desired bit is the enrollment commit point. A close,
                     * USB teardown, or a newer request changes the epoch and
                     * makes this CAS fail, so a hidden enrollment cannot add an
                     * identity after cancellation has won the race. */
                    const uint32_t completed_control =
                        active_enrollment_control & ~kEnrollmentDesiredMask;
                    const bool committed = atomic_compare_exchange_u32(
                        &s_service.enrollment_control,
                        active_enrollment_control,
                        completed_control);
                    if (committed) {
                        active_enrollment_control = completed_control;
                        ++identity_count;
                        atomic_store_u32(&s_service.identity_count,
                                         identity_count,
                                         __ATOMIC_RELAXED);
                        atomic_increment(&s_service.enroll_count);
                    } else {
                        std::memset(destination, 0,
                                    static_cast<size_t>(feature_length) * sizeof(float));
                    }
                    enrolling = false;
                    enroll_samples = 0U;
                    std::memset(enrollment_sum, 0, static_cast<size_t>(feature_length) * sizeof(float));
                }
            }

            if (identity_count > 0U) {
                float best_similarity = -1.0F;
                uint8_t best_index = 0U;
                for (uint8_t i = 0; i < identity_count; ++i) {
                    const float similarity =
                        dot_product(feature, database + static_cast<size_t>(i) * feature_length, feature_length);
                    if (similarity > best_similarity) {
                        best_similarity = similarity;
                        best_index = i;
                    }
                }
                snapshot.faces[0].similarity = best_similarity;
                if (best_similarity >= kRecognitionThreshold) {
                    snapshot.faces[0].identity_id = static_cast<uint8_t>(best_index + 1U);
                    make_identity_name(snapshot.faces[0].identity_id,
                                       snapshot.faces[0].name,
                                       sizeof(snapshot.faces[0].name));
                }
                atomic_increment(&s_service.recognition_count);
            }
        } else {
            record_error(ESP_ERR_INVALID_RESPONSE, false);
        }
    }

    if (enrolling && !enrollment_control_matches(active_enrollment_control)) {
        enrolling = false;
        enroll_samples = 0U;
        std::memset(enrollment_sum, 0, static_cast<size_t>(feature_length) * sizeof(float));
    }

    fill_identities(snapshot, identity_count);
    snapshot.enrolling = enrolling;
    snapshot.enroll_target_id = enrolling ? static_cast<uint8_t>(identity_count + 1U) : 0U;
    snapshot.enroll_samples_collected = enroll_samples;
    snapshot.frame_sequence = atomic_load_u32(&s_service.frame_sequence);
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    snapshot.inference_ms = static_cast<uint32_t>((elapsed_us + 999LL) / 1000LL);
    atomic_update_max(&s_service.inference_max_ms, snapshot.inference_ms);
    atomic_increment(&s_service.inference_count);
    if (atomic_load_u32(&s_service.inference_count) == 1U) {
        ESP_LOGI(TAG, "First inference complete: core=%d, elapsed=%" PRIu32 " ms",
                 xPortGetCoreID(), snapshot.inference_ms);
    }
    const uint32_t watermark = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    atomic_store_u32(&s_service.task_stack_min_free_bytes, watermark, __ATOMIC_RELAXED);

    atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_READY);
    publish_snapshot(snapshot);
}

void face_task(void *)
{
    face_recognition_snapshot_t snapshot = {};
    snapshot.state = FACE_RECOGNITION_STATE_STARTING;
    snapshot.last_error = ESP_OK;

    // ESP32-S31 is mapped by both managed components to their ESP32-P4 model assets.
    HumanFaceDetect detector(HumanFaceDetect::MSRMNP_S8_V1, false);
    HumanFaceFeat feature_model(HumanFaceFeat::MFN_S8_V1, false);

    const int feature_length = feature_model.get_feat_len();
    if (detector.get_raw_model(0) == nullptr || detector.get_raw_model(1) == nullptr ||
        feature_model.get_raw_model() == nullptr || feature_length <= 0 || feature_length > kMaxFeatureLength) {
        record_error(ESP_ERR_NOT_FOUND, true);
        atomic_store_u32(&s_service.frame_state, FRAME_FREE);
        publish_snapshot(snapshot);
        ESP_LOGE(TAG, "Face model initialization failed");
        for (;;) {
            vTaskSuspend(nullptr);
        }
    }

    const size_t database_bytes =
        static_cast<size_t>(FACE_RECOGNITION_MAX_IDENTITIES) * feature_length * sizeof(float);
    float *database = static_cast<float *>(
        heap_caps_aligned_calloc(16U, 1U, database_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    float *enrollment_sum = static_cast<float *>(heap_caps_aligned_calloc(
        16U, static_cast<size_t>(feature_length), sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (database == nullptr || enrollment_sum == nullptr) {
        heap_caps_free(database);
        heap_caps_free(enrollment_sum);
        record_error(ESP_ERR_NO_MEM, true);
        atomic_store_u32(&s_service.frame_state, FRAME_FREE);
        publish_snapshot(snapshot);
        ESP_LOGE(TAG, "Unable to allocate the volatile face database in PSRAM");
        for (;;) {
            vTaskSuspend(nullptr);
        }
    }

    uint8_t identity_count = 0U;
    uint8_t enroll_samples = 0U;
    bool enrolling = false;
    uint32_t active_enrollment_control =
        atomic_load_u32(&s_service.enrollment_control);
    atomic_store_i32(&s_service.last_error, ESP_OK);
    atomic_store_u32(&s_service.identity_count, 0U, __ATOMIC_RELAXED);
    atomic_store_u32(&s_service.model_ready, 1U);
    atomic_store_u32(&s_service.service_ready, 1U);
    atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_READY);
    publish_snapshot(snapshot);
    ESP_LOGI(TAG, "Face service ready: USB RGB565LE -> 320x240, feature length %d, core=%d",
             feature_length, xPortGetCoreID());

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        process_commands(snapshot,
                         database,
                         enrollment_sum,
                         feature_length,
                         identity_count,
                         enrolling,
                         enroll_samples);
        (void)apply_enrollment_intent(snapshot,
                                      enrollment_sum,
                                      feature_length,
                                      identity_count,
                                      active_enrollment_control,
                                      enrolling,
                                      enroll_samples);

        if (atomic_compare_exchange_u32(&s_service.frame_state, FRAME_READY, FRAME_READING)) {
            process_frame(snapshot,
                          detector,
                          feature_model,
                          database,
                          enrollment_sum,
                          feature_length,
                          identity_count,
                          active_enrollment_control,
                          enrolling,
                          enroll_samples);
            atomic_store_u32(&s_service.frame_state, FRAME_FREE);
        }
    }
}

bool queue_command(CommandType type)
{
    if (atomic_load_u32(&s_service.service_ready) == 0U || s_service.command_queue == nullptr) {
        return false;
    }
    const Command command{type};
    if (xQueueSend(s_service.command_queue, &command, 0) != pdTRUE) {
        atomic_increment(&s_service.command_drops);
        return false;
    }
    xTaskNotifyGive(s_service.task);
    return true;
}

} // namespace

extern "C" esp_err_t face_recognition_service_start(void)
{
    if (!atomic_compare_exchange_u32(&s_service.started, 0U, 1U)) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_STARTING);
    atomic_store_i32(&s_service.last_error, ESP_OK);
    s_service.command_queue = xQueueCreateStatic(kCommandQueueLength,
                                                  sizeof(Command),
                                                  s_service.command_storage,
                                                  &s_service.command_queue_control);
    if (s_service.command_queue == nullptr) {
        atomic_store_u32(&s_service.started, 0U);
        atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_ERROR);
        atomic_store_i32(&s_service.last_error, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    s_service.frame = static_cast<uint8_t *>(
        heap_caps_aligned_calloc(64U, 1U, kWorkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_service.frame == nullptr) {
        s_service.command_queue = nullptr;
        atomic_store_u32(&s_service.started, 0U);
        atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_ERROR);
        atomic_store_i32(&s_service.last_error, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(face_task,
                                                              "face_ai",
                                                              kTaskStackBytes,
                                                              nullptr,
                                                              kTaskPriority,
                                                              &s_service.task,
                                                              kTaskCore,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        heap_caps_free(s_service.frame);
        s_service.frame = nullptr;
        s_service.command_queue = nullptr;
        atomic_store_u32(&s_service.started, 0U);
        atomic_store_u32(&s_service.state, FACE_RECOGNITION_STATE_ERROR);
        atomic_store_i32(&s_service.last_error, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" bool face_recognition_service_submit_rgb565le(const uint8_t *data,
                                                            size_t data_size,
                                                            uint16_t width,
                                                           uint16_t height,
                                                           uint16_t stride,
                                                           uint32_t sequence)
{
    atomic_increment(&s_service.frames_submitted);
    const bool full_size = width == FACE_RECOGNITION_INPUT_WIDTH &&
                           height == FACE_RECOGNITION_INPUT_HEIGHT;
    const bool work_size = width == kWorkWidth && height == kWorkHeight;
    if (data == nullptr || (!full_size && !work_size) ||
        stride < static_cast<uint16_t>(width * 2U) ||
        data_size < (static_cast<size_t>(height - 1U) * stride + static_cast<size_t>(width) * 2U)) {
        atomic_increment(&s_service.invalid_frames);
        return false;
    }
    if (atomic_load_u32(&s_service.service_ready) == 0U || atomic_load_u32(&s_service.model_ready) == 0U) {
        atomic_increment(&s_service.frames_dropped_busy);
        return false;
    }

    const TickType_t now = xTaskGetTickCount();
    if (atomic_load_u32(&s_service.frames_accepted, __ATOMIC_RELAXED) != 0U &&
        elapsed_ms(now, atomic_load_u32(&s_service.last_accepted_tick, __ATOMIC_RELAXED)) < kAdmissionIntervalMs) {
        atomic_increment(&s_service.frames_dropped_throttled);
        return false;
    }
    if (!atomic_compare_exchange_u32(&s_service.frame_state, FRAME_FREE, FRAME_WRITING)) {
        atomic_increment(&s_service.frames_dropped_busy);
        return false;
    }

    const uint16_t source_step = full_size ? 2U : 1U;
    for (uint16_t destination_y = 0; destination_y < kWorkHeight; ++destination_y) {
        const uint8_t *source_row =
            data + static_cast<size_t>(destination_y * source_step) * stride;
        uint8_t *destination_row = s_service.frame + static_cast<size_t>(destination_y) * kWorkStride;
        for (uint16_t destination_x = 0; destination_x < kWorkWidth; ++destination_x) {
            const uint8_t *source_pixel = source_row +
                static_cast<size_t>(destination_x * source_step) * 2U;
            destination_row[static_cast<size_t>(destination_x) * 2U] = source_pixel[0];
            destination_row[static_cast<size_t>(destination_x) * 2U + 1U] = source_pixel[1];
        }
    }

    atomic_store_u32(&s_service.frame_sequence, sequence, __ATOMIC_RELAXED);
    atomic_store_u32(&s_service.last_accepted_tick, static_cast<uint32_t>(now), __ATOMIC_RELAXED);
    atomic_increment(&s_service.frames_accepted);
    atomic_store_u32(&s_service.frame_state, FRAME_READY);
    xTaskNotifyGive(s_service.task);
    return true;
}

extern "C" bool face_recognition_service_request_enroll(void)
{
    if (atomic_load_u32(&s_service.service_ready) == 0U ||
            atomic_load_u32(&s_service.model_ready) == 0U ||
            s_service.task == nullptr) {
        return false;
    }

    uint32_t current = atomic_load_u32(&s_service.enrollment_control);
    for (;;) {
        if ((current & kEnrollmentDesiredMask) != 0U ||
                atomic_load_u32(&s_service.identity_count, __ATOMIC_RELAXED) >=
                    FACE_RECOGNITION_MAX_IDENTITIES) {
            return false;
        }
        const uint32_t next = enrollment_control_next(current, true);
        if (__atomic_compare_exchange_n(&s_service.enrollment_control,
                                        &current,
                                        next,
                                        false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            xTaskNotifyGive(s_service.task);
            return true;
        }
    }
}

extern "C" bool face_recognition_service_request_cancel_enroll(void)
{
    if (atomic_load_u32(&s_service.started) == 0U || s_service.task == nullptr) {
        return false;
    }
    (void)set_enrollment_intent(false);
    xTaskNotifyGive(s_service.task);
    return true;
}

extern "C" bool face_recognition_service_request_delete_last(void)
{
    (void)face_recognition_service_request_cancel_enroll();
    return queue_command(CommandType::DELETE_LAST);
}

extern "C" bool face_recognition_service_request_clear(void)
{
    (void)face_recognition_service_request_cancel_enroll();
    return queue_command(CommandType::CLEAR);
}

extern "C" bool face_recognition_service_get_snapshot(face_recognition_snapshot_t *out_snapshot)
{
    if (out_snapshot == nullptr || atomic_load_u32(&s_service.started) == 0U) {
        return false;
    }

    portENTER_CRITICAL(&s_snapshot_lock);
    *out_snapshot = s_snapshot;
    const uint32_t snapshot_tick = atomic_load_u32(&s_service.snapshot_tick, __ATOMIC_RELAXED);
    portEXIT_CRITICAL(&s_snapshot_lock);

    out_snapshot->state = static_cast<face_recognition_state_t>(atomic_load_u32(&s_service.state));
    out_snapshot->last_error = static_cast<esp_err_t>(atomic_load_i32(&s_service.last_error));
    out_snapshot->service_ready = atomic_load_u32(&s_service.service_ready) != 0U;
    out_snapshot->model_ready = atomic_load_u32(&s_service.model_ready) != 0U;
    out_snapshot->age_ms = elapsed_ms(xTaskGetTickCount(), snapshot_tick);
    out_snapshot->inference_max_ms = atomic_load_u32(&s_service.inference_max_ms, __ATOMIC_RELAXED);
    out_snapshot->task_stack_min_free_bytes =
        atomic_load_u32(&s_service.task_stack_min_free_bytes, __ATOMIC_RELAXED);
    out_snapshot->frames_submitted = atomic_load_u32(&s_service.frames_submitted, __ATOMIC_RELAXED);
    out_snapshot->frames_accepted = atomic_load_u32(&s_service.frames_accepted, __ATOMIC_RELAXED);
    out_snapshot->frames_dropped_busy = atomic_load_u32(&s_service.frames_dropped_busy, __ATOMIC_RELAXED);
    out_snapshot->frames_dropped_throttled =
        atomic_load_u32(&s_service.frames_dropped_throttled, __ATOMIC_RELAXED);
    out_snapshot->invalid_frames = atomic_load_u32(&s_service.invalid_frames, __ATOMIC_RELAXED);
    out_snapshot->inference_count = atomic_load_u32(&s_service.inference_count, __ATOMIC_RELAXED);
    out_snapshot->faces_detected = atomic_load_u32(&s_service.faces_detected, __ATOMIC_RELAXED);
    out_snapshot->recognition_count = atomic_load_u32(&s_service.recognition_count, __ATOMIC_RELAXED);
    out_snapshot->enroll_count = atomic_load_u32(&s_service.enroll_count, __ATOMIC_RELAXED);
    out_snapshot->command_drops = atomic_load_u32(&s_service.command_drops, __ATOMIC_RELAXED);
    out_snapshot->error_count = atomic_load_u32(&s_service.error_count, __ATOMIC_RELAXED);
    return true;
}
