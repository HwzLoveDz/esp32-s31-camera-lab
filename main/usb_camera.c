/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * Modified for the ESP32-S31 Camera Lab project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_camera.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "face_recognition_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/uvc_host.h"

#define USB_CAMERA_MAX_WIDTH                 640U
#define USB_CAMERA_MAX_HEIGHT                480U
#define USB_CAMERA_YUY2_MAX_WIDTH            320U
#define USB_CAMERA_YUY2_MAX_HEIGHT           240U
#define USB_CAMERA_RGB565_BYTES_PER_PIXEL    2U
#define USB_CAMERA_INPUT_FRAME_SIZE          (USB_CAMERA_MAX_WIDTH * USB_CAMERA_MAX_HEIGHT * 2U)
#define USB_CAMERA_OUTPUT_FRAME_SIZE         (USB_CAMERA_MAX_WIDTH * USB_CAMERA_MAX_HEIGHT * USB_CAMERA_RGB565_BYTES_PER_PIXEL)
#define USB_CAMERA_INPUT_BUFFER_COUNT        3U
#define USB_CAMERA_OUTPUT_BUFFER_COUNT       2U
#define USB_CAMERA_DEVICE_QUEUE_LENGTH       6U
#define USB_CAMERA_FRAME_QUEUE_LENGTH        USB_CAMERA_INPUT_BUFFER_COUNT
#define USB_CAMERA_TARGET_INTERVAL_100NS     666667U /* about 15 fps */
#define USB_CAMERA_UVC_TASK_STACK_SIZE       (6U * 1024U)
#define USB_CAMERA_UVC_TASK_PRIORITY         8U
#define USB_CAMERA_MANAGER_STACK_SIZE        (6U * 1024U)
#define USB_CAMERA_MANAGER_PRIORITY          7U
#define USB_CAMERA_DECODE_STACK_SIZE         (8U * 1024U)
#define USB_CAMERA_DECODE_PRIORITY           5U
#define USB_CAMERA_OPEN_TIMEOUT_MS           1500U
#define USB_CAMERA_FRAME_WATCHDOG_MS         3000U
#define USB_CAMERA_FRAME_RETURN_TIMEOUT_MS   3000U
#define USB_CAMERA_CLOSE_RETRY_COUNT          50U
#define USB_CAMERA_DECODE_ERROR_LIMIT        8U

#define USB_CAMERA_NOTIFY_DISCONNECTED       (1UL << 0)
#define USB_CAMERA_NOTIFY_RETRY_FORMAT       (1UL << 1)

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define USB_CAMERA_TASK_CORE                 1
#else
#define USB_CAMERA_TASK_CORE                 tskNO_AFFINITY
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
    uint8_t dev_addr;
    uint8_t stream_index;
    size_t frame_info_count;
} camera_device_event_t;

typedef struct {
    uvc_host_stream_hdl_t stream;
    uvc_host_frame_t *frame;
} camera_input_frame_t;

typedef struct {
    enum uvc_host_stream_format format;
    uint16_t width;
    uint16_t height;
    uint32_t interval_100ns;
    float fps;
} camera_candidate_t;

typedef struct {
    portMUX_TYPE output_lock;
    QueueHandle_t device_queue;
    QueueHandle_t frame_queue;
    TaskHandle_t manager_task;
    TaskHandle_t decode_task;
    jpeg_decoder_handle_t jpeg_decoder;
    uvc_host_stream_hdl_t stream;
    uint8_t *input_buffers[USB_CAMERA_INPUT_BUFFER_COUNT];
    size_t input_capacity;
    output_slot_t output[USB_CAMERA_OUTPUT_BUFFER_COUNT];

    bool starting;
    bool started;
    bool service_ready;
    bool device_connected;
    bool streaming;
    bool fatal_error;
    uint32_t state;
    uint32_t source_format;
    int32_t last_error;
    uint32_t fps_milli;
    uint32_t width;
    uint32_t height;
    uint32_t next_sequence;
    uint32_t outstanding_frames;
    uint32_t decode_error_streak;

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
} usb_camera_context_t;

static const char *TAG = "usb_camera";
static usb_camera_context_t s_camera = {
    .output_lock = portMUX_INITIALIZER_UNLOCKED,
    .state = USB_CAMERA_STATE_STOPPED,
    .source_format = USB_CAMERA_FORMAT_NONE,
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

static void camera_enter_fatal(esp_err_t error)
{
    camera_set_error(error);
    __atomic_store_n(&s_camera.fatal_error, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_camera.streaming, false, __ATOMIC_RELAXED);
    __atomic_store_n(&s_camera.device_connected, false, __ATOMIC_RELAXED);
    __atomic_store_n(&s_camera.service_ready, false, __ATOMIC_RELEASE);
    (void)face_recognition_service_request_cancel_enroll();
    camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_ERROR);
}

static const char *camera_format_name(enum uvc_host_stream_format format)
{
    switch (format) {
        case UVC_VS_FORMAT_MJPEG:
            return "MJPEG";
        case UVC_VS_FORMAT_YUY2:
            return "YUY2";
        default:
            return "unsupported";
    }
}

static usb_camera_format_t camera_public_format(enum uvc_host_stream_format format)
{
    switch (format) {
        case UVC_VS_FORMAT_MJPEG:
            return USB_CAMERA_FORMAT_MJPEG;
        case UVC_VS_FORMAT_YUY2:
            return USB_CAMERA_FORMAT_YUY2;
        default:
            return USB_CAMERA_FORMAT_NONE;
    }
}

