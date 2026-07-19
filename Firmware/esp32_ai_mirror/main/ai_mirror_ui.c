/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * AI Mirror LVGL face animation and provisioning QR UI for the round 240x240 display.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lvgl.h"

#define FACE_SIZE 240
#define SCENE_MS 2600
#define FRAME_MS 33

typedef enum {
    FACE_SCENE_IDLE = 0,
    FACE_SCENE_HAPPY,
    FACE_SCENE_SAD,
    FACE_SCENE_ANGRY,
    FACE_SCENE_LISTENING,
    FACE_SCENE_BLINK,
    FACE_SCENE_MAX,
} face_scene_t;

typedef struct {
    lv_obj_t *face;
    lv_obj_t *eye_l;
    lv_obj_t *eye_r;
    lv_obj_t *shine_l;
    lv_obj_t *shine_r;
    lv_obj_t *cheek_l;
    lv_obj_t *cheek_r;
    lv_obj_t *mouth;
    lv_obj_t *brow_l;
    lv_obj_t *brow_r;
    lv_obj_t *tear_l;
    lv_obj_t *tear_r;
    lv_obj_t *listen_l1;
    lv_obj_t *listen_l2;
    lv_obj_t *listen_r1;
    lv_obj_t *listen_r2;
    lv_obj_t *spark_l;
    lv_obj_t *spark_r;
    lv_obj_t *spark_top;
    uint32_t tick_ms;
    face_scene_t scene;
} cute_face_t;

static cute_face_t s_face;

static int32_t ease_smooth_u10(uint32_t t)
{
    if (t > 1024) {
        t = 1024;
    }

    /* Smoothstep: 3t^2 - 2t^3. Keep it integer-only to avoid float work in the LVGL refresh path. */
    uint32_t t2 = (t * t) / 1024;
    uint32_t t3 = (t2 * t) / 1024;
    return (int32_t)((3 * t2) - (2 * t3));
}

static int16_t ease_lerp_i16(int16_t from, int16_t to, uint32_t elapsed, uint32_t duration)
{
    if (duration == 0 || elapsed >= duration) {
        return to;
    }

    int32_t eased = ease_smooth_u10((elapsed * 1024) / duration);
    return (int16_t)(from + (((int32_t)to - from) * eased) / 1024);
}

static int32_t tri_wave(uint32_t tick, uint32_t period, int32_t amplitude)
{
    uint32_t phase = tick % period;
    uint32_t half = period / 2;

    if (half == 0) {
        return 0;
    }

    if (phase < half) {
        int32_t eased = ease_smooth_u10((phase * 1024) / half);
        return -amplitude + (2 * amplitude * eased) / 1024;
    }

    int32_t eased = ease_smooth_u10(((phase - half) * 1024) / half);
    return amplitude - (2 * amplitude * eased) / 1024;
}

static uint8_t wave_opa(uint32_t tick, uint32_t period, uint8_t low, uint8_t high)
{
    int32_t mid = (low + high) / 2;
    int32_t amp = (high - low) / 2;
    return (uint8_t)(mid + tri_wave(tick, period, amp));
}

