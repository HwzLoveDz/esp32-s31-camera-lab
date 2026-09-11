/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * Modified for the ESP32-S31 Camera Lab project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "onboard_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "dev_camera.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/v4l2-controls.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"

#define ONBOARD_CAMERA_WIDTH                    640U
#define ONBOARD_CAMERA_HEIGHT                   480U
#define ONBOARD_CAMERA_BYTES_PER_PIXEL          2U
#define ONBOARD_CAMERA_MMAP_BUFFER_COUNT        1U
#define ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT      2U
#define ONBOARD_CAMERA_TASK_STACK_SIZE          (6U * 1024U)
#define ONBOARD_CAMERA_TASK_PRIORITY            4U
#define ONBOARD_CAMERA_DQBUF_TIMEOUT_MS          500U
#define ONBOARD_CAMERA_FPS_WINDOW_US            1000000LL
#define ONBOARD_CAMERA_COPY_WARN_US              20000U
#define ONBOARD_CAMERA_OUTPUT_ALIGNMENT          64U
#define ONBOARD_CAMERA_STREAMOFF_RETRIES          3U

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define ONBOARD_CAMERA_TASK_CORE                0
#else
#define ONBOARD_CAMERA_TASK_CORE                tskNO_AFFINITY
#endif

typedef enum {
    OUTPUT_SLOT_FREE = 0,
    OUTPUT_SLOT_WRITING,
    OUTPUT_SLOT_READY,
    OUTPUT_SLOT_ACQUIRED,
    OUTPUT_SLOT_DISPLAYED,
} output_slot_state_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t data_size;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint32_t sequence;
    uint32_t generation;
    output_slot_state_t state;
} output_slot_t;

typedef struct {
    portMUX_TYPE output_lock;
    TaskHandle_t task;
    output_slot_t output[ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT];
    bool starting;
    bool started;
    bool service_ready;
    bool streaming;
    uint32_t state;
    int32_t last_error;
    uint32_t fps_milli;
    uint32_t width;
    uint32_t height;
    uint32_t next_sequence;
    uint32_t frames_captured;
    uint32_t display_drops;
    uint32_t capture_errors;
    uint32_t copy_time_max_us;
    uint32_t task_stack_min_free_bytes;
} onboard_camera_context_t;

static const char *TAG = "onboard_camera";
static onboard_camera_context_t s_camera = {
    .output_lock = portMUX_INITIALIZER_UNLOCKED,
    .state = ONBOARD_CAMERA_STATE_STOPPED,
    .last_error = ESP_OK,
};

