/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * LVGL 演示界面：使用 lv_animimg 控件播放 4 帧眼睛动画。
 */

#include "lvgl.h"
#include "image.h"

/* 图片数组必须是 static，因为 lv_animimg 内部存储了指向该数组的指针，
 * 动画在 example_lvgl_demo_ui 返回后仍会继续运行（由 LVGL 定时器驱动）。 */
static const void *s_eye_images[] = {
    &Page019_EYE,
    &Page020_EYE,
    &Page021_EYE,
    &Page022_EYE,
};

void example_lvgl_demo_ui(lv_disp_t *disp)
{
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    lv_obj_t *anim_img = lv_animimg_create(scr);
    lv_obj_center(anim_img);

    lv_animimg_set_src(anim_img, s_eye_images, 4);
    lv_animimg_set_duration(anim_img, 800);          /* 每帧 200ms × 4 帧 */
    lv_animimg_set_repeat_count(anim_img, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(anim_img);
}