static uint32_t camera_closest_interval(const uvc_host_frame_info_t *info)
{
    uint32_t best = info->default_interval;
    uint32_t best_distance = best > USB_CAMERA_TARGET_INTERVAL_100NS ?
                             best - USB_CAMERA_TARGET_INTERVAL_100NS :
                             USB_CAMERA_TARGET_INTERVAL_100NS - best;

    if (info->interval_type == 0U) {
        const uint32_t minimum = info->interval_min;
        const uint32_t maximum = info->interval_max;
        if (minimum != 0U && maximum >= minimum) {
            uint32_t selected = USB_CAMERA_TARGET_INTERVAL_100NS;
            if (selected < minimum) {
                selected = minimum;
            } else if (selected > maximum) {
                selected = maximum;
            }
            if (info->interval_step != 0U && selected > minimum) {
                uint32_t steps = (selected - minimum + info->interval_step / 2U) / info->interval_step;
                uint64_t stepped = (uint64_t)minimum + (uint64_t)steps * info->interval_step;
                selected = stepped > maximum ? maximum : (uint32_t)stepped;
            }
            best = selected;
        }
    } else {
        size_t interval_count = info->interval_type;
        if (interval_count > CONFIG_UVC_INTERVAL_ARRAY_SIZE) {
            interval_count = CONFIG_UVC_INTERVAL_ARRAY_SIZE;
        }
        for (size_t i = 0; i < interval_count; ++i) {
            const uint32_t candidate = info->interval[i];
            if (candidate == 0U) {
                continue;
            }
            const uint32_t distance = candidate > USB_CAMERA_TARGET_INTERVAL_100NS ?
                                      candidate - USB_CAMERA_TARGET_INTERVAL_100NS :
                                      USB_CAMERA_TARGET_INTERVAL_100NS - candidate;
            if (best == 0U || distance < best_distance) {
                best = candidate;
                best_distance = distance;
            }
        }
    }
    return best;
}

static int camera_candidate_compare(const void *left_ptr, const void *right_ptr)
{
    const camera_candidate_t *left = left_ptr;
    const camera_candidate_t *right = right_ptr;
    if (left->format != right->format) {
        return left->format == UVC_VS_FORMAT_MJPEG ? -1 : 1;
    }
    const uint32_t left_area = (uint32_t)left->width * left->height;
    const uint32_t right_area = (uint32_t)right->width * right->height;
    if (left_area != right_area) {
        return left_area > right_area ? -1 : 1;
    }
    const uint32_t left_distance = left->interval_100ns > USB_CAMERA_TARGET_INTERVAL_100NS ?
                                   left->interval_100ns - USB_CAMERA_TARGET_INTERVAL_100NS :
                                   USB_CAMERA_TARGET_INTERVAL_100NS - left->interval_100ns;
    const uint32_t right_distance = right->interval_100ns > USB_CAMERA_TARGET_INTERVAL_100NS ?
                                    right->interval_100ns - USB_CAMERA_TARGET_INTERVAL_100NS :
                                    USB_CAMERA_TARGET_INTERVAL_100NS - right->interval_100ns;
    if (left_distance != right_distance) {
        return left_distance < right_distance ? -1 : 1;
    }
    return 0;
}

static camera_candidate_t *camera_build_candidates(const camera_device_event_t *device,
                                                    size_t *out_count,
                                                    esp_err_t *out_error)
{
    *out_count = 0;
    size_t info_count = device->frame_info_count;
    if (info_count == 0U) {
        esp_err_t ret = uvc_host_get_frame_list(device->dev_addr, device->stream_index, NULL, &info_count);
        if (ret != ESP_OK || info_count == 0U) {
            *out_error = ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
            return NULL;
        }
    }

    uvc_host_frame_info_t *info = calloc(info_count, sizeof(*info));
    camera_candidate_t *candidates = calloc(info_count, sizeof(*candidates));
    if (info == NULL || candidates == NULL) {
        free(info);
        free(candidates);
        *out_error = ESP_ERR_NO_MEM;
        return NULL;
    }

    size_t actual_count = info_count;
    esp_err_t ret = uvc_host_get_frame_list(device->dev_addr, device->stream_index,
                                            (uvc_host_frame_info_t (*)[])info, &actual_count);
    if (ret != ESP_OK) {
        free(info);
        free(candidates);
        *out_error = ret;
        return NULL;
    }

    size_t candidate_count = 0;
    for (size_t i = 0; i < actual_count; ++i) {
        const bool is_mjpeg = info[i].format == UVC_VS_FORMAT_MJPEG;
        const bool is_yuy2 = info[i].format == UVC_VS_FORMAT_YUY2;
        if ((!is_mjpeg && !is_yuy2) || info[i].h_res == 0U || info[i].v_res == 0U ||
                info[i].h_res > USB_CAMERA_MAX_WIDTH || info[i].v_res > USB_CAMERA_MAX_HEIGHT) {
            continue;
        }
        if (is_yuy2 && ((info[i].h_res & 1U) != 0U ||
                info[i].h_res > USB_CAMERA_YUY2_MAX_WIDTH ||
                info[i].v_res > USB_CAMERA_YUY2_MAX_HEIGHT)) {
            continue;
        }
        const uint32_t interval = camera_closest_interval(&info[i]);
        candidates[candidate_count++] = (camera_candidate_t) {
            .format = info[i].format,
            .width = (uint16_t)info[i].h_res,
            .height = (uint16_t)info[i].v_res,
            .interval_100ns = interval,
            .fps = interval == 0U ? 0.0f : 10000000.0f / (float)interval,
        };
    }
    free(info);

    if (candidate_count == 0U) {
        free(candidates);
        *out_error = ESP_ERR_NOT_SUPPORTED;
        return NULL;
    }
    qsort(candidates, candidate_count, sizeof(*candidates), camera_candidate_compare);
    *out_count = candidate_count;
    *out_error = ESP_OK;
    return candidates;
}