static inline uint32_t camera_atomic_load_u32(const uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static inline void camera_atomic_store_u32(uint32_t *value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

static inline uint32_t camera_counter_add(uint32_t *counter)
{
    return __atomic_add_fetch(counter, 1U, __ATOMIC_RELAXED);
}

static inline void camera_set_error(esp_err_t error)
{
    __atomic_store_n(&s_camera.last_error, (int32_t)error, __ATOMIC_RELAXED);
}

static inline bool camera_should_log_count(uint32_t count)
{
    return count == 1U || (count & (count - 1U)) == 0U;
}

static void camera_update_max_u32(uint32_t *value, uint32_t candidate)
{
    uint32_t current = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (candidate > current &&
           !__atomic_compare_exchange_n(value, &current, candidate, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void camera_free_output_buffers(void)
{
    uint8_t *released[ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT] = {0};
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        released[i] = s_camera.output[i].data;
        memset(&s_camera.output[i], 0, sizeof(s_camera.output[i]));
        s_camera.output[i].capacity = 0;
        s_camera.output[i].state = OUTPUT_SLOT_FREE;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        heap_caps_free(released[i]);
    }
}

static esp_err_t camera_allocate_output_buffers(size_t capacity)
{
    uint8_t *allocated[ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT] = {0};
    if (capacity == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        allocated[i] = heap_caps_aligned_calloc(
            ONBOARD_CAMERA_OUTPUT_ALIGNMENT, 1, capacity,
            MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (allocated[i] == NULL) {
            for (size_t j = 0; j < i; ++j) {
                heap_caps_free(allocated[j]);
            }
            return ESP_ERR_NO_MEM;
        }
    }
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        memset(&s_camera.output[i], 0, sizeof(s_camera.output[i]));
        s_camera.output[i].data = allocated[i];
        s_camera.output[i].capacity = capacity;
        s_camera.output[i].state = OUTPUT_SLOT_FREE;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
    return ESP_OK;
}

static bool camera_reserve_output_slot(uint8_t *out_slot, uint32_t *out_generation)
{
    int selected = -1;
    uint32_t oldest_sequence = UINT32_MAX;
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        if (s_camera.output[i].state == OUTPUT_SLOT_FREE) {
            selected = (int)i;
            break;
        }
        if (s_camera.output[i].state == OUTPUT_SLOT_READY &&
            s_camera.output[i].sequence < oldest_sequence) {
            selected = (int)i;
            oldest_sequence = s_camera.output[i].sequence;
        }
    }
    if (selected >= 0) {
        output_slot_t *slot = &s_camera.output[selected];
        if (slot->state == OUTPUT_SLOT_READY) {
            camera_counter_add(&s_camera.display_drops);
        }
        slot->state = OUTPUT_SLOT_WRITING;
        slot->generation++;
        *out_slot = (uint8_t)selected;
        *out_generation = slot->generation;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
    return selected >= 0;
}

static void camera_abandon_output_slot(uint8_t slot_index, uint32_t generation)
{
    portENTER_CRITICAL(&s_camera.output_lock);
    if (slot_index < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT &&
        s_camera.output[slot_index].state == OUTPUT_SLOT_WRITING &&
        s_camera.output[slot_index].generation == generation) {
        s_camera.output[slot_index].state = OUTPUT_SLOT_FREE;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
}

static uint32_t camera_publish_output_slot(uint8_t slot_index, uint32_t generation,
                                           uint16_t width, uint16_t height,
                                           uint16_t stride, size_t data_size)
{
    uint32_t sequence = 0;
    portENTER_CRITICAL(&s_camera.output_lock);
    if (slot_index < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT &&
        s_camera.output[slot_index].state == OUTPUT_SLOT_WRITING &&
        s_camera.output[slot_index].generation == generation) {
        for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
            if (i != slot_index && s_camera.output[i].state == OUTPUT_SLOT_READY) {
                s_camera.output[i].state = OUTPUT_SLOT_FREE;
                camera_counter_add(&s_camera.display_drops);
            }
        }
        output_slot_t *slot = &s_camera.output[slot_index];
        slot->width = width;
        slot->height = height;
        slot->stride = stride;
        slot->data_size = data_size;
        slot->sequence = ++s_camera.next_sequence;
        sequence = slot->sequence;
        slot->state = OUTPUT_SLOT_READY;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
    return sequence;
}

static void camera_set_terminal_state(esp_err_t error)
{
    camera_set_error(error);
    __atomic_store_n(&s_camera.streaming, false, __ATOMIC_RELEASE);
    camera_atomic_store_u32(&s_camera.state, ONBOARD_CAMERA_STATE_ERROR);
}

static int camera_release_mmap_buffers(int fd, void **mapped, size_t *mapped_length,
                                       uint32_t count)
{
    if (mapped != NULL && mapped_length != NULL) {
        for (uint32_t i = 0; i < count; ++i) {
            if (mapped[i] != NULL && mapped[i] != MAP_FAILED && mapped_length[i] > 0U) {
                (void)munmap(mapped[i], mapped_length[i]);
                mapped[i] = NULL;
                mapped_length[i] = 0U;
            }
        }
    }
    if (fd >= 0) {
        struct v4l2_requestbuffers release = {
            .count = 0,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        return ioctl(fd, VIDIOC_REQBUFS, &release);
    }
    return 0;
}

static void camera_capture_task(void *arg)
{
    (void)arg;
    dev_camera_handle_t *camera_handle = NULL;
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    struct v4l2_requestbuffers request = {
        .count = ONBOARD_CAMERA_MMAP_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    void *mapped[ONBOARD_CAMERA_MMAP_BUFFER_COUNT] = {0};
    size_t mapped_length[ONBOARD_CAMERA_MMAP_BUFFER_COUNT] = {0};
    uint32_t mapped_count = 0;
    int fd = -1;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool stream_on = false;
    bool output_allocated = false;
    bool published_any_frame = false;
    esp_err_t ret = ESP_OK;

    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_CAMERA,
                                               (void **)&camera_handle);
    if (ret != ESP_OK || camera_handle == NULL || camera_handle->dev_path == NULL) {
        ret = ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
        ESP_LOGE(TAG, "Board camera handle unavailable: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    fd = open(camera_handle->dev_path, O_RDONLY);
    if (fd < 0) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "Open %s failed, errno=%d", camera_handle->dev_path, errno);
        goto cleanup;
    }

    struct v4l2_capability capability = {0};
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed, errno=%d", errno);
        goto cleanup;
    }
    uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
                    capability.device_caps : capability.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0U || (caps & V4L2_CAP_STREAMING) == 0U) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGE(TAG, "Board camera lacks capture/streaming capability");
        goto cleanup;
    }

    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0 ||
        ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "Get/set board camera format failed, errno=%d", errno);
        goto cleanup;
    }

    struct v4l2_ext_control orientation_controls[2] = {
        {
            .id = V4L2_CID_VFLIP,
            .value = 1,
        },
        {
            .id = V4L2_CID_HFLIP,
            .value = 1,
        },
    };
    struct v4l2_ext_controls orientation = {
        .ctrl_class = V4L2_CTRL_CLASS_USER,
        .count = 2,
        .controls = orientation_controls,
    };
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &orientation) != 0) {
        ESP_LOGW(TAG, "Board-camera 180-degree sensor correction failed, errno=%d", errno);
    } else {
        ESP_LOGI(TAG, "Board-camera 180-degree sensor correction applied (VFLIP+HFLIP)");
    }

    const uint32_t width = format.fmt.pix.width;
    const uint32_t height = format.fmt.pix.height;
    const uint32_t stride = format.fmt.pix.bytesperline != 0U ?
                            format.fmt.pix.bytesperline : width * ONBOARD_CAMERA_BYTES_PER_PIXEL;
    const size_t visible_size = (size_t)stride * height;
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565X ||
        width != ONBOARD_CAMERA_WIDTH || height != ONBOARD_CAMERA_HEIGHT ||
        stride < width * ONBOARD_CAMERA_BYTES_PER_PIXEL ||
        stride > UINT16_MAX || visible_size == 0U) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGE(TAG, "Expected RGB565X 640x480, got fourcc=%08" PRIx32
                      " size=%" PRIu32 "x%" PRIu32 " stride=%" PRIu32,
                 format.fmt.pix.pixelformat, width, height, stride);
        goto cleanup;
    }

    ret = camera_allocate_output_buffers(visible_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Allocate RGB565 lease buffers failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    output_allocated = true;

    struct timeval timeout = {
        .tv_sec = ONBOARD_CAMERA_DQBUF_TIMEOUT_MS / 1000U,
        .tv_usec = (ONBOARD_CAMERA_DQBUF_TIMEOUT_MS % 1000U) * 1000U,
    };
    if (ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "Set dequeue timeout failed, errno=%d", errno);
        goto cleanup;
    }
    if (ioctl(fd, VIDIOC_REQBUFS, &request) != 0 ||
        request.count < ONBOARD_CAMERA_MMAP_BUFFER_COUNT) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Request camera MMAP buffer failed, count=%" PRIu32 " errno=%d",
                 request.count, errno);
        goto cleanup;
    }
    mapped_count = request.count < ONBOARD_CAMERA_MMAP_BUFFER_COUNT ?
                   request.count : ONBOARD_CAMERA_MMAP_BUFFER_COUNT;

    for (uint32_t i = 0; i < mapped_count; ++i) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0 || buffer.length < visible_size) {
            ret = ESP_ERR_INVALID_SIZE;
            ESP_LOGE(TAG, "Query camera buffer %" PRIu32 " failed/short: len=%" PRIu32,
                     i, buffer.length);
            goto cleanup;
        }
        mapped[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, buffer.m.offset);
        mapped_length[i] = buffer.length;
        if (mapped[i] == NULL || mapped[i] == MAP_FAILED) {
            mapped[i] = NULL;
            mapped_length[i] = 0U;
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Map camera buffer %" PRIu32 " failed", i);
            goto cleanup;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Queue camera buffer %" PRIu32 " failed, errno=%d", i, errno);
            goto cleanup;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "Start board camera stream failed, errno=%d", errno);
        goto cleanup;
    }
    stream_on = true;
    __atomic_store_n(&s_camera.service_ready, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_camera.streaming, true, __ATOMIC_RELEASE);
    camera_atomic_store_u32(&s_camera.width, width);
    camera_atomic_store_u32(&s_camera.height, height);
    camera_set_error(ESP_OK);
    ESP_LOGI(TAG, "Board camera streaming %" PRIu32 "x%" PRIu32
                  " RGB565_BE stride=%" PRIu32 ", MMAP=%u, lease=%u, core=%d",
             width, height, stride, (unsigned)ONBOARD_CAMERA_MMAP_BUFFER_COUNT,
             (unsigned)ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT, ONBOARD_CAMERA_TASK_CORE);

    int64_t fps_window_start = esp_timer_get_time();
    uint32_t fps_window_frames = 0;
    uint32_t dequeue_error_streak = 0;
    uint32_t invalid_frame_streak = 0;
    uint32_t slow_copy_count = 0;
    while (true) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(fd, VIDIOC_DQBUF, &buffer) != 0) {
            const int dequeue_errno = errno;
            uint32_t errors = camera_counter_add(&s_camera.capture_errors);
            dequeue_error_streak++;
            const bool transient = dequeue_errno == ETIMEDOUT || dequeue_errno == EINTR ||
                                   dequeue_errno == EAGAIN;
            camera_set_error(dequeue_errno == ETIMEDOUT ? ESP_ERR_TIMEOUT : ESP_FAIL);
            if (camera_should_log_count(errors)) {
                ESP_LOGW(TAG, "Board camera dequeue failed/timeout (%" PRIu32 "), errno=%d",
                         errors, dequeue_errno);
            }
            if (transient && dequeue_error_streak < 8U) {
                continue;
            }
            ret = dequeue_errno == ETIMEDOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        dequeue_error_streak = 0;

        bool valid = buffer.index < mapped_count && mapped[buffer.index] != NULL &&
                     (buffer.flags & V4L2_BUF_FLAG_DONE) != 0U &&
                     (buffer.flags & V4L2_BUF_FLAG_ERROR) == 0U &&
                     buffer.bytesused <= mapped_length[buffer.index] &&
                     buffer.bytesused >= visible_size;
        if (valid) {
            invalid_frame_streak = 0;
            uint8_t slot_index = 0;
            uint32_t generation = 0;
            if (camera_reserve_output_slot(&slot_index, &generation)) {
                output_slot_t *slot = &s_camera.output[slot_index];
                if (slot->data != NULL && slot->capacity >= visible_size) {
                    int64_t copy_start = esp_timer_get_time();
                    memcpy(slot->data, mapped[buffer.index], visible_size);
                    uint32_t copy_us = (uint32_t)(esp_timer_get_time() - copy_start);
                    camera_update_max_u32(&s_camera.copy_time_max_us, copy_us);
                    if (copy_us > ONBOARD_CAMERA_COPY_WARN_US) {
                        ++slow_copy_count;
                        if (camera_should_log_count(slow_copy_count)) {
                            ESP_LOGW(TAG, "Slow board-camera copy (%" PRIu32 "): %" PRIu32 " us",
                                     slow_copy_count, copy_us);
                        }
                    }
                    uint32_t frame_sequence = camera_publish_output_slot(
                        slot_index, generation, (uint16_t)width, (uint16_t)height,
                        (uint16_t)stride, visible_size);
                    if (frame_sequence != 0U) {
                        camera_counter_add(&s_camera.frames_captured);
                        published_any_frame = true;
                        fps_window_frames++;
                        camera_atomic_store_u32(&s_camera.state, ONBOARD_CAMERA_STATE_STREAMING);
                        camera_set_error(ESP_OK);
                    }
                } else {
                    camera_abandon_output_slot(slot_index, generation);
                    camera_counter_add(&s_camera.capture_errors);
                }
            } else {
                camera_counter_add(&s_camera.display_drops);
            }
        } else {
            uint32_t errors = camera_counter_add(&s_camera.capture_errors);
            invalid_frame_streak++;
            if (camera_should_log_count(errors)) {
                ESP_LOGW(TAG, "Invalid board camera frame index=%" PRIu32
                              " bytes=%" PRIu32 " flags=0x%" PRIx32,
                         buffer.index, buffer.bytesused, buffer.flags);
            }
        }

        if (buffer.index >= mapped_count || ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Requeue board camera frame failed, index=%" PRIu32 " errno=%d",
                     buffer.index, errno);
            break;
        }
        if (invalid_frame_streak >= 8U) {
            ret = ESP_ERR_INVALID_RESPONSE;
            ESP_LOGE(TAG, "Too many invalid board camera frames");
            break;
        }

        int64_t now = esp_timer_get_time();
        int64_t elapsed = now - fps_window_start;
        if (elapsed >= ONBOARD_CAMERA_FPS_WINDOW_US) {
            uint32_t fps_milli = elapsed > 0 ?
                (uint32_t)(((uint64_t)fps_window_frames * 1000000000ULL) / (uint64_t)elapsed) : 0U;
            camera_atomic_store_u32(&s_camera.fps_milli, fps_milli);
            fps_window_start = now;
            fps_window_frames = 0;
            camera_atomic_store_u32(&s_camera.task_stack_min_free_bytes,
                                    (uint32_t)uxTaskGetStackHighWaterMark(NULL));
        }
    }

