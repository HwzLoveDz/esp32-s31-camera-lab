/* SPDX-License-Identifier: Apache-2.0 */
#include "camera_lab_ui.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "face_recognition_service.h"
#include "onboard_camera.h"
#include "usb_camera.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#define SCREEN_W 800
#define SCREEN_H 480
#define MAIN_X 80
#define MAIN_W 640
#define MAIN_H 480
#define PIP_X 494
#define PIP_Y 62
#define PIP_W 212
#define PIP_H 162
#define PIP_INSET 6
#define COLOR_BG 0x071017
#define COLOR_PANEL 0x0d202a
#define COLOR_TEXT 0xe7f4f4
#define COLOR_MUTED 0x8aa5ad
#define COLOR_MINT 0x6ce7cc
#define COLOR_AMBER 0xf4bf71
#define COLOR_RED 0xfa867b
#define FONT (&lv_font_montserrat_14)
#define FRAME_FRESH_MS 1500U

static const char *TAG = "camera_lab_ui";

typedef enum {
    ACTION_SWAP, ACTION_FREEZE, ACTION_CLEAN, ACTION_GALLERY,
    ACTION_ADD, ACTION_CANCEL, ACTION_DELETE, ACTION_COUNT
} ui_action_t;

typedef struct {
    lv_obj_t *box;
    lv_obj_t *name;
} face_overlay_t;

typedef struct {
    lv_obj_t *view;
    lv_obj_t *image;
    lv_obj_t *badge;
    lv_obj_t *empty;
    lv_image_dsc_t descriptor;
    uint32_t last_frame_ms;
    bool presented;
    bool gap_active;
} camera_view_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *header;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *toolbar;
    lv_obj_t *freeze_label;
    lv_obj_t *left_rail;
    lv_obj_t *right_rail;
    lv_obj_t *usb_fps;
    lv_obj_t *board_fps;
    lv_obj_t *face_count;
    lv_obj_t *ai_ms;
    lv_obj_t *session_label;
    lv_obj_t *status;
    lv_obj_t *clean_button;
    lv_obj_t *clean_label;
    lv_obj_t *gallery;
    lv_obj_t *gallery_status;
    lv_obj_t *profiles[FACE_RECOGNITION_MAX_IDENTITIES];
    lv_obj_t *profile_names[FACE_RECOGNITION_MAX_IDENTITIES];
    lv_obj_t *profile_details[FACE_RECOGNITION_MAX_IDENTITIES];
    lv_obj_t *enroll_button;
    lv_obj_t *delete_button;
    lv_obj_t *delete_label;
    lv_obj_t *progress;
    lv_obj_t *toast;
    camera_view_t usb;
    camera_view_t board;
    face_overlay_t faces[FACE_RECOGNITION_MAX_FACES];
    usb_camera_frame_t usb_frame;
    onboard_camera_frame_t board_frame;
    face_recognition_snapshot_t frozen_faces;
    QueueHandle_t commands;
    lv_timer_t *timer;
    camera_lab_ui_stats_t stats;
    uint32_t last_hud_ms;
    uint32_t last_faces_ms;
    uint32_t delete_deadline;
    uint32_t toast_deadline;
    bool delete_armed;
} camera_lab_ui_t;

static camera_lab_ui_t s_ui;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static camera_lab_ui_stats_t s_published;

static void flag_visible(lv_obj_t *obj, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, FONT, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(obj, text);
    return obj;
}

static void set_label(lv_obj_t *obj, const char *text)
{
    if (strcmp(lv_label_get_text(obj), text) != 0) {
        lv_label_set_text(obj, text);
    }
}

static void toast(const char *text)
{
    set_label(s_ui.toast, text);
    lv_obj_align(s_ui.toast, LV_ALIGN_BOTTOM_MID, 0, -78);
    lv_obj_remove_flag(s_ui.toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.toast);
    s_ui.toast_deadline = lv_tick_get() + 2200U;
}

static void reset_delete(void)
{
    s_ui.delete_armed = false;
    s_ui.delete_deadline = 0;
    set_label(s_ui.delete_label, "DELETE LAST");
}