static bool camera_reserve_output_slot(uint8_t *out_slot, uint32_t *out_generation)
{
    int selected = -1;
    uint32_t oldest_sequence = UINT32_MAX;
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < USB_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
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
    if (slot_index < USB_CAMERA_OUTPUT_BUFFER_COUNT &&
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
    uint32_t sequence = 0U;
    portENTER_CRITICAL(&s_camera.output_lock);
    if (slot_index < USB_CAMERA_OUTPUT_BUFFER_COUNT &&
            s_camera.output[slot_index].state == OUTPUT_SLOT_WRITING &&
            s_camera.output[slot_index].generation == generation) {
        for (size_t i = 0; i < USB_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
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

static inline uint8_t camera_clamp_color(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static inline uint16_t camera_yuv_pixel_to_rgb565(uint8_t y, int u, int v)
{
    int c = (int)y - 16;
    if (c < 0) {
        c = 0;
    }
    const uint8_t r = camera_clamp_color((298 * c + 409 * v + 128) >> 8);
    const uint8_t g = camera_clamp_color((298 * c - 100 * u - 208 * v + 128) >> 8);
    const uint8_t b = camera_clamp_color((298 * c + 516 * u + 128) >> 8);
    return (uint16_t)(((uint16_t)(r & 0xf8U) << 8) |
                      ((uint16_t)(g & 0xfcU) << 3) |
                      ((uint16_t)b >> 3));
}

static bool camera_convert_yuy2(const uvc_host_frame_t *frame, output_slot_t *output,
                                uint16_t *out_width, uint16_t *out_height,
                                uint16_t *out_stride, size_t *out_size)
{
    if (frame == NULL || frame->data == NULL || output == NULL || output->data == NULL) {
        return false;
    }
    const uint32_t width = frame->vs_format.h_res;
    const uint32_t height = frame->vs_format.v_res;
    if (width == 0U || height == 0U || width > USB_CAMERA_YUY2_MAX_WIDTH ||
            height > USB_CAMERA_YUY2_MAX_HEIGHT || (width & 1U) != 0U) {
        return false;
    }
    const size_t required = (size_t)width * height * 2U;
    if (frame->data_len < required || output->capacity < required) {
        return false;
    }

    const uint8_t *source = frame->data;
    uint16_t *destination = (uint16_t *)output->data;
    const size_t pairs = (size_t)width * height / 2U;
    for (size_t i = 0; i < pairs; ++i) {
        const uint8_t y0 = source[0];
        const int u = (int)source[1] - 128;
        const uint8_t y1 = source[2];
        const int v = (int)source[3] - 128;
        destination[0] = camera_yuv_pixel_to_rgb565(y0, u, v);
        destination[1] = camera_yuv_pixel_to_rgb565(y1, u, v);
        source += 4;
        destination += 2;
    }
    *out_width = (uint16_t)width;
    *out_height = (uint16_t)height;
    *out_stride = (uint16_t)(width * 2U);
    *out_size = required;
    return true;
}

static bool camera_decode_mjpeg(const uvc_host_frame_t *frame, output_slot_t *output,
                                uint16_t *out_width, uint16_t *out_height,
                                uint16_t *out_stride, size_t *out_size)
{
    if (frame->data == NULL || frame->data_len < 4U ||
            frame->data_len > s_camera.input_capacity) {
        return false;
    }

    jpeg_decode_picture_info_t info = {0};
    esp_err_t ret = jpeg_decoder_get_info(frame->data, (uint32_t)frame->data_len, &info);
    if (ret != ESP_OK || info.width == 0U || info.height == 0U ||
            info.width > USB_CAMERA_MAX_WIDTH || info.height > USB_CAMERA_MAX_HEIGHT) {
        camera_set_error(ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret);
        return false;
    }

    uint32_t mcu_width = 16U;
    switch (info.sample_method) {
        case JPEG_DOWN_SAMPLING_YUV444:
        case JPEG_DOWN_SAMPLING_GRAY:
            mcu_width = 8U;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
        case JPEG_DOWN_SAMPLING_YUV420:
        default:
            mcu_width = 16U;
            break;
    }
    const uint32_t padded_width = (info.width + mcu_width - 1U) & ~(mcu_width - 1U);
    const uint32_t padded_height = (info.height + 15U) & ~15U;
    const size_t maximum_output = (size_t)padded_width * padded_height * 2U;
    if (maximum_output > output->capacity) {
        camera_set_error(ESP_ERR_INVALID_SIZE);
        return false;
    }

    const jpeg_decode_cfg_t decode_config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* BGR is the driver's documented little-endian RGB565 byte order. */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(s_camera.jpeg_decoder, &decode_config,
                               frame->data, (uint32_t)frame->data_len,
                               output->data, (uint32_t)output->capacity,
                               &decoded_size);
    if (ret != ESP_OK || decoded_size < (size_t)padded_width * info.height * 2U) {
        camera_set_error(ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret);
        return false;
    }

    *out_width = (uint16_t)info.width;
    *out_height = (uint16_t)info.height;
    *out_stride = (uint16_t)(padded_width * 2U);
    *out_size = (size_t)(*out_stride) * info.height;
    return true;
}

static void camera_return_input_frame(const camera_input_frame_t *input)
{
    esp_err_t ret = uvc_host_frame_return(input->stream, input->frame);
    if (ret != ESP_OK) {
        camera_enter_fatal(ret);
        ESP_LOGW(TAG, "Returning UVC frame failed: %s", esp_err_to_name(ret));
        if (s_camera.manager_task != NULL) {
            (void)xTaskNotify(s_camera.manager_task, USB_CAMERA_NOTIFY_RETRY_FORMAT, eSetBits);
        }
    }
    /* The decoder no longer dereferences the frame. If the driver rejected the
     * return, fatal quarantine prevents this stream or its buffers being reused. */
    __atomic_sub_fetch(&s_camera.outstanding_frames, 1U, __ATOMIC_RELEASE);
}

static void camera_decode_task(void *arg)
{
    (void)arg;
    camera_input_frame_t input;
    while (true) {
        if (xQueueReceive(s_camera.frame_queue, &input, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint8_t slot_index = 0;
        uint32_t generation = 0;
        if (!camera_reserve_output_slot(&slot_index, &generation)) {
            camera_counter_add(&s_camera.display_drops);
            camera_return_input_frame(&input);
            continue;
        }

        output_slot_t *output = &s_camera.output[slot_index];
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t stride = 0;
        size_t data_size = 0;
        bool success = false;
        switch (input.frame->vs_format.format) {
            case UVC_VS_FORMAT_MJPEG:
                success = camera_decode_mjpeg(input.frame, output, &width, &height, &stride, &data_size);
                break;
            case UVC_VS_FORMAT_YUY2:
                success = camera_convert_yuy2(input.frame, output, &width, &height, &stride, &data_size);
                break;
            default:
                break;
        }

        if (success) {
            __atomic_store_n(&s_camera.decode_error_streak, 0U, __ATOMIC_RELAXED);
            const uint32_t frame_sequence = camera_publish_output_slot(
                slot_index, generation, width, height, stride, data_size);
            camera_counter_add(&s_camera.frames_decoded);
            camera_set_error(ESP_OK);
            if (frame_sequence != 0U) {
                (void)face_recognition_service_submit_rgb565le(
                    output->data, data_size, width, height, stride, frame_sequence);
            }
        } else {
            camera_abandon_output_slot(slot_index, generation);
            camera_counter_add(&s_camera.decode_errors);
            const uint32_t streak = __atomic_add_fetch(&s_camera.decode_error_streak, 1U, __ATOMIC_RELAXED);
            if (streak == USB_CAMERA_DECODE_ERROR_LIMIT && s_camera.manager_task != NULL) {
                (void)xTaskNotify(s_camera.manager_task, USB_CAMERA_NOTIFY_RETRY_FORMAT, eSetBits);
            }
        }
        camera_return_input_frame(&input);
    }
}

static bool camera_frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;
    if (frame == NULL || __atomic_load_n(&s_camera.fatal_error, __ATOMIC_ACQUIRE)) {
        return true;
    }
    camera_counter_add(&s_camera.frames_received);
    uvc_host_stream_hdl_t stream = __atomic_load_n(&s_camera.stream, __ATOMIC_ACQUIRE);
    if (stream == NULL || s_camera.frame_queue == NULL) {
        camera_counter_add(&s_camera.callback_drops);
        return true;
    }

    const camera_input_frame_t input = {
        .stream = stream,
        .frame = (uvc_host_frame_t *)frame,
    };
    __atomic_add_fetch(&s_camera.outstanding_frames, 1U, __ATOMIC_ACQ_REL);
    if (xQueueSend(s_camera.frame_queue, &input, 0) != pdTRUE) {
        __atomic_sub_fetch(&s_camera.outstanding_frames, 1U, __ATOMIC_RELEASE);
        camera_counter_add(&s_camera.callback_drops);
        return true;
    }
    return false;
}

static void camera_stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
        case UVC_HOST_TRANSFER_ERROR: {
            const uint32_t count = camera_counter_add(&s_camera.transfer_errors);
            camera_set_error(event->transfer_error.error);
            if (camera_should_log_count(count)) {
                ESP_LOGW(TAG, "UVC transfer error %s (count %u)",
                         esp_err_to_name(event->transfer_error.error), (unsigned)count);
            }
            break;
        }
        case UVC_HOST_DEVICE_DISCONNECTED:
            if (event->device_disconnected.stream_hdl !=
                    __atomic_load_n(&s_camera.stream, __ATOMIC_ACQUIRE)) {
                ESP_LOGW(TAG, "Ignoring stale UVC disconnect event");
                break;
            }
            __atomic_store_n(&s_camera.streaming, false, __ATOMIC_RELAXED);
            __atomic_store_n(&s_camera.device_connected, false, __ATOMIC_RELAXED);
            /* Advance the enrollment epoch in the same task-context event that
             * observes disconnect. Waiting for the manager would leave a
             * cross-core window in which a third sample could still commit. */
            (void)face_recognition_service_request_cancel_enroll();
            camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_WAITING);
            camera_counter_add(&s_camera.disconnect_count);
            if (s_camera.manager_task != NULL) {
                (void)xTaskNotify(s_camera.manager_task, USB_CAMERA_NOTIFY_DISCONNECTED, eSetBits);
            }
            ESP_LOGI(TAG, "USB camera disconnected");
            break;
        case UVC_HOST_FRAME_BUFFER_OVERFLOW: {
            const uint32_t count = camera_counter_add(&s_camera.frame_overflows);
            if (camera_should_log_count(count)) {
                ESP_LOGW(TAG, "UVC frame overflow (count %u)", (unsigned)count);
            }
            break;
        }
        case UVC_HOST_FRAME_BUFFER_UNDERFLOW: {
            const uint32_t count = camera_counter_add(&s_camera.frame_underflows);
            if (camera_should_log_count(count)) {
                ESP_LOGW(TAG, "UVC frame underflow (count %u)", (unsigned)count);
            }
            break;
        }
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
        case UVC_HOST_DEVICE_SUSPENDED:
            __atomic_store_n(&s_camera.streaming, false, __ATOMIC_RELAXED);
            (void)face_recognition_service_request_cancel_enroll();
            break;
        case UVC_HOST_DEVICE_RESUMED:
            if (s_camera.manager_task != NULL) {
                (void)xTaskNotify(s_camera.manager_task, USB_CAMERA_NOTIFY_RETRY_FORMAT, eSetBits);
            }
            break;
#endif
        default:
            ESP_LOGW(TAG, "Ignoring unknown UVC stream event %d", (int)event->type);
            break;
    }
}

static void camera_driver_callback(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL || event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED ||
            s_camera.device_queue == NULL) {
        return;
    }
    const camera_device_event_t device = {
        .dev_addr = event->device_connected.dev_addr,
        .stream_index = event->device_connected.uvc_stream_index,
        .frame_info_count = event->device_connected.frame_info_num,
    };
    if (xQueueSend(s_camera.device_queue, &device, 0) != pdTRUE) {
        camera_counter_add(&s_camera.callback_drops);
        ESP_LOGW(TAG, "USB camera connection queue full");
    }
}

static esp_err_t camera_wait_until_frames_returned(void)
{
    uint32_t waits = 0;
    const TickType_t started = xTaskGetTickCount();
    while (__atomic_load_n(&s_camera.outstanding_frames, __ATOMIC_ACQUIRE) != 0U) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++waits == 200U) {
            ESP_LOGW(TAG, "Waiting for %u retained UVC frames",
                     (unsigned)__atomic_load_n(&s_camera.outstanding_frames, __ATOMIC_RELAXED));
            waits = 0;
        }
        if ((xTaskGetTickCount() - started) >= pdMS_TO_TICKS(USB_CAMERA_FRAME_RETURN_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "Timed out with %u retained UVC frames",
                     (unsigned)__atomic_load_n(&s_camera.outstanding_frames, __ATOMIC_RELAXED));
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static esp_err_t camera_close_stream(uvc_host_stream_hdl_t stream, bool stop_first)
{
    if (stream == NULL) {
        return ESP_OK;
    }
    if (stop_first) {
        esp_err_t stop_ret = uvc_host_stream_stop(stream);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE && stop_ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Stopping UVC stream failed: %s", esp_err_to_name(stop_ret));
        }
    }
    esp_err_t wait_ret = camera_wait_until_frames_returned();
    if (wait_ret != ESP_OK) {
        camera_enter_fatal(wait_ret);
        return wait_ret;
    }
    esp_err_t last_close_ret = ESP_FAIL;
    for (uint32_t retry = 0; retry < USB_CAMERA_CLOSE_RETRY_COUNT; ++retry) {
        esp_err_t close_ret = uvc_host_stream_close(stream);
        if (close_ret == ESP_OK || close_ret == ESP_ERR_NOT_FOUND) {
            uvc_host_stream_hdl_t expected = stream;
            (void)__atomic_compare_exchange_n(&s_camera.stream, &expected, NULL, false,
                                              __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            return ESP_OK;
        }
        last_close_ret = close_ret;
        camera_set_error(close_ret);
        if (retry == 0U || (retry & 0x0fU) == 0U) {
            ESP_LOGW(TAG, "Closing UVC stream failed: %s (retry %u)",
                     esp_err_to_name(close_ret), (unsigned)(retry + 1U));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "UVC stream could not be closed after %u attempts",
             (unsigned)USB_CAMERA_CLOSE_RETRY_COUNT);
    camera_enter_fatal(last_close_ret);
    return last_close_ret;
}

static esp_err_t camera_try_candidate(const camera_device_event_t *device,
                                      const camera_candidate_t *candidate,
                                      uvc_host_stream_hdl_t *out_stream)
{
    const uvc_host_stream_config_t stream_config = {
        .event_cb = camera_stream_callback,
        .frame_cb = camera_frame_callback,
        .user_ctx = NULL,
        .usb = {
            .dev_addr = device->dev_addr,
            .vid = UVC_HOST_ANY_VID,
            .pid = UVC_HOST_ANY_PID,
            .uvc_stream_index = device->stream_index,
        },
        .vs_format = {
            .h_res = candidate->width,
            .v_res = candidate->height,
            .fps = candidate->fps,
            .format = candidate->format,
        },
        .advanced = {
            .number_of_frame_buffers = USB_CAMERA_INPUT_BUFFER_COUNT,
            .frame_size = s_camera.input_capacity,
            .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            .number_of_urbs = 3,
            .urb_size = 0,
            .user_frame_buffers = s_camera.input_buffers,
        },
    };

    ESP_LOGI(TAG, "Trying UVC %s %ux%u@%.1f",
             camera_format_name(candidate->format),
             (unsigned)candidate->width, (unsigned)candidate->height,
             (double)candidate->fps);
    uvc_host_stream_hdl_t stream = NULL;
    esp_err_t ret = uvc_host_stream_open(&stream_config,
                                         pdMS_TO_TICKS(USB_CAMERA_OPEN_TIMEOUT_MS),
                                         &stream);
    if (ret != ESP_OK) {
        return ret;
    }

    __atomic_store_n(&s_camera.stream, stream, __ATOMIC_RELEASE);
    ret = uvc_host_stream_start(stream);
    if (ret != ESP_OK) {
        esp_err_t close_ret = camera_close_stream(stream, false);
        return close_ret == ESP_OK ? ret : close_ret;
    }
    *out_stream = stream;
    return ESP_OK;
}

static void camera_manager_task(void *arg)
{
    (void)arg;
    camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_WAITING);
    camera_device_event_t device;
    while (true) {
        if (xQueueReceive(s_camera.device_queue, &device, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t stale_notifications = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &stale_notifications, 0);
        camera_counter_add(&s_camera.connect_count);
        __atomic_store_n(&s_camera.device_connected, true, __ATOMIC_RELAXED);
        camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_OPENING);
        ESP_LOGI(TAG, "UVC device address %u stream %u, %u frame descriptors",
                 (unsigned)device.dev_addr, (unsigned)device.stream_index,
                 (unsigned)device.frame_info_count);

        size_t candidate_count = 0;
        esp_err_t error = ESP_OK;
        camera_candidate_t *candidates = camera_build_candidates(&device, &candidate_count, &error);
        bool disconnected = false;
        bool streamed = false;

        for (size_t index = 0; index < candidate_count && !disconnected; ++index) {
            __atomic_store_n(&s_camera.decode_error_streak, 0U, __ATOMIC_RELAXED);
            uvc_host_stream_hdl_t stream = NULL;
            error = camera_try_candidate(&device, &candidates[index], &stream);
            if (error != ESP_OK) {
                camera_set_error(error);
                ESP_LOGW(TAG, "UVC candidate failed: %s", esp_err_to_name(error));
                if (__atomic_load_n(&s_camera.fatal_error, __ATOMIC_ACQUIRE)) {
                    break;
                }
                continue;
            }

            streamed = true;
            xQueueReset(s_camera.device_queue); /* discard duplicate interfaces from this attachment */
            camera_set_error(ESP_OK);
            camera_atomic_store_u32(&s_camera.source_format,
                                        camera_public_format(candidates[index].format));
            camera_atomic_store_u32(&s_camera.width, candidates[index].width);
            camera_atomic_store_u32(&s_camera.height, candidates[index].height);
            camera_atomic_store_u32(&s_camera.fps_milli,
                                        (uint32_t)(candidates[index].fps * 1000.0f + 0.5f));
            __atomic_store_n(&s_camera.streaming, true, __ATOMIC_RELAXED);
            camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_STREAMING);
            ESP_LOGI(TAG, "USB camera streaming %s %ux%u@%.1f",
                     camera_format_name(candidates[index].format),
                     (unsigned)candidates[index].width, (unsigned)candidates[index].height,
                     (double)candidates[index].fps);

            uint32_t last_frames = camera_atomic_load_u32(&s_camera.frames_received);
            TickType_t last_frame_tick = xTaskGetTickCount();
            bool retry_format = false;
            while (!disconnected && !retry_format) {
                uint32_t notifications = 0;
                (void)xTaskNotifyWait(0, UINT32_MAX, &notifications, pdMS_TO_TICKS(500));
                if ((notifications & USB_CAMERA_NOTIFY_DISCONNECTED) != 0U) {
                    disconnected = true;
                    break;
                }
                if ((notifications & USB_CAMERA_NOTIFY_RETRY_FORMAT) != 0U) {
                    int32_t reported = __atomic_load_n(&s_camera.last_error, __ATOMIC_RELAXED);
                    error = reported == ESP_OK ? ESP_FAIL : (esp_err_t)reported;
                    retry_format = true;
                    break;
                }
                const uint32_t now_frames = camera_atomic_load_u32(&s_camera.frames_received);
                if (now_frames != last_frames) {
                    last_frames = now_frames;
                    last_frame_tick = xTaskGetTickCount();
                } else if ((xTaskGetTickCount() - last_frame_tick) >=
                           pdMS_TO_TICKS(USB_CAMERA_FRAME_WATCHDOG_MS)) {
                    ESP_LOGW(TAG, "No UVC frames for %u ms; trying fallback",
                             (unsigned)USB_CAMERA_FRAME_WATCHDOG_MS);
                    error = ESP_ERR_TIMEOUT;
                    camera_set_error(error);
                    retry_format = true;
                }
            }

            __atomic_store_n(&s_camera.streaming, false, __ATOMIC_RELAXED);
            (void)face_recognition_service_request_cancel_enroll();
            error = camera_close_stream(stream, !disconnected);
            if (error != ESP_OK) {
                break;
            }
            if (!disconnected) {
                camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_OPENING);
            }
        }
        free(candidates);

        if (__atomic_load_n(&s_camera.fatal_error, __ATOMIC_ACQUIRE)) {
            ESP_LOGE(TAG, "USB camera service stopped safely after unrecoverable teardown failure");
            vTaskSuspend(NULL);
            continue;
        }

        __atomic_store_n(&s_camera.streaming, false, __ATOMIC_RELAXED);
        __atomic_store_n(&s_camera.device_connected, false, __ATOMIC_RELAXED);
        camera_atomic_store_u32(&s_camera.source_format, USB_CAMERA_FORMAT_NONE);
        camera_atomic_store_u32(&s_camera.width, 0U);
        camera_atomic_store_u32(&s_camera.height, 0U);
        camera_atomic_store_u32(&s_camera.fps_milli, 0U);
        if (!disconnected) {
            if (error == ESP_OK) {
                error = ESP_ERR_NOT_SUPPORTED;
            }
            camera_set_error(error);
            camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_ERROR);
            ESP_LOGW(TAG, "No usable USB camera stream: %s", esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(streamed ? 300U : 1200U));
        }
        camera_atomic_store_u32(&s_camera.state, USB_CAMERA_STATE_WAITING);
    }
}