static void hide_obj(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *round_obj(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                           uint32_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void set_eye(lv_obj_t *eye, lv_obj_t *shine, int16_t x, int16_t y,
                    int16_t w, int16_t h, int16_t look_x, int16_t look_y)
{
    lv_obj_set_size(eye, w, h);
    lv_obj_set_pos(eye, x + look_x, y + (44 - h) / 2 + look_y);

    if (h > 12) {
        lv_obj_set_size(shine, 10, 10);
        lv_obj_set_pos(shine, x + look_x + 10, y + look_y + 8);
        lv_obj_set_style_bg_opa(shine, LV_OPA_90, 0);
    } else {
        lv_obj_set_style_bg_opa(shine, LV_OPA_TRANSP, 0);
    }
}

static void set_mouth(cute_face_t *ui, int16_t x, int16_t y, int16_t w, int16_t h,
                      uint32_t color, lv_opa_t opa)
{
    lv_obj_set_size(ui->mouth, w, h);
    lv_obj_set_pos(ui->mouth, x, y);
    lv_obj_set_style_bg_color(ui->mouth, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(ui->mouth, opa, 0);
}

static void set_mouth_centered(cute_face_t *ui, int16_t center_x, int16_t y,
                               int16_t w, int16_t h, uint32_t color, lv_opa_t opa)
{
    if (w < 8) {
        w = 8;
    }
    if (h < 5) {
        h = 5;
    }

    set_mouth(ui, center_x - w / 2, y, w, h, color, opa);
}

static int16_t idle_blink_height(uint32_t tick)
{
    uint32_t phase = tick % 3800;

    if (phase > 3020 && phase < 3180) {
        uint32_t blink = phase - 3020;
        if (blink < 80) {
            return ease_lerp_i16(44, 8, blink, 80);
        }
        return ease_lerp_i16(8, 44, blink - 80, 80);
    }
    return 44;
}

static int16_t fast_blink_height(uint32_t tick)
{
    uint32_t phase = tick % 720;

    if (phase < 120) {
        return ease_lerp_i16(44, 7, phase, 120);
    }
    if (phase < 220) {
        return ease_lerp_i16(7, 44, phase - 120, 100);
    }
    if (phase > 520 && phase < 650) {
        return ease_lerp_i16(44, 7, phase - 520, 130);
    }
    return 44;
}

static void set_scene_static(cute_face_t *ui, face_scene_t scene)
{
    hide_obj(ui->brow_l, scene != FACE_SCENE_ANGRY);
    hide_obj(ui->brow_r, scene != FACE_SCENE_ANGRY);
    hide_obj(ui->tear_l, scene != FACE_SCENE_SAD);
    hide_obj(ui->tear_r, scene != FACE_SCENE_SAD);
    hide_obj(ui->listen_l1, scene != FACE_SCENE_LISTENING);
    hide_obj(ui->listen_l2, scene != FACE_SCENE_LISTENING);
    hide_obj(ui->listen_r1, scene != FACE_SCENE_LISTENING);
    hide_obj(ui->listen_r2, scene != FACE_SCENE_LISTENING);

    switch (scene) {
    case FACE_SCENE_IDLE:
        lv_obj_set_style_bg_color(ui->face, lv_color_hex(0xFFF1BC), 0);
        lv_obj_set_style_border_color(ui->face, lv_color_hex(0xFFD47D), 0);
        break;
    case FACE_SCENE_HAPPY:
        lv_obj_set_style_bg_color(ui->face, lv_color_hex(0xFFECA8), 0);
        lv_obj_set_style_border_color(ui->face, lv_color_hex(0xFFC966), 0);
        break;
    case FACE_SCENE_SAD:
        lv_obj_set_style_bg_color(ui->face, lv_color_hex(0xD8E9FF), 0);
        lv_obj_set_style_border_color(ui->face, lv_color_hex(0x92C7FF), 0);
        break;
    case FACE_SCENE_ANGRY:
        lv_obj_set_style_bg_color(ui->face, lv_color_hex(0xFFC7A6), 0);
        lv_obj_set_style_border_color(ui->face, lv_color_hex(0xFF805F), 0);
        break;
    case FACE_SCENE_LISTENING:
        lv_obj_set_style_bg_color(ui->face, lv_color_hex(0xDFF8FF), 0);
        lv_obj_set_style_border_color(ui->face, lv_color_hex(0x74DFFF), 0);
        break;
    case FACE_SCENE_BLINK:
        lv_obj_set_style_bg_color(ui->face, lv_color_hex(0xF6E7FF), 0);
        lv_obj_set_style_border_color(ui->face, lv_color_hex(0xD9ACFF), 0);
        break;
    default:
        break;
    }
}

static void draw_scene(cute_face_t *ui, face_scene_t scene, uint32_t local_tick)
{
    int16_t bob = tri_wave(local_tick, 1800, 4);
    int16_t look_x = tri_wave(local_tick + 400, 2400, 5);
    int16_t look_y = tri_wave(local_tick + 900, 3000, 3);

    lv_obj_set_pos(ui->spark_top, 114 + tri_wave(local_tick, 2100, 5), 12 + tri_wave(local_tick + 300, 1900, 4));
    lv_obj_set_pos(ui->spark_l, 21 + tri_wave(local_tick, 2400, 4), 56 + tri_wave(local_tick + 400, 2200, 5));
    lv_obj_set_pos(ui->spark_r, 205 + tri_wave(local_tick + 800, 2800, 4), 62 + tri_wave(local_tick, 2300, 4));

    switch (scene) {
    case FACE_SCENE_IDLE:
        set_eye(ui->eye_l, ui->shine_l, 50, 78 + bob, 44, idle_blink_height(local_tick), look_x, look_y);
        set_eye(ui->eye_r, ui->shine_r, 146, 78 + bob, 44, idle_blink_height(local_tick), look_x, look_y);
        set_mouth_centered(ui, 120 + look_x / 3, 152 + bob / 2, 48 + tri_wave(local_tick + 600, 1800, 5), 9 + tri_wave(local_tick + 200, 1600, 2), 0x6A4864, LV_OPA_COVER);
        lv_obj_set_style_bg_opa(ui->cheek_l, 62, 0);
        lv_obj_set_style_bg_opa(ui->cheek_r, 62, 0);
        break;
    case FACE_SCENE_HAPPY:
        set_eye(ui->eye_l, ui->shine_l, 47, 72 + bob, 50, 48, look_x / 2, look_y / 2);
        set_eye(ui->eye_r, ui->shine_r, 143, 72 + bob, 50, 48, look_x / 2, look_y / 2);
        set_mouth_centered(ui, 120 + tri_wave(local_tick, 1200, 3), 153 + bob / 2, 82 + tri_wave(local_tick, 900, 8), 23 + tri_wave(local_tick + 220, 700, 5), 0xF06C8E, LV_OPA_COVER);
        lv_obj_set_style_bg_opa(ui->cheek_l, wave_opa(local_tick, 900, 74, 102), 0);
        lv_obj_set_style_bg_opa(ui->cheek_r, wave_opa(local_tick + 200, 900, 74, 102), 0);
        break;
    case FACE_SCENE_SAD:
        set_eye(ui->eye_l, ui->shine_l, 52, 86 + bob, 40, 34, -2, 4);
        set_eye(ui->eye_r, ui->shine_r, 148, 86 + bob, 40, 34, -2, 4);
        set_mouth_centered(ui, 120 - 1, 164 + bob / 3, 40 + tri_wave(local_tick + 300, 1700, 4), 8 + tri_wave(local_tick + 700, 1600, 2), 0x5B6278, LV_OPA_COVER);
        lv_obj_set_style_bg_opa(ui->cheek_l, 36, 0);
        lv_obj_set_style_bg_opa(ui->cheek_r, 36, 0);
        lv_obj_set_pos(ui->tear_l, 88, 119 + tri_wave(local_tick, 1500, 8));
        lv_obj_set_pos(ui->tear_r, 146, 126 + tri_wave(local_tick + 400, 1600, 7));
        break;
    case FACE_SCENE_ANGRY:
        set_eye(ui->eye_l, ui->shine_l, 49, 88, 47, 22, -2, 0);
        set_eye(ui->eye_r, ui->shine_r, 144, 88, 47, 22, 2, 0);
        set_mouth_centered(ui, 120 + tri_wave(local_tick, 260, 2), 158 + tri_wave(local_tick + 80, 320, 1), 50 + tri_wave(local_tick + 100, 500, 3), 9, 0x71352F, LV_OPA_COVER);
        lv_obj_set_pos(ui->brow_l, 45, 63 + tri_wave(local_tick, 900, 2));
        lv_obj_set_pos(ui->brow_r, 144, 63 + tri_wave(local_tick + 200, 900, 2));
        lv_obj_set_style_bg_opa(ui->cheek_l, 22, 0);
        lv_obj_set_style_bg_opa(ui->cheek_r, 22, 0);
        break;
    case FACE_SCENE_LISTENING:
        set_eye(ui->eye_l, ui->shine_l, 45, 73 + bob, 54, 54, look_x / 2, look_y / 2);
        set_eye(ui->eye_r, ui->shine_r, 141, 73 + bob, 54, 54, look_x / 2, look_y / 2);
        set_mouth_centered(ui, 120, 158 + tri_wave(local_tick + 160, 900, 2), 30 + tri_wave(local_tick, 620, 7), 13 + tri_wave(local_tick + 80, 620, 4), 0x356273, LV_OPA_COVER);
        lv_obj_set_style_bg_opa(ui->cheek_l, 54, 0);
        lv_obj_set_style_bg_opa(ui->cheek_r, 54, 0);
        lv_obj_set_size(ui->listen_l1, 10, 18 + tri_wave(local_tick, 660, 8));
        lv_obj_set_size(ui->listen_l2, 8, 12 + tri_wave(local_tick + 180, 660, 7));
        lv_obj_set_size(ui->listen_r1, 10, 18 + tri_wave(local_tick + 330, 660, 8));
        lv_obj_set_size(ui->listen_r2, 8, 12 + tri_wave(local_tick + 510, 660, 7));
        lv_obj_set_pos(ui->listen_l1, 21, 105 - lv_obj_get_height(ui->listen_l1) / 2);
        lv_obj_set_pos(ui->listen_l2, 36, 105 - lv_obj_get_height(ui->listen_l2) / 2);
        lv_obj_set_pos(ui->listen_r1, 209, 105 - lv_obj_get_height(ui->listen_r1) / 2);
        lv_obj_set_pos(ui->listen_r2, 196, 105 - lv_obj_get_height(ui->listen_r2) / 2);
        break;
    case FACE_SCENE_BLINK:
        set_eye(ui->eye_l, ui->shine_l, 50, 80 + bob, 46, fast_blink_height(local_tick), 0, 0);
        set_eye(ui->eye_r, ui->shine_r, 144, 80 + bob, 46, fast_blink_height(local_tick), 0, 0);
        set_mouth_centered(ui, 120, 154 + bob / 2, 56 + tri_wave(local_tick + 200, 1000, 5), 14 + tri_wave(local_tick, 900, 3), 0x765284, LV_OPA_COVER);
        lv_obj_set_style_bg_opa(ui->cheek_l, wave_opa(local_tick, 800, 58, 88), 0);
        lv_obj_set_style_bg_opa(ui->cheek_r, wave_opa(local_tick + 180, 800, 58, 88), 0);
        break;
    default:
        break;
    }
}

static void face_timer_cb(lv_timer_t *timer)
{
    cute_face_t *ui = (cute_face_t *)timer->user_data;
    ui->tick_ms += FRAME_MS;

    face_scene_t scene = (face_scene_t)((ui->tick_ms / SCENE_MS) % FACE_SCENE_MAX);
    uint32_t local_tick = ui->tick_ms % SCENE_MS;

    if (scene != ui->scene) {
        ui->scene = scene;
        set_scene_static(ui, scene);
    }

    draw_scene(ui, scene, local_tick);
}

void ai_mirror_ui_show_face_demo(lv_disp_t *disp)
{
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x161423), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_face = (cute_face_t){0};
    s_face.scene = FACE_SCENE_MAX;

    s_face.face = round_obj(scr, FACE_SIZE, FACE_SIZE, 0xFFF1BC, LV_OPA_COVER);
    lv_obj_set_pos(s_face.face, 0, 0);
    lv_obj_set_style_border_width(s_face.face, 4, 0);
    lv_obj_set_style_border_color(s_face.face, lv_color_hex(0xFFD47D), 0);

    s_face.spark_top = round_obj(s_face.face, 9, 9, 0xFFFFFF, LV_OPA_70);
    s_face.spark_l = round_obj(s_face.face, 12, 12, 0xB8F7FF, LV_OPA_70);
    s_face.spark_r = round_obj(s_face.face, 11, 11, 0xFFE7A8, LV_OPA_80);

    s_face.cheek_l = round_obj(s_face.face, 42, 18, 0xFF8FB2, LV_OPA_60);
    s_face.cheek_r = round_obj(s_face.face, 42, 18, 0xFF8FB2, LV_OPA_60);
    lv_obj_set_pos(s_face.cheek_l, 27, 126);
    lv_obj_set_pos(s_face.cheek_r, 171, 126);

    s_face.eye_l = round_obj(s_face.face, 44, 44, 0x2A2438, LV_OPA_COVER);
    s_face.eye_r = round_obj(s_face.face, 44, 44, 0x2A2438, LV_OPA_COVER);
    s_face.shine_l = round_obj(s_face.face, 10, 10, 0xFFFFFF, LV_OPA_90);
    s_face.shine_r = round_obj(s_face.face, 10, 10, 0xFFFFFF, LV_OPA_90);

    s_face.mouth = round_obj(s_face.face, 48, 10, 0x6A4864, LV_OPA_COVER);

    s_face.brow_l = round_obj(s_face.face, 54, 10, 0x71352F, LV_OPA_COVER);
    s_face.brow_r = round_obj(s_face.face, 54, 10, 0x71352F, LV_OPA_COVER);

    s_face.tear_l = round_obj(s_face.face, 10, 22, 0x59BFFF, LV_OPA_80);
    s_face.tear_r = round_obj(s_face.face, 9, 19, 0x59BFFF, LV_OPA_70);

    s_face.listen_l1 = round_obj(s_face.face, 10, 22, 0x31C7FF, LV_OPA_80);
    s_face.listen_l2 = round_obj(s_face.face, 8, 16, 0x80E8FF, LV_OPA_80);
    s_face.listen_r1 = round_obj(s_face.face, 10, 22, 0x31C7FF, LV_OPA_80);
    s_face.listen_r2 = round_obj(s_face.face, 8, 16, 0x80E8FF, LV_OPA_80);

    set_scene_static(&s_face, FACE_SCENE_IDLE);
    draw_scene(&s_face, FACE_SCENE_IDLE, 0);
    lv_timer_create(face_timer_cb, FRAME_MS, &s_face);
}
void ai_mirror_ui_show_prov_qr(lv_disp_t *disp, const char *payload, const char *service_name, const char *transport)
{
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF8FBFF), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#if LV_USE_QRCODE
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SCAN TO PROVISION");
    lv_obj_set_style_text_color(title, lv_color_hex(0x24344D), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *qr = lv_qrcode_create(scr, 168, lv_color_hex(0x111827), lv_color_hex(0xFFFFFF));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, 7);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(qr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(qr, LV_OPA_COVER, 0);

    if (payload && payload[0]) {
        lv_qrcode_update(qr, payload, strlen(payload));
    }

    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text_fmt(info, "%s  %s\nBTN2 BACK", transport ? transport : "ble", service_name ? service_name : "PROV");
    lv_obj_set_width(info, 210);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0x42526B), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -12);
#else
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "QR disabled\nEnable CONFIG_LV_USE_QRCODE");
    lv_obj_set_style_text_color(label, lv_color_hex(0x24344D), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
#endif
}
static lv_obj_t *make_center_label(lv_obj_t *parent, const char *text, int16_t y, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_width(label, 210);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

void ai_mirror_ui_show_message(lv_disp_t *disp, const char *title, const char *line1,
                               const char *line2, uint32_t bg_color, uint32_t accent_color)
{
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *halo = round_obj(scr, 214, 214, 0xFFFFFF, LV_OPA_30);
    lv_obj_center(halo);

    lv_obj_t *dot = round_obj(scr, 38, 38, accent_color, LV_OPA_COVER);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 24);

    make_center_label(scr, title, -42, 0x24344D);
    make_center_label(scr, line1, -4, 0x42526B);
    make_center_label(scr, line2, 34, 0x667085);
}