cleanup:
    if (stream_on) {
        int stop_errno = 0;
        for (uint32_t attempt = 0; attempt < ONBOARD_CAMERA_STREAMOFF_RETRIES; ++attempt) {
            if (ioctl(fd, VIDIOC_STREAMOFF, &type) == 0) {
                stream_on = false;
                break;
            }
            stop_errno = errno;
            vTaskDelay(pdMS_TO_TICKS(20U << attempt));
        }
        if (stream_on) {
            if (ret == ESP_OK) {
                ret = ESP_FAIL;
            }
            camera_set_terminal_state(ret);
            __atomic_store_n(&s_camera.service_ready, false, __ATOMIC_RELEASE);
            camera_atomic_store_u32(&s_camera.task_stack_min_free_bytes,
                                    (uint32_t)uxTaskGetStackHighWaterMark(NULL));
            ESP_LOGE(TAG,
                     "VIDIOC_STREAMOFF failed after %u attempts (errno=%d); "
                     "camera DMA resources retained until reboot",
                     ONBOARD_CAMERA_STREAMOFF_RETRIES, stop_errno);
            for (;;) {
                vTaskSuspend(NULL);
            }
        }
    }
    if (camera_release_mmap_buffers(fd, mapped, mapped_length, mapped_count) != 0) {
        ESP_LOGW(TAG, "VIDIOC_REQBUFS(0) failed after stream stop (errno=%d)", errno);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    camera_set_terminal_state(ret);
    __atomic_store_n(&s_camera.service_ready, false, __ATOMIC_RELEASE);
    camera_atomic_store_u32(&s_camera.task_stack_min_free_bytes,
                            (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    /* Published lease buffers remain allocated on a runtime failure because
     * LVGL may still reference the last frame. Setup failures have no leases. */
    if (output_allocated && !published_any_frame) {
        camera_free_output_buffers();
    }
    s_camera.task = NULL;
    ESP_LOGE(TAG, "Board camera service stopped: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
}

esp_err_t onboard_camera_start(void)
{
    if (__atomic_exchange_n(&s_camera.starting, true, __ATOMIC_ACQ_REL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (__atomic_load_n(&s_camera.started, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ESP_ERR_INVALID_STATE;
    }

    camera_atomic_store_u32(&s_camera.state, ONBOARD_CAMERA_STATE_STARTING);
    camera_set_error(ESP_OK);
    if (xTaskCreatePinnedToCore(camera_capture_task, "board_camera",
                                ONBOARD_CAMERA_TASK_STACK_SIZE, NULL,
                                ONBOARD_CAMERA_TASK_PRIORITY, &s_camera.task,
                                ONBOARD_CAMERA_TASK_CORE) != pdPASS) {
        s_camera.task = NULL;
        camera_set_terminal_state(ESP_ERR_NO_MEM);
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }

    __atomic_store_n(&s_camera.started, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
    return ESP_OK;
}

bool onboard_camera_get_stats(onboard_camera_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return false;
    }
    bool frame_available = false;
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        if (s_camera.output[i].state == OUTPUT_SLOT_READY) {
            frame_available = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_camera.output_lock);

    *out_stats = (onboard_camera_stats_t) {
        .state = (onboard_camera_state_t)camera_atomic_load_u32(&s_camera.state),
        .last_error = (esp_err_t)__atomic_load_n(&s_camera.last_error, __ATOMIC_RELAXED),
        .fps = camera_atomic_load_u32(&s_camera.fps_milli) / 1000.0f,
        .width = (uint16_t)camera_atomic_load_u32(&s_camera.width),
        .height = (uint16_t)camera_atomic_load_u32(&s_camera.height),
        .frames_captured = camera_atomic_load_u32(&s_camera.frames_captured),
        .display_drops = camera_atomic_load_u32(&s_camera.display_drops),
        .capture_errors = camera_atomic_load_u32(&s_camera.capture_errors),
        .copy_time_max_us = camera_atomic_load_u32(&s_camera.copy_time_max_us),
        .task_stack_min_free_bytes = camera_atomic_load_u32(&s_camera.task_stack_min_free_bytes),
        .service_ready = __atomic_load_n(&s_camera.service_ready, __ATOMIC_ACQUIRE),
        .streaming = __atomic_load_n(&s_camera.streaming, __ATOMIC_ACQUIRE),
        .frame_available = frame_available,
    };
    return __atomic_load_n(&s_camera.started, __ATOMIC_ACQUIRE);
}

bool onboard_camera_acquire_latest_frame(onboard_camera_frame_t *out_frame)
{
    if (out_frame == NULL) {
        return false;
    }
    int selected = -1;
    uint32_t newest_sequence = 0;
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        if (s_camera.output[i].state == OUTPUT_SLOT_ACQUIRED) {
            portEXIT_CRITICAL(&s_camera.output_lock);
            return false;
        }
        if (s_camera.output[i].state == OUTPUT_SLOT_READY &&
            (selected < 0 || s_camera.output[i].sequence > newest_sequence)) {
            selected = (int)i;
            newest_sequence = s_camera.output[i].sequence;
        }
    }
    if (selected >= 0) {
        output_slot_t *slot = &s_camera.output[selected];
        slot->state = OUTPUT_SLOT_ACQUIRED;
        *out_frame = (onboard_camera_frame_t) {
            .data = slot->data,
            .data_size = slot->data_size,
            .width = slot->width,
            .height = slot->height,
            .stride = slot->stride,
            .sequence = slot->sequence,
            .byte_swapped = true,
            ._slot = (uint8_t)selected,
            ._generation = slot->generation,
        };
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
    return selected >= 0;
}

void onboard_camera_present_frame(const onboard_camera_frame_t *frame)
{
    if (frame == NULL || frame->_slot >= ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_camera.output_lock);
    output_slot_t *slot = &s_camera.output[frame->_slot];
    if (slot->state == OUTPUT_SLOT_ACQUIRED && slot->generation == frame->_generation) {
        for (size_t i = 0; i < ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
            if (i != frame->_slot && s_camera.output[i].state == OUTPUT_SLOT_DISPLAYED) {
                s_camera.output[i].state = OUTPUT_SLOT_FREE;
            }
        }
        slot->state = OUTPUT_SLOT_DISPLAYED;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
}

void onboard_camera_release_frame(const onboard_camera_frame_t *frame)
{
    if (frame == NULL || frame->_slot >= ONBOARD_CAMERA_OUTPUT_BUFFER_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_camera.output_lock);
    output_slot_t *slot = &s_camera.output[frame->_slot];
    if (slot->generation == frame->_generation &&
        (slot->state == OUTPUT_SLOT_ACQUIRED || slot->state == OUTPUT_SLOT_DISPLAYED)) {
        slot->state = OUTPUT_SLOT_FREE;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
}
