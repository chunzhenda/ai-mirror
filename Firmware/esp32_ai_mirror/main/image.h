/*
 * Image declarations — LVGL version
 *
 * Each image is 240×240 RGB565 (16-bit) raw pixel data.
 * Uses lv_img_dsc_t for LVGL image display via lv_animimg widget.
 */

#ifndef MY_IMAGE_H
#define MY_IMAGE_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 4-frame eye animation images, 240×240 RGB565 */
extern const lv_img_dsc_t Page019_EYE;
extern const lv_img_dsc_t Page020_EYE;
extern const lv_img_dsc_t Page021_EYE;
extern const lv_img_dsc_t Page022_EYE;

#ifdef __cplusplus
}
#endif

#endif /* MY_IMAGE_H */