static void apply_roles(void)
{
    camera_view_t *main = s_ui.stats.onboard_is_main ? &s_ui.board : &s_ui.usb;
    camera_view_t *pip = s_ui.stats.onboard_is_main ? &s_ui.usb : &s_ui.board;

    /* Each camera owns a permanent image and descriptor. Reposition the two
     * views in one LVGL task pass: no NULL sources, lease release or empty frame. */
    lv_obj_set_pos(main->view, MAIN_X, 0);
    lv_obj_set_size(main->view, MAIN_W, MAIN_H);
    lv_obj_set_style_border_width(main->view, 0, 0);
    lv_obj_set_style_radius(main->view, 0, 0);
    lv_obj_set_pos(main->image, 0, 0);
    lv_obj_set_size(main->image, MAIN_W, MAIN_H);
    flag_visible(main->badge, false);
    lv_obj_center(main->empty);

    lv_obj_set_pos(pip->view, PIP_X, PIP_Y);
    lv_obj_set_size(pip->view, PIP_W, PIP_H);
    lv_obj_set_style_border_width(pip->view, 2, 0);
    lv_obj_set_style_radius(pip->view, 12, 0);
    lv_obj_set_pos(pip->image, PIP_INSET, PIP_INSET);
    lv_obj_set_size(pip->image, 200, 150);
    flag_visible(pip->badge, !s_ui.stats.clean_view);
    lv_obj_align(pip->badge, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_center(pip->empty);

    lv_obj_move_foreground(pip->view);
    lv_obj_move_foreground(s_ui.left_rail);
    lv_obj_move_foreground(s_ui.right_rail);
    lv_obj_move_foreground(s_ui.header);
    lv_obj_move_foreground(s_ui.toolbar);
    lv_obj_move_foreground(s_ui.status);
    lv_obj_move_foreground(s_ui.clean_button);
    lv_obj_move_foreground(s_ui.gallery);
    lv_obj_move_foreground(s_ui.toast);
    set_label(s_ui.title, s_ui.stats.onboard_is_main ? "BOARD / 02" : "USB / 01");
    s_ui.last_faces_ms = 0;
}

static void apply_clean(void)
{
    bool hud = !s_ui.stats.clean_view;
    flag_visible(s_ui.header, hud);
    flag_visible(s_ui.toolbar, hud);
    flag_visible(s_ui.left_rail, hud);
    flag_visible(s_ui.right_rail, hud);
    flag_visible(s_ui.status, hud);
    set_label(s_ui.clean_label, hud ? "CLEAN" : "HUD");
    if (!hud && s_ui.stats.gallery_open) {
        s_ui.stats.gallery_open = false;
        flag_visible(s_ui.gallery, false);
        (void)face_recognition_service_request_cancel_enroll();
        reset_delete();
    }
    apply_roles();
}

static void action(ui_action_t cmd)
{
    switch (cmd) {
    case ACTION_SWAP:
        if (s_ui.stats.onboard_is_main ? !s_ui.usb.presented : !s_ui.board.presented) {
            toast("Waiting for the other camera's first frame");
            break;
        }
        s_ui.stats.onboard_is_main = !s_ui.stats.onboard_is_main;
        s_ui.stats.swap_count++;
        apply_roles();
        break;
    case ACTION_FREEZE:
        s_ui.stats.frozen = !s_ui.stats.frozen;
        s_ui.stats.freeze_count++;
        if (s_ui.stats.frozen) {
            memset(&s_ui.frozen_faces, 0, sizeof(s_ui.frozen_faces));
            (void)face_recognition_service_get_snapshot(&s_ui.frozen_faces);
            (void)face_recognition_service_request_cancel_enroll();
            set_label(s_ui.freeze_label, LV_SYMBOL_PLAY "  LIVE");
            toast("Frame held - tap LIVE to resume");
        } else {
            set_label(s_ui.freeze_label, LV_SYMBOL_PAUSE "  HOLD");
            s_ui.usb.gap_active = false;
            s_ui.board.gap_active = false;
        }
        break;
    case ACTION_CLEAN:
        s_ui.stats.clean_view = !s_ui.stats.clean_view;
        s_ui.stats.clean_count++;
        apply_clean();
        break;
    case ACTION_GALLERY:
        s_ui.stats.gallery_open = !s_ui.stats.gallery_open;
        flag_visible(s_ui.gallery, s_ui.stats.gallery_open);
        if (s_ui.stats.gallery_open) {
            lv_obj_move_foreground(s_ui.gallery);
        } else {
            (void)face_recognition_service_request_cancel_enroll();
        }
        reset_delete();
        break;
    case ACTION_ADD:
        reset_delete();
        if (s_ui.stats.frozen) {
            toast("Resume LIVE before adding a face");
        } else if (!face_recognition_service_request_enroll()) {
            toast("AI busy or gallery full - try again");
        } else {
            toast("Look at the USB camera and hold still");
        }
        break;
    case ACTION_CANCEL:
        (void)face_recognition_service_request_cancel_enroll();
        reset_delete();
        toast("Enrollment cancelled");
        break;
    case ACTION_DELETE:
        if (!s_ui.delete_armed) {
            s_ui.delete_armed = true;
            s_ui.delete_deadline = lv_tick_get() + 4000U;
            set_label(s_ui.delete_label, "CONFIRM DELETE");
        } else {
            bool accepted = face_recognition_service_request_delete_last();
            reset_delete();
            toast(accepted ? "Last profile deletion requested" : "No profile to delete or AI busy");
        }
        break;
    default:
        break;
    }
    s_ui.last_hud_ms = 0;
    s_ui.last_faces_ms = 0;
}

static void button_event(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_scroll_obj(indev) != NULL) {
        return;
    }
    action((ui_action_t)(uintptr_t)lv_event_get_user_data(event));
}