void ai_mirror_ui_show_prov_select(lv_disp_t *disp, const char *selected_transport)
{
    bool softap = selected_transport && strcmp(selected_transport, "softap") == 0;
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF8FBFF), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    make_center_label(scr, "WiFi Provision", -82, 0x24344D);
    make_center_label(scr, "BTN2 switch", 66, 0x667085);
    make_center_label(scr, "BTN1 confirm", 90, 0x667085);

    lv_obj_t *ap = round_obj(scr, 92, 58, softap ? 0x2FD681 : 0xE7EEF8, LV_OPA_COVER);
    lv_obj_set_pos(ap, 22, 91);
    lv_obj_t *ble = round_obj(scr, 92, 58, !softap ? 0x6EA8FF : 0xE7EEF8, LV_OPA_COVER);
    lv_obj_set_pos(ble, 126, 91);

    lv_obj_t *ap_label = lv_label_create(ap);
    lv_label_set_text(ap_label, "AP");
    lv_obj_set_style_text_color(ap_label, lv_color_hex(softap ? 0xFFFFFF : 0x42526B), 0);
    lv_obj_set_style_text_font(ap_label, &lv_font_montserrat_14, 0);
    lv_obj_center(ap_label);

    lv_obj_t *ble_label = lv_label_create(ble);
    lv_label_set_text(ble_label, "BLE");
    lv_obj_set_style_text_color(ble_label, lv_color_hex(!softap ? 0xFFFFFF : 0x42526B), 0);
    lv_obj_set_style_text_font(ble_label, &lv_font_montserrat_14, 0);
    lv_obj_center(ble_label);
}