static void camera_free_buffers(void)
{
    for (size_t i = 0; i < USB_CAMERA_INPUT_BUFFER_COUNT; ++i) {
        free(s_camera.input_buffers[i]);
        s_camera.input_buffers[i] = NULL;
    }
    for (size_t i = 0; i < USB_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        free(s_camera.output[i].data);
        memset(&s_camera.output[i], 0, sizeof(s_camera.output[i]));
    }
    s_camera.input_capacity = 0;
}

static esp_err_t camera_allocate_buffers(void)
{
    const jpeg_decode_memory_alloc_cfg_t input_config = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    const jpeg_decode_memory_alloc_cfg_t output_config = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    size_t common_input_capacity = 0;
    for (size_t i = 0; i < USB_CAMERA_INPUT_BUFFER_COUNT; ++i) {
        size_t capacity = 0;
        s_camera.input_buffers[i] = jpeg_alloc_decoder_mem(USB_CAMERA_INPUT_FRAME_SIZE,
                                                           &input_config, &capacity);
        if (s_camera.input_buffers[i] == NULL) {
            camera_free_buffers();
            return ESP_ERR_NO_MEM;
        }
        if (i == 0U) {
            common_input_capacity = capacity;
        } else if (capacity < common_input_capacity) {
            common_input_capacity = capacity;
        }
    }
    s_camera.input_capacity = common_input_capacity;

    for (size_t i = 0; i < USB_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        size_t capacity = 0;
        s_camera.output[i].data = jpeg_alloc_decoder_mem(USB_CAMERA_OUTPUT_FRAME_SIZE,
                                                         &output_config, &capacity);
        if (s_camera.output[i].data == NULL) {
            camera_free_buffers();
            return ESP_ERR_NO_MEM;
        }
        s_camera.output[i].capacity = capacity;
        s_camera.output[i].generation = 1U;
        s_camera.output[i].state = OUTPUT_SLOT_FREE;
    }
    return ESP_OK;
}

