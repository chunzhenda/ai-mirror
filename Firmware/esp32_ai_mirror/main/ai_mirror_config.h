/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "sdkconfig.h"

/* AI Mirror hardware/audio configuration */
#define AI_MIRROR_RECV_BUF_SIZE   (2400)
#define AI_MIRROR_SAMPLE_RATE     (16000)
#define AI_MIRROR_MCLK_MULTIPLE   (384) // If not using 24-bit data width, 256 should be enough
#define AI_MIRROR_MCLK_FREQ_HZ    (AI_MIRROR_SAMPLE_RATE * AI_MIRROR_MCLK_MULTIPLE)
#define AI_MIRROR_VOICE_VOLUME    CONFIG_AI_MIRROR_VOICE_VOLUME
#if CONFIG_AI_MIRROR_AUDIO_MODE_ECHO
#define AI_MIRROR_MIC_GAIN        CONFIG_AI_MIRROR_MIC_GAIN
#endif


#if !defined(CONFIG_AI_MIRROR_BSP)

/* I2C port and GPIOs */
#define I2C_NUM         (0)
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
#define I2C_SCL_IO      (GPIO_NUM_9)
#define I2C_SDA_IO      (GPIO_NUM_10)
#elif CONFIG_IDF_TARGET_ESP32H2
#define I2C_SCL_IO      (GPIO_NUM_8)
#define I2C_SDA_IO      (GPIO_NUM_9)
#else
#define I2C_SCL_IO      (GPIO_NUM_6)
#define I2C_SDA_IO      (GPIO_NUM_7)
#endif

/* I2S port and GPIOs */
#define I2S_NUM         (0)
#define I2S_MCK_IO      (GPIO_NUM_6)
#define I2S_BCK_IO      (GPIO_NUM_7)
#define I2S_WS_IO       (GPIO_NUM_8)
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
#define I2S_DO_IO       (GPIO_NUM_11)
#define I2S_DI_IO       (GPIO_NUM_12)
#else
#define I2S_DO_IO       (GPIO_NUM_2)
#define I2S_DI_IO       (GPIO_NUM_3)
#endif


#else // CONFIG_AI_MIRROR_BSP
#include "bsp/esp-bsp.h"
#define I2C_NUM BSP_I2C_NUM

#endif // CONFIG_AI_MIRROR_BSP
