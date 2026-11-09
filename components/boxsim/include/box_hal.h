/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* The Espressif ESP32-S3-BOX's hardware as a program plays it: the LCD,
   the speaker, the touchscreen and its red button, the side button and the
   IMU. box_hal_esp32.c is the BOX's; box_hal_sim.c stands in for it, a window
   on a desktop, under the same calls. Taps see what is drawn and played
   and can press as a hand would, the program none the wiser. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOX_HAL_W 320
#define BOX_HAL_H 240

/* What box_hal_open is to open, and what it did. */
enum { BOX_HAL_SPEAKER = 1, BOX_HAL_SCREEN = 2, BOX_HAL_IMU = 4, BOX_HAL_TOUCH = 8,
       BOX_HAL_SIDE_BUTTON = 16 };
enum { BOX_HAL_SIDE, BOX_HAL_HOME };

/* A finger on the screen: the controller's id for it, and where, in the
   screen's pixels. */
typedef struct
{
    int id, x, y;
} box_hal_contact;

/* Each may be NULL. */
typedef struct
{
    /* After the program draws, or writes to the speaker. */
    void (*drawn) (int x0, int y0, int x1, int y1, const uint16_t *pixels);
    void (*written) (const int16_t *frames, int count);
    /* A finger down at (x, y), in place of the screen's. */
    bool (*finger) (int *x, int *y);
    /* A button held, besides the real one. */
    bool (*held) (int button);
    /* A jolt of more than 2 g, as a shake gives the IMU. */
    bool (*jolt) (void);
} box_hal_taps;

/* What of `want' opened; called again for more. The speaker plays stereo
   16-bit frames at `rate', at the codec's `volume' (0 to 100). In QEMU the
   screen never answers, and opening it never returns. */
unsigned box_hal_open (unsigned want, int rate, int volume);
void box_hal_tap (const box_hal_taps *taps);

/* Pixels big-endian RGB565, as the panel takes them, x1 and y1 past the
   end. Queued only: the pixels are being sent until the next draw, so a
   buffer is not to be filled again until then. */
void box_lcd_draw (int x0, int y0, int x1, int y1, const uint16_t *pixels);
/* Waits while the codec's buffer is full, where there is a speaker. */
void box_audio_write (const int16_t *frames, int count);
/* The fingers on the screen, up to `most'. */
int box_touch_read (box_hal_contact *out, int most);
bool box_button_down (int button);
/* In g and degrees a second, the sensor's axes; false if unread. A tap's
   jolt reads even with no IMU, as the BOX upright. */
bool box_imu_read (float accel[3], float gyro[3]);

#ifdef __cplusplus
}
#endif