static void camera_start_cleanup(bool uvc_installed)
{
    if (s_camera.manager_task != NULL) {
        vTaskDelete(s_camera.manager_task);
        s_camera.manager_task = NULL;
    }
    if (s_camera.decode_task != NULL) {
        vTaskDelete(s_camera.decode_task);
        s_camera.decode_task = NULL;
    }
    if (uvc_installed) {
        esp_err_t ret = uvc_host_uninstall();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "UVC cleanup failed: %s", esp_err_to_name(ret));
        }
    }
    if (s_camera.jpeg_decoder != NULL) {
        (void)jpeg_del_decoder_engine(s_camera.jpeg_decoder);
        s_camera.jpeg_decoder = NULL;
    }
    if (s_camera.device_queue != NULL) {
        vQueueDelete(s_camera.device_queue);
        s_camera.device_queue = NULL;
    }
    if (s_camera.frame_queue != NULL) {
        vQueueDelete(s_camera.frame_queue);
        s_camera.frame_queue = NULL;
    }
    camera_free_buffers();
}

esp_err_t usb_camera_start(void)
{
    if (__atomic_exchange_n(&s_camera.starting, true, __ATOMIC_ACQ_REL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (__atomic_load_n(&s_camera.started, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&s_camera.fatal_error, false, __ATOMIC_RELEASE);

    s_camera.device_queue = xQueueCreate(USB_CAMERA_DEVICE_QUEUE_LENGTH,
                                         sizeof(camera_device_event_t));
    s_camera.frame_queue = xQueueCreate(USB_CAMERA_FRAME_QUEUE_LENGTH,
                                        sizeof(camera_input_frame_t));
    if (s_camera.device_queue == NULL || s_camera.frame_queue == NULL) {
        camera_start_cleanup(false);
        camera_set_error(ESP_ERR_NO_MEM);
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = camera_allocate_buffers();
    if (ret != ESP_OK) {
        camera_start_cleanup(false);
        camera_set_error(ret);
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ret;
    }

    const jpeg_decode_engine_cfg_t decoder_config = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    ret = jpeg_new_decoder_engine(&decoder_config, &s_camera.jpeg_decoder);
    if (ret != ESP_OK) {
        camera_start_cleanup(false);
        camera_set_error(ret);
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ret;
    }

    const uvc_host_driver_config_t driver_config = {
        .driver_task_stack_size = USB_CAMERA_UVC_TASK_STACK_SIZE,
        .driver_task_priority = USB_CAMERA_UVC_TASK_PRIORITY,
        .xCoreID = USB_CAMERA_TASK_CORE,
        .create_background_task = true,
        .event_cb = camera_driver_callback,
        .user_ctx = NULL,
    };
    ret = uvc_host_install(&driver_config);
    if (ret != ESP_OK) {
        camera_start_cleanup(false);
        camera_set_error(ret);
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ret;
    }

    if (xTaskCreatePinnedToCore(camera_decode_task, "usb_cam_decode",
                                USB_CAMERA_DECODE_STACK_SIZE, NULL,
                                USB_CAMERA_DECODE_PRIORITY, &s_camera.decode_task,
                                USB_CAMERA_TASK_CORE) != pdPASS ||
            xTaskCreatePinnedToCore(camera_manager_task, "usb_cam_manager",
                                    USB_CAMERA_MANAGER_STACK_SIZE, NULL,
                                    USB_CAMERA_MANAGER_PRIORITY, &s_camera.manager_task,
                                    USB_CAMERA_TASK_CORE) != pdPASS) {
        camera_start_cleanup(true);
        camera_set_error(ESP_ERR_NO_MEM);
        __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }

    __atomic_store_n(&s_camera.service_ready, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_camera.started, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_camera.starting, false, __ATOMIC_RELEASE);
    camera_set_error(ESP_OK);
    ESP_LOGI(TAG, "USB UVC service ready on core %d; PSRAM buffers %u input + %u RGB565",
             USB_CAMERA_TASK_CORE,
             (unsigned)(USB_CAMERA_INPUT_BUFFER_COUNT * s_camera.input_capacity),
             (unsigned)(USB_CAMERA_OUTPUT_BUFFER_COUNT * s_camera.output[0].capacity));
    return ESP_OK;
}

bool usb_camera_get_stats(usb_camera_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return false;
    }

    bool frame_available = false;
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < USB_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
        if (s_camera.output[i].state == OUTPUT_SLOT_READY) {
            frame_available = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_camera.output_lock);

    *out_stats = (usb_camera_stats_t) {
        .state = (usb_camera_state_t)camera_atomic_load_u32(&s_camera.state),
        .source_format = (usb_camera_format_t)camera_atomic_load_u32(&s_camera.source_format),
        .last_error = (esp_err_t)__atomic_load_n(&s_camera.last_error, __ATOMIC_RELAXED),
        .fps = camera_atomic_load_u32(&s_camera.fps_milli) / 1000.0f,
        .width = (uint16_t)camera_atomic_load_u32(&s_camera.width),
        .height = (uint16_t)camera_atomic_load_u32(&s_camera.height),
        .connect_count = camera_atomic_load_u32(&s_camera.connect_count),
        .disconnect_count = camera_atomic_load_u32(&s_camera.disconnect_count),
        .frames_received = camera_atomic_load_u32(&s_camera.frames_received),
        .frames_decoded = camera_atomic_load_u32(&s_camera.frames_decoded),
        .callback_drops = camera_atomic_load_u32(&s_camera.callback_drops),
        .display_drops = camera_atomic_load_u32(&s_camera.display_drops),
        .decode_errors = camera_atomic_load_u32(&s_camera.decode_errors),
        .transfer_errors = camera_atomic_load_u32(&s_camera.transfer_errors),
        .frame_overflows = camera_atomic_load_u32(&s_camera.frame_overflows),
        .frame_underflows = camera_atomic_load_u32(&s_camera.frame_underflows),
        .manager_stack_min_free_bytes = s_camera.manager_task != NULL ?
                                        (uint32_t)uxTaskGetStackHighWaterMark(s_camera.manager_task) : 0U,
        .decode_stack_min_free_bytes = s_camera.decode_task != NULL ?
                                       (uint32_t)uxTaskGetStackHighWaterMark(s_camera.decode_task) : 0U,
        .service_ready = __atomic_load_n(&s_camera.service_ready, __ATOMIC_ACQUIRE),
        .device_connected = __atomic_load_n(&s_camera.device_connected, __ATOMIC_RELAXED),
        .streaming = __atomic_load_n(&s_camera.streaming, __ATOMIC_RELAXED),
        .frame_available = frame_available,
    };
    return __atomic_load_n(&s_camera.started, __ATOMIC_ACQUIRE);
}

bool usb_camera_acquire_latest_frame(usb_camera_frame_t *out_frame)
{
    if (out_frame == NULL) {
        return false;
    }

    int selected = -1;
    uint32_t newest_sequence = 0;
    portENTER_CRITICAL(&s_camera.output_lock);
    for (size_t i = 0; i < USB_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
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
        *out_frame = (usb_camera_frame_t) {
            .data = slot->data,
            .data_size = slot->data_size,
            .width = slot->width,
            .height = slot->height,
            .stride = slot->stride,
            .sequence = slot->sequence,
            ._slot = (uint8_t)selected,
            ._generation = slot->generation,
        };
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
    return selected >= 0;
}

void usb_camera_present_frame(const usb_camera_frame_t *frame)
{
    if (frame == NULL || frame->_slot >= USB_CAMERA_OUTPUT_BUFFER_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_camera.output_lock);
    output_slot_t *slot = &s_camera.output[frame->_slot];
    if (slot->state == OUTPUT_SLOT_ACQUIRED && slot->generation == frame->_generation) {
        for (size_t i = 0; i < USB_CAMERA_OUTPUT_BUFFER_COUNT; ++i) {
            if (i != frame->_slot && s_camera.output[i].state == OUTPUT_SLOT_DISPLAYED) {
                s_camera.output[i].state = OUTPUT_SLOT_FREE;
            }
        }
        slot->state = OUTPUT_SLOT_DISPLAYED;
    }
    portEXIT_CRITICAL(&s_camera.output_lock);
}

void usb_camera_release_frame(const usb_camera_frame_t *frame)
{
    if (frame == NULL || frame->_slot >= USB_CAMERA_OUTPUT_BUFFER_COUNT) {
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
