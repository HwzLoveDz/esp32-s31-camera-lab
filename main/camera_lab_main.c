/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_device.h"
#include "dev_display_lcd.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "camera_lab_ui.h"
#include "usb_camera.h"
#include "onboard_camera.h"
#include "face_recognition_service.h"

static const char *TAG = "camera_lab";

static void usb_host_events(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        esp_err_t ret = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB host event error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ret = usb_host_device_free_all();
            if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(TAG, "USB release: %s", esp_err_to_name(ret));
            }
        }
    }
}

static esp_err_t start_usb_host(void)
{
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t ret = usb_host_install(&config);
    if (ret != ESP_OK) return ret;
    if (xTaskCreatePinnedToCore(usb_host_events, "usb_host_events", 4096, NULL,
                              9, NULL, 1) != pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static int camera_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    usb_camera_stats_t usb = {0};
    onboard_camera_stats_t dvp = {0};
    face_recognition_snapshot_t face = {0};
    camera_lab_ui_stats_t ui = {0};
    usb_camera_get_stats(&usb);
    onboard_camera_get_stats(&dvp);
    face_recognition_service_get_snapshot(&face);
    camera_lab_ui_get_stats(&ui);
    printf("CAMERA_STATUS uptime=%" PRId64 " usb=%d usb_fps=%.2f received=%" PRIu32
           " decoded=%" PRIu32 " decode_errors=%" PRIu32 " transfer_errors=%" PRIu32
           " dvp=%d dvp_fps=%.2f captured=%" PRIu32 " capture_errors=%" PRIu32
           " ai=%" PRIu32 " faces=%u detected=%" PRIu32 " ai_ms=%" PRIu32
           " ai_max_ms=%" PRIu32 " recognized=%" PRIu32 " identities=%u enrolled=%" PRIu32
           " ai_errors=%" PRIu32 " ai_stack=%" PRIu32 " heap=%u internal=%u\n",
           esp_timer_get_time() / 1000000, usb.streaming, usb.fps, usb.frames_received,
           usb.frames_decoded, usb.decode_errors, usb.transfer_errors, dvp.streaming, dvp.fps,
           dvp.frames_captured, dvp.capture_errors, face.inference_count, face.face_count,
           face.faces_detected, face.inference_ms, face.inference_max_ms,
           face.recognition_count, face.identity_count, face.enroll_count, face.error_count,
           face.task_stack_min_free_bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("CAMERA_UI ready=%d usb_frames=%" PRIu32 " dvp_frames=%" PRIu32
           " swaps=%" PRIu32 " freezes=%" PRIu32 " cleans=%" PRIu32 " frozen=%d clean=%d"
           " board_main=%d gallery=%d gaps=%" PRIu32 " rejected=%" PRIu32
           " usb_age=%" PRIu32 " dvp_age=%" PRIu32 "\n",
           ui.ready, ui.usb_presentations, ui.onboard_presentations, ui.swap_count,
           ui.freeze_count, ui.clean_count, ui.frozen, ui.clean_view, ui.onboard_is_main,
           ui.gallery_open, ui.held_gap_count, ui.rejected_frames, ui.usb_frame_age_ms,
           ui.onboard_frame_age_ms);
    printf("CAMERA_PIPE callback_drops=%" PRIu32 " display_drops=%" PRIu32
           " overflow=%" PRIu32 " underflow=%" PRIu32 " manager_stack=%" PRIu32
           " decode_stack=%" PRIu32 " dvp_drops=%" PRIu32 " dvp_copy_max_us=%" PRIu32
           " dvp_stack=%" PRIu32 "\n",
           usb.callback_drops, usb.display_drops, usb.frame_overflows, usb.frame_underflows,
           usb.manager_stack_min_free_bytes, usb.decode_stack_min_free_bytes,
           dvp.display_drops, dvp.copy_time_max_us, dvp.task_stack_min_free_bytes);
    return 0;
}

static int camera_action(int argc, char **argv)
{
    if (argc != 2) {
        printf("camera_action swap|freeze|clean|gallery|add|cancel|delete\n");
        return 1;
    }
    bool accepted = camera_lab_ui_command(argv[1]);
    printf("CAMERA_ACTION %s %s\n", argv[1], accepted ? "queued" : "rejected");
    return accepted ? 0 : 1;
}

// Capture the rendered framebuffer for host-side UI review. The copy is made
// under the LVGL lock; slow serial transmission never blocks live rendering.
static int camera_screen(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint16_t *copy = heap_caps_malloc(400 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *line = malloc(1620);
    if (!copy || !line) { free(copy); free(line); return 1; }
    if (!lvgl_port_lock(1000)) { free(copy); free(line); return 1; }
    lv_draw_buf_t *buf = lv_display_get_buf_active(lv_display_get_default());
    if (!buf || buf->header.w != 800 || buf->header.h != 480 ||
        buf->header.cf != LV_COLOR_FORMAT_RGB565) {
        lvgl_port_unlock(); free(copy); free(line); return 1;
    }
    for (unsigned y = 0; y < 240; ++y) {
        const uint16_t *row = (const uint16_t *)(buf->data + y * 2 * buf->header.stride);
        for (unsigned x = 0; x < 400; ++x) copy[y * 400 + x] = row[x * 2];
    }
    lvgl_port_unlock();
    printf("CAMSCREEN_BEGIN 400 240 RGB565LE\n");
    const char hex[] = "0123456789abcdef";
    for (unsigned y = 0; y < 240; ++y) {
        int prefix = snprintf(line, 20, "CAMROW %03u ", y);
        const uint8_t *row = (const uint8_t *)(copy + y * 400);
        for (unsigned x = 0; x < 800; ++x) {
            line[prefix + x * 2] = hex[row[x] >> 4];
            line[prefix + x * 2 + 1] = hex[row[x] & 15];
        }
        line[prefix + 1600] = '\n';
        fwrite(line, 1, prefix + 1601, stdout);
        vTaskDelay(1);
    }
    printf("CAMSCREEN_END\n");
    free(copy); free(line);
    return 0;
}

static void start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = "CameraLab >";
    esp_console_dev_uart_config_t uart = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart, &config, &repl));
    const esp_console_cmd_t commands[] = {
        {.command = "camera_status", .help = "Camera and AI diagnostics", .func = camera_status},
        {.command = "camera_action", .help = "Operate the camera UI", .func = camera_action},
        {.command = "camera_screen", .help = "Export a small framebuffer preview", .func = camera_screen},
    };
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI CAMERA LAB 1.0 | USB + DVP | ESP-DL PIE V2 on Core 1");
    dev_display_lcd_config_t *original = NULL;
    ESP_ERROR_CHECK(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                       (void **)&original));
    dev_display_lcd_config_t lcd = *original;
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
    lcd.sub_cfg.rgb.panel_config.num_fbs = 2;
    // Internal bounce buffers cushion LCD scanout from USB JPEG/AI/DVP PSRAM
    // bandwidth bursts. Keep cache invalidation off with a multicore renderer.
    lcd.sub_cfg.rgb.panel_config.bounce_buffer_size_px = lcd.lcd_width * 20;
    lcd.sub_cfg.rgb.panel_config.flags.bb_invalidate_cache = false;
    ESP_LOGI(TAG, "LCD double framebuffer + 20-line internal bounce buffers");
#endif
    ESP_ERROR_CHECK(esp_board_device_override_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                    &lcd, sizeof(lcd)));
    ESP_ERROR_CHECK(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD));
    esp_err_t ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH);
    if (ret != ESP_OK) ESP_LOGW(TAG, "Touch: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(camera_lab_ui_start());
    start_console();
    ret = face_recognition_service_start();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Face AI: %s", esp_err_to_name(ret));
    ret = start_usb_host();
    if (ret == ESP_OK) ret = usb_camera_start();
    if (ret != ESP_OK) ESP_LOGW(TAG, "USB camera: %s", esp_err_to_name(ret));
    ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
    if (ret == ESP_OK) ret = onboard_camera_start();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Board camera: %s", esp_err_to_name(ret));
    ESP_LOGI(TAG, "CAMERA_LAB_READY: local preview; no Bluetooth/audio startup; identities in RAM");
}