static void view_event(lv_event_t *event)
{
    lv_obj_t *clicked = lv_event_get_target(event);
    camera_view_t *pip = s_ui.stats.onboard_is_main ? &s_ui.usb : &s_ui.board;
    if (clicked == pip->view && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        action(ACTION_SWAP);
    } else if (clicked != pip->view && s_ui.stats.clean_view &&
               lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
        action(ACTION_FREEZE);
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y,
                        int w, int h, ui_action_t cmd, bool accent, lv_obj_t **out_text)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(accent ? COLOR_MINT : 0x1a3541), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(accent ? 0x40bca4 : 0x2e5662), LV_STATE_PRESSED);
    lv_obj_set_style_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, button_event, LV_EVENT_CLICKED, (void *)(uintptr_t)cmd);
    lv_obj_t *txt = label(obj, text, 0, 0, accent ? COLOR_BG : COLOR_TEXT);
    lv_obj_center(txt);
    if (out_text != NULL) {
        *out_text = txt;
    }
    return obj;
}

static void view_create(camera_view_t *view, const char *name)
{
    view->view = panel(s_ui.root, MAIN_X, 0, MAIN_W, MAIN_H, COLOR_BG);
    lv_obj_set_style_border_color(view->view, lv_color_hex(COLOR_MINT), 0);
    lv_obj_set_style_clip_corner(view->view, true, 0);
    lv_obj_add_flag(view->view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(view->view, view_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(view->view, view_event, LV_EVENT_LONG_PRESSED, NULL);
    view->image = lv_image_create(view->view);
    lv_obj_set_size(view->image, MAIN_W, MAIN_H);
    lv_image_set_inner_align(view->image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(view->image, false);
    lv_obj_remove_flag(view->image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    flag_visible(view->image, false); /* Only hidden until the very first complete frame. */
    view->empty = label(view->view, "WAITING\nFOR VIDEO", 0, 0, COLOR_MUTED);
    lv_obj_set_style_text_align(view->empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(view->empty);
    view->badge = label(view->view, name, 0, 0, COLOR_TEXT);
    lv_obj_set_style_bg_color(view->badge, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(view->badge, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(view->badge, 4, 0);
    lv_obj_set_style_radius(view->badge, 5, 0);
}

static void overlay_create(void)
{
    for (size_t i = 0; i < FACE_RECOGNITION_MAX_FACES; ++i) {
        face_overlay_t *face = &s_ui.faces[i];
        face->box = panel(s_ui.usb.view, 0, 0, 40, 40, COLOR_BG);
        lv_obj_set_style_bg_opa(face->box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(face->box, 2, 0);
        lv_obj_set_style_border_color(face->box, lv_color_hex(COLOR_AMBER), 0);
        lv_obj_set_style_radius(face->box, 8, 0);
        face->name = label(face->box, "FACE", 3, 3, COLOR_TEXT);
        lv_obj_set_style_bg_opa(face->name, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(face->name, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_pad_all(face->name, 3, 0);
        lv_obj_set_style_radius(face->name, 4, 0);
        flag_visible(face->box, false);
    }
    lv_obj_move_foreground(s_ui.usb.badge);
}

static void gallery_create(void)
{
    s_ui.gallery = panel(s_ui.root, 382, 10, 328, 460, COLOR_PANEL);
    lv_obj_set_style_radius(s_ui.gallery, 18, 0);
    lv_obj_set_style_border_width(s_ui.gallery, 1, 0);
    lv_obj_set_style_border_color(s_ui.gallery, lv_color_hex(0x3d6874), 0);
    lv_obj_add_flag(s_ui.gallery, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *title = label(s_ui.gallery, "FACE\nGALLERY", 20, 15, COLOR_TEXT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    button(s_ui.gallery, LV_SYMBOL_CLOSE, 264, 18, 44, 44, ACTION_GALLERY, false, NULL);
    s_ui.gallery_status = label(s_ui.gallery, "Loading face models...", 20, 80, COLOR_MINT);
    lv_obj_set_size(s_ui.gallery_status, 288, 42);
    lv_label_set_long_mode(s_ui.gallery_status, LV_LABEL_LONG_WRAP);
    s_ui.progress = lv_bar_create(s_ui.gallery);
    lv_obj_set_pos(s_ui.progress, 20, 125);
    lv_obj_set_size(s_ui.progress, 288, 4);
    lv_bar_set_range(s_ui.progress, 0, FACE_RECOGNITION_ENROLL_SAMPLES);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(0x213c46), 0);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(COLOR_MINT), LV_PART_INDICATOR);
    for (size_t i = 0; i < FACE_RECOGNITION_MAX_IDENTITIES; ++i) {
        lv_obj_t *row = panel(s_ui.gallery, 20, 141 + (int)i * 43, 288, 38, 0x142d38);
        s_ui.profiles[i] = row;
        lv_obj_set_style_radius(row, 8, 0);
        char number[4];
        snprintf(number, sizeof(number), "%02u", (unsigned)i + 1U);
        lv_obj_t *num = label(row, number, 10, 10, COLOR_MINT);
        lv_obj_set_style_text_letter_space(num, 1, 0);
        s_ui.profile_names[i] = label(row, "EMPTY SLOT", 45, 4, COLOR_MUTED);
        s_ui.profile_details[i] = label(row, "Add a face to remember it", 45, 21, COLOR_MUTED);
    }
    s_ui.enroll_button = button(s_ui.gallery, "+ ADD FACE", 20, 326, 140, 48,
                               ACTION_ADD, true, NULL);
    button(s_ui.gallery, "CANCEL", 168, 326, 140, 48, ACTION_CANCEL, false, NULL);
    s_ui.delete_button = button(s_ui.gallery, "DELETE LAST", 20, 383, 288, 38,
                               ACTION_DELETE, false, &s_ui.delete_label);
    lv_obj_t *note = label(s_ui.gallery, "Local session  /  resets on reboot", 20, 436, COLOR_MUTED);
    lv_obj_set_style_text_letter_space(note, 0, 0);
    flag_visible(s_ui.gallery, false);
}

static void scene_create(void)
{
    s_ui.root = lv_screen_active();
    lv_obj_set_style_bg_color(s_ui.root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    view_create(&s_ui.usb, "USB / AI");
    view_create(&s_ui.board, "BOARD / 02");
    overlay_create();

    s_ui.left_rail = panel(s_ui.root, 0, 0, 80, SCREEN_H, COLOR_BG);
    label(s_ui.left_rail, "CAM", 14, 20, COLOR_MINT);
    lv_obj_t *lab = label(s_ui.left_rail, "LAB", 12, 42, COLOR_TEXT);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_28, 0);
    panel(s_ui.left_rail, 16, 89, 48, 1, 0x28424c);
    label(s_ui.left_rail, "USB", 16, 110, COLOR_MUTED);
    s_ui.usb_fps = label(s_ui.left_rail, "--", 14, 133, COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.usb_fps, &lv_font_montserrat_28, 0);
    label(s_ui.left_rail, "FPS", 16, 170, COLOR_MUTED);
    label(s_ui.left_rail, "BOARD", 12, 210, COLOR_MUTED);
    s_ui.board_fps = label(s_ui.left_rail, "--", 14, 233, COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.board_fps, &lv_font_montserrat_28, 0);
    label(s_ui.left_rail, "FPS", 16, 270, COLOR_MUTED);
    label(s_ui.left_rail, "DUAL\nVISION", 14, 369, COLOR_MUTED);
    label(s_ui.left_rail, "S31", 14, 439, COLOR_MINT);

    s_ui.right_rail = panel(s_ui.root, 720, 0, 80, SCREEN_H, COLOR_BG);
    label(s_ui.right_rail, "FACE", 18, 22, COLOR_MINT);
    label(s_ui.right_rail, "AI", 25, 48, COLOR_TEXT);
    panel(s_ui.right_rail, 16, 89, 48, 1, 0x28424c);
    s_ui.face_count = label(s_ui.right_rail, "0", 29, 113, COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.face_count, &lv_font_montserrat_32, 0);
    label(s_ui.right_rail, "IN VIEW", 11, 159, COLOR_MUTED);
    s_ui.ai_ms = label(s_ui.right_rail, "--", 10, 211, COLOR_TEXT);
    lv_obj_set_width(s_ui.ai_ms, 60);
    lv_obj_set_style_text_align(s_ui.ai_ms, LV_TEXT_ALIGN_CENTER, 0);
    label(s_ui.right_rail, "AI / ms", 12, 241, COLOR_MUTED);
    s_ui.session_label = label(s_ui.right_rail, "PEAK\n0", 10, 310, COLOR_MUTED);
    lv_obj_set_width(s_ui.session_label, 60);
    lv_obj_set_style_text_align(s_ui.session_label, LV_TEXT_ALIGN_CENTER, 0);

    s_ui.header = panel(s_ui.root, 96, 12, 276, 44, COLOR_BG);
    lv_obj_set_style_bg_opa(s_ui.header, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_ui.header, 10, 0);
    panel(s_ui.header, 10, 12, 4, 20, COLOR_MINT);
    s_ui.title = label(s_ui.header, "USB / 01", 25, 6, COLOR_TEXT);
    s_ui.subtitle = label(s_ui.header, "WAITING FOR VIDEO", 25, 25, COLOR_MUTED);

    s_ui.toolbar = panel(s_ui.root, 96, 414, 608, 54, COLOR_BG);
    lv_obj_set_style_bg_opa(s_ui.toolbar, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_ui.toolbar, 16, 0);
    button(s_ui.toolbar, LV_SYMBOL_REFRESH "  SWAP", 6, 6, 118, 42, ACTION_SWAP, false, NULL);
    button(s_ui.toolbar, LV_SYMBOL_PAUSE "  HOLD", 132, 6, 118, 42, ACTION_FREEZE, false, &s_ui.freeze_label);
    button(s_ui.toolbar, "FACE GALLERY", 258, 6, 184, 42, ACTION_GALLERY, true, NULL);
    label(s_ui.toolbar, "TAP PIP\nTO SWAP", 463, 10, COLOR_MUTED);

    s_ui.status = label(s_ui.root, "Starting cameras and face AI...", 110, 379, COLOR_TEXT);
    lv_obj_set_style_bg_opa(s_ui.status, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_ui.status, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_pad_hor(s_ui.status, 10, 0);
    lv_obj_set_style_pad_ver(s_ui.status, 5, 0);
    lv_obj_set_style_radius(s_ui.status, 8, 0);
    s_ui.clean_button = button(s_ui.root, "CLEAN", 727, 417, 66, 48, ACTION_CLEAN, false, &s_ui.clean_label);
    gallery_create();
    s_ui.toast = label(s_ui.root, "", 0, 0, COLOR_BG);
    lv_obj_set_style_bg_color(s_ui.toast, lv_color_hex(COLOR_MINT), 0);
    lv_obj_set_style_bg_opa(s_ui.toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_ui.toast, 14, 0);
    lv_obj_set_style_pad_ver(s_ui.toast, 10, 0);
    lv_obj_set_style_radius(s_ui.toast, 12, 0);
    flag_visible(s_ui.toast, false);
    apply_roles();
}

static bool valid_frame(const uint8_t *data, size_t size, uint16_t w, uint16_t h, uint16_t stride)
{
    return data != NULL && w > 0 && h > 0 && w <= 640 && h <= 480 &&
           stride >= (uint16_t)(w * 2U) && size >= (size_t)stride * h;
}

static void present(camera_view_t *view, const uint8_t *data, size_t size,
                    uint16_t w, uint16_t h, uint16_t stride, bool swapped)
{
    /* Called only in the LVGL task. Drop the decoder cache before changing the
     * descriptor. The producer releases the previous immutable lease only after
     * this object references the replacement, never on decode errors or gaps. */
    if (view->presented) {
        lv_image_cache_drop(&view->descriptor);
    }
    view->descriptor = (lv_image_dsc_t) {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565,
            .w = w, .h = h, .stride = stride,
        },
        .data_size = size, .data = data,
    };
    lv_image_set_src(view->image, &view->descriptor);
    if (!view->presented) {
        flag_visible(view->image, true);
        flag_visible(view->empty, false);
    }
    lv_obj_invalidate(view->image);
    view->presented = true;
    view->last_frame_ms = lv_tick_get();
    view->gap_active = false;
}

static void poll_frames(void)
{
    if (s_ui.stats.frozen) {
        return;
    }
    usb_camera_frame_t usb;
    if (usb_camera_acquire_latest_frame(&usb)) {
        if (!valid_frame(usb.data, usb.data_size, usb.width, usb.height, usb.stride)) {
            usb_camera_release_frame(&usb);
            s_ui.stats.rejected_frames++;
        } else {
            present(&s_ui.usb, usb.data, usb.data_size, usb.width, usb.height, usb.stride, false);
            usb_camera_present_frame(&usb);
            s_ui.usb_frame = usb;
            s_ui.stats.usb_presentations++;
        }
    }
    onboard_camera_frame_t board;
    if (onboard_camera_acquire_latest_frame(&board)) {
        if (!valid_frame(board.data, board.data_size, board.width, board.height, board.stride)) {
            onboard_camera_release_frame(&board);
            s_ui.stats.rejected_frames++;
        } else {
            present(&s_ui.board, board.data, board.data_size, board.width, board.height,
                    board.stride, board.byte_swapped);
            onboard_camera_present_frame(&board);
            s_ui.board_frame = board;
            s_ui.stats.onboard_presentations++;
        }
    }
}

static void face_update(const face_recognition_snapshot_t *live, bool usb_live)
{
    const face_recognition_snapshot_t *snap = s_ui.stats.frozen ? &s_ui.frozen_faces : live;
    bool fresh = snap->model_ready && s_ui.usb.presented && snap->age_ms <= FRAME_FRESH_MS;
    if (!s_ui.stats.frozen) {
        fresh = fresh && usb_live;
    }
    if (s_ui.usb_frame.sequence > snap->frame_sequence &&
        s_ui.usb_frame.sequence - snap->frame_sequence > 15U) {
        fresh = false;
    }
    bool pip = s_ui.stats.onboard_is_main;
    int w = pip ? 200 : 640;
    int h = pip ? 150 : 480;
    int inset = pip ? PIP_INSET : 0;
    for (size_t i = 0; i < FACE_RECOGNITION_MAX_FACES; ++i) {
        const face_recognition_face_t *face = &snap->faces[i];
        bool show = fresh && i < snap->face_count && !s_ui.stats.clean_view &&
                    face->x2 > face->x1 && face->y2 > face->y1;
        flag_visible(s_ui.faces[i].box, show);
        if (!show) {
            continue;
        }
        int x1 = face->x1 * w / 640;
        int y1 = face->y1 * h / 480;
        int x2 = face->x2 * w / 640;
        int y2 = face->y2 * h / 480;
        if (x2 > w) x2 = w;
        if (y2 > h) y2 = h;
        lv_obj_set_pos(s_ui.faces[i].box, inset + x1, inset + y1);
        lv_obj_set_size(s_ui.faces[i].box, x2 - x1, y2 - y1);
        lv_obj_set_style_border_color(s_ui.faces[i].box,
            lv_color_hex(face->identity_id ? COLOR_MINT : COLOR_AMBER), 0);
        flag_visible(s_ui.faces[i].name, !pip);
        char text[48];
        if (face->identity_id) {
            snprintf(text, sizeof(text), "%.15s / MATCH %d", face->name, (int)(face->similarity * 100.0f));
        } else {
            snprintf(text, sizeof(text), "FACE %u%s", (unsigned)i + 1U, face->primary ? " / FOCUS" : "");
        }
        set_label(s_ui.faces[i].name, text);
    }
}

static void gallery_update(const face_recognition_snapshot_t *snap, bool usb_live)
{
    char text[120];
    if (s_ui.delete_armed) {
        snprintf(text, sizeof(text), "Tap CONFIRM DELETE to remove\nthe last enrolled profile");
    } else if (!snap->model_ready) {
        snprintf(text, sizeof(text), "%s", snap->state == FACE_RECOGNITION_STATE_ERROR ?
                 "Face AI could not start" : "Loading face models...");
    } else if (!usb_live) {
        snprintf(text, sizeof(text), "Connect a USB camera\nFace AI follows the USB view");
    } else if (s_ui.stats.frozen) {
        snprintf(text, sizeof(text), "Frame held\nResume LIVE to add a face");
    } else if (snap->enrolling) {
        snprintf(text, sizeof(text), "PERSON %02u  /  %u of %u samples\n%s",
                 (unsigned)snap->enroll_target_id, (unsigned)snap->enroll_samples_collected,
                 (unsigned)FACE_RECOGNITION_ENROLL_SAMPLES,
                 snap->face_count > 1 ? "Keep only one face in view" : "Look at USB camera - hold still");
    } else {
        snprintf(text, sizeof(text), "%u / %u profiles  /  USB face AI\nAdd yourself, then step back in",
                 (unsigned)snap->identity_count, (unsigned)FACE_RECOGNITION_MAX_IDENTITIES);
    }
    set_label(s_ui.gallery_status, text);
    lv_bar_set_value(s_ui.progress, snap->enrolling ? snap->enroll_samples_collected : 0, LV_ANIM_OFF);
    for (size_t i = 0; i < FACE_RECOGNITION_MAX_IDENTITIES; ++i) {
        bool valid = snap->identities[i].valid;
        set_label(s_ui.profile_names[i], valid ? snap->identities[i].name : "EMPTY SLOT");
        bool seen = false;
        if (valid && usb_live && snap->age_ms <= FRAME_FRESH_MS) {
            for (size_t f = 0; f < snap->face_count && f < FACE_RECOGNITION_MAX_FACES; ++f) {
                seen |= snap->faces[f].identity_id == snap->identities[i].id;
            }
        }
        set_label(s_ui.profile_details[i], seen ? "Recognized now" : valid ?
                  "Ready to recognize" : "Add a face to remember it");
        lv_obj_set_style_text_color(s_ui.profile_names[i], lv_color_hex(valid ? COLOR_TEXT : COLOR_MUTED), 0);
        lv_obj_set_style_bg_color(s_ui.profiles[i], lv_color_hex(seen ? 0x194d46 : 0x142d38), 0);
    }
    bool add_enabled = snap->model_ready && usb_live && !snap->enrolling && !s_ui.stats.frozen &&
                       snap->identity_count < FACE_RECOGNITION_MAX_IDENTITIES;
    bool delete_enabled = snap->model_ready && !snap->enrolling && snap->identity_count > 0;
    if (add_enabled) lv_obj_remove_state(s_ui.enroll_button, LV_STATE_DISABLED);
    else lv_obj_add_state(s_ui.enroll_button, LV_STATE_DISABLED);
    if (delete_enabled) lv_obj_remove_state(s_ui.delete_button, LV_STATE_DISABLED);
    else lv_obj_add_state(s_ui.delete_button, LV_STATE_DISABLED);
}

static void hud_update(const usb_camera_stats_t *usb, const onboard_camera_stats_t *board,
                       const face_recognition_snapshot_t *face)
{
    char text[128];
    snprintf(text, sizeof(text), "%u", (unsigned)(usb->fps < 100 && usb->fps > 0 ? usb->fps + 0.5f : 0));
    set_label(s_ui.usb_fps, text);
    snprintf(text, sizeof(text), "%u", (unsigned)(board->fps < 100 && board->fps > 0 ? board->fps + 0.5f : 0));
    set_label(s_ui.board_fps, text);
    const face_recognition_snapshot_t *shown_face = s_ui.stats.frozen ? &s_ui.frozen_faces : face;
    bool fresh = usb->streaming && shown_face->age_ms <= FRAME_FRESH_MS && shown_face->model_ready;
    unsigned count = fresh ? shown_face->face_count : 0;
    if (count > s_ui.stats.session_max_faces) s_ui.stats.session_max_faces = count;
    snprintf(text, sizeof(text), "%u", count);
    set_label(s_ui.face_count, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)face->inference_ms);
    set_label(s_ui.ai_ms, face->model_ready ? text : "--");
    snprintf(text, sizeof(text), "PEAK\n%u", (unsigned)s_ui.stats.session_max_faces);
    set_label(s_ui.session_label, text);

    bool main_live = s_ui.stats.onboard_is_main ? board->streaming : usb->streaming;
    camera_view_t *main = s_ui.stats.onboard_is_main ? &s_ui.board : &s_ui.usb;
    const char *mode = s_ui.stats.frozen ? "HOLD" : main_live ? "LIVE" : main->presented ? "LAST FRAME" : "WAITING";
    if (main->presented) {
        snprintf(text, sizeof(text), "%s  /  %u x %u", mode,
                 (unsigned)main->descriptor.header.w, (unsigned)main->descriptor.header.h);
    } else {
        snprintf(text, sizeof(text), "%s FOR VIDEO", mode);
    }
    set_label(s_ui.subtitle, text);
    if (s_ui.stats.frozen) {
        snprintf(text, sizeof(text), "FRAME HELD  /  tap LIVE to resume");
    } else if (!usb->device_connected) {
        snprintf(text, sizeof(text), "USB disconnected  /  %s", s_ui.usb.presented ? "last frame held" : "connect a camera");
    } else if (!usb->streaming) {
        snprintf(text, sizeof(text), "USB %s  /  keeping last good frame",
                 usb->state == USB_CAMERA_STATE_ERROR ? "video error" : "connecting");
    } else if (!face->model_ready) {
        snprintf(text, sizeof(text), "Face AI %s", face->state == FACE_RECOGNITION_STATE_ERROR ? "unavailable" : "loading...");
    } else if (face->enrolling) {
        snprintf(text, sizeof(text), "ADDING PERSON %02u  /  %u of %u samples",
                 (unsigned)face->enroll_target_id, (unsigned)face->enroll_samples_collected,
                 (unsigned)FACE_RECOGNITION_ENROLL_SAMPLES);
    } else if (fresh && face->face_count > 0) {
        const face_recognition_face_t *primary = &face->faces[0];
        for (size_t i = 0; i < face->face_count && i < FACE_RECOGNITION_MAX_FACES; ++i) {
            if (face->faces[i].primary) primary = &face->faces[i];
        }
        if (primary->identity_id) snprintf(text, sizeof(text), "WELCOME BACK, %s  /  %u in view", primary->name, count);
        else snprintf(text, sizeof(text), "%u FACE%s  /  add yourself in FACE GALLERY", count, count == 1 ? "" : "S");
    } else {
        snprintf(text, sizeof(text), "USB FACE AI READY  /  step into view");
    }
    set_label(s_ui.status, text);
    lv_obj_set_style_border_color(s_ui.usb.view, lv_color_hex(usb->streaming ? COLOR_MINT : COLOR_AMBER), 0);
    lv_obj_set_style_border_color(s_ui.board.view, lv_color_hex(board->streaming ? COLOR_MINT : COLOR_AMBER), 0);
    gallery_update(face, usb->streaming && usb->device_connected);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    ui_action_t cmd;
    for (unsigned i = 0; i < 4 && xQueueReceive(s_ui.commands, &cmd, 0) == pdTRUE; ++i) {
        action(cmd);
    }
    poll_frames();
    uint32_t now = lv_tick_get();
    if (s_ui.delete_armed && (int32_t)(now - s_ui.delete_deadline) >= 0) reset_delete();
    if (s_ui.toast_deadline && (int32_t)(now - s_ui.toast_deadline) >= 0) {
        flag_visible(s_ui.toast, false);
        s_ui.toast_deadline = 0;
    }
    usb_camera_stats_t usb = {0};
    onboard_camera_stats_t board = {0};
    face_recognition_snapshot_t face = {0};
    (void)usb_camera_get_stats(&usb);
    (void)onboard_camera_get_stats(&board);
    (void)face_recognition_service_get_snapshot(&face);
    if (now - s_ui.last_hud_ms >= 250U || s_ui.last_hud_ms == 0) {
        hud_update(&usb, &board, &face);
        s_ui.last_hud_ms = now;
    }
    if (now - s_ui.last_faces_ms >= 100U || s_ui.last_faces_ms == 0) {
        face_update(&face, usb.streaming && usb.device_connected);
        s_ui.last_faces_ms = now;
    }
    camera_view_t *views[] = {&s_ui.usb, &s_ui.board};
    for (size_t i = 0; i < 2; ++i) {
        camera_view_t *view = views[i];
        if (view->presented && !s_ui.stats.frozen && now - view->last_frame_ms > 500U && !view->gap_active) {
            view->gap_active = true;
            s_ui.stats.held_gap_count++;
        }
    }
    s_ui.stats.usb_frame_presented = s_ui.usb.presented;
    s_ui.stats.onboard_frame_presented = s_ui.board.presented;
    s_ui.stats.usb_sequence = s_ui.usb_frame.sequence;
    s_ui.stats.onboard_sequence = s_ui.board_frame.sequence;
    s_ui.stats.usb_frame_age_ms = s_ui.usb.presented ? now - s_ui.usb.last_frame_ms : 0;
    s_ui.stats.onboard_frame_age_ms = s_ui.board.presented ? now - s_ui.board.last_frame_ms : 0;
    portENTER_CRITICAL(&s_stats_lock);
    s_published = s_ui.stats;
    portEXIT_CRITICAL(&s_stats_lock);
}

bool camera_lab_ui_command(const char *command)
{
    static const char *names[ACTION_COUNT] = {"swap", "freeze", "clean", "gallery", "add", "cancel", "delete"};
    if (command == NULL || s_ui.commands == NULL) return false;
    for (unsigned i = 0; i < ACTION_COUNT; ++i) {
        if (strcmp(command, names[i]) == 0) {
            ui_action_t cmd = (ui_action_t)i;
            return xQueueSend(s_ui.commands, &cmd, 0) == pdTRUE;
        }
    }
    return false;
}

bool camera_lab_ui_get_stats(camera_lab_ui_stats_t *out_stats)
{
    if (out_stats == NULL) return false;
    portENTER_CRITICAL(&s_stats_lock);
    *out_stats = s_published;
    portEXIT_CRITICAL(&s_stats_lock);
    return out_stats->ready;
}

esp_err_t camera_lab_ui_start(void)
{
    if (s_ui.stats.ready) return ESP_ERR_INVALID_STATE;
    dev_display_lcd_handles_t *lcd = NULL;
    dev_display_lcd_config_t *cfg = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                        (void **)&lcd), TAG, "LCD handle");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                        (void **)&cfg), TAG, "LCD configuration");
    ESP_RETURN_ON_FALSE(lcd != NULL && cfg != NULL, ESP_ERR_INVALID_STATE, TAG, "LCD unavailable");
    ESP_RETURN_ON_FALSE(cfg->lcd_width == SCREEN_W && cfg->lcd_height == SCREEN_H,
                        ESP_ERR_NOT_SUPPORTED, TAG, "Camera Lab requires an 800x480 display");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 16 * 1024;
    port_cfg.task_affinity = 0;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL initialization");
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd->io_handle,
        .panel_handle = lcd->panel_handle,
        .buffer_size = cfg->lcd_width * (SCREEN_H / 2),
        .double_buffer = true,
        .hres = cfg->lcd_width,
        .vres = cfg->lcd_height,
        .monochrome = false,
        .rotation = {.swap_xy = cfg->swap_xy, .mirror_x = cfg->mirror_x, .mirror_y = cfg->mirror_y},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = false, .buff_spiram = true},
    };
    lv_display_t *disp = NULL;
    if (strcmp(cfg->sub_type, "rgb") == 0) {
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
        lvgl_port_display_rgb_cfg_t rgb_cfg = {
            .flags = {
                .bb_mode = cfg->sub_cfg.rgb.panel_config.bounce_buffer_size_px > 0,
                .avoid_tearing = cfg->sub_cfg.rgb.panel_config.num_fbs > 1,
            },
        };
        disp_cfg.flags.direct_mode = rgb_cfg.flags.avoid_tearing;
        disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
        ESP_LOGI(TAG, "RGB display: fb=%u bounce=%u direct=%u",
                 (unsigned)cfg->sub_cfg.rgb.panel_config.num_fbs,
                 (unsigned)cfg->sub_cfg.rgb.panel_config.bounce_buffer_size_px,
                 (unsigned)disp_cfg.flags.direct_mode);
#endif
    } else {
        disp = lvgl_port_add_disp(&disp_cfg);
    }
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "LVGL display unavailable");
#ifdef CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    dev_lcd_touch_handles_t *touch = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, (void **)&touch);
    if (ret == ESP_OK && touch != NULL && touch->touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = touch->touch_handle};
        if (lvgl_port_add_touch(&touch_cfg) == NULL) ESP_LOGW(TAG, "Touch input unavailable");
    } else {
        ESP_LOGW(TAG, "Touch handle unavailable: %s", esp_err_to_name(ret));
    }
#endif
    s_ui.commands = xQueueCreate(12, sizeof(ui_action_t));
    ESP_RETURN_ON_FALSE(s_ui.commands != NULL, ESP_ERR_NO_MEM, TAG, "UI command queue");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(5000), ESP_ERR_TIMEOUT, TAG, "LVGL lock");
    scene_create();
    s_ui.stats.ready = true;
    s_ui.timer = lv_timer_create(tick, 33, NULL);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Camera Lab ready: USB AI + DVP PIP; stable frame leases; 16 KiB LVGL stack");
    return ESP_OK;
}