void ai_mirror_ui_show_internet_warning(lv_disp_t *disp, bool retry_selected)
{
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFF7ED), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    make_center_label(scr, "Internet warning", -84, 0x7C2D12);
    make_center_label(scr, "WiFi connected", -46, 0x9A3412);
    make_center_label(scr, "but internet test failed", -20, 0x9A3412);

    lv_obj_t *retry = round_obj(scr, 92, 48, retry_selected ? 0xF97316 : 0xFFE7D1, LV_OPA_COVER);
    lv_obj_set_pos(retry, 22, 135);
    lv_obj_t *cont = round_obj(scr, 92, 48, !retry_selected ? 0x475467 : 0xFFE7D1, LV_OPA_COVER);
    lv_obj_set_pos(cont, 126, 135);

    lv_obj_t *retry_label = lv_label_create(retry);
    lv_label_set_text(retry_label, "Retry");
    lv_obj_set_style_text_color(retry_label, lv_color_hex(retry_selected ? 0xFFFFFF : 0x9A3412), 0);
    lv_obj_set_style_text_font(retry_label, &lv_font_montserrat_14, 0);
    lv_obj_center(retry_label);

    lv_obj_t *cont_label = lv_label_create(cont);
    lv_label_set_text(cont_label, "Continue");
    lv_obj_set_style_text_color(cont_label, lv_color_hex(!retry_selected ? 0xFFFFFF : 0x9A3412), 0);
    lv_obj_set_style_text_font(cont_label, &lv_font_montserrat_14, 0);
    lv_obj_center(cont_label);

    make_center_label(scr, "BTN2 switch  BTN1 ok", 91, 0x9A3412);
}