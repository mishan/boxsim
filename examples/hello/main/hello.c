/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* hello: eight color bars, a white dot wherever the screen is touched
   and an 880 Hz beep while it is, the red button clearing the dots, and
   along the bottom a level, its black mark following the BOX's roll.
   The same on the BOX and in boxsim's window. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "box_hal.h"
#include "debugcon.h"

#define W BOX_HAL_W
#define H BOX_HAL_H
#define BLOCK 256
#define BARS_H 200                      /* the bars; the level below */
#define DOT 8

static const uint16_t BARS[8] = { 0xf800, 0x07e0, 0x001f, 0xffff,
                                  0xffe0, 0x07ff, 0xf81f, 0x0000 };

static volatile bool beeping = false;
static int rate = 16000;
static uint16_t *buf;                   /* W * DOT, one strip of the screen */

/* The panel takes RGB565 big-endian. */
static uint16_t
be (uint16_t c)
{
    return (uint16_t)(c >> 8 | c << 8);
}

/* Each draw waits for the last to be sent before the buffer is reused:
   one at a time, on the one buffer, from the one task. */
static void
fill (int x0, int y0, int x1, int y1, uint16_t (*color) (int x, int y))
{
    for (int y = y0; y < y1; y += DOT)
    {
        const int y2 = y + DOT < y1 ? y + DOT : y1;

        for (int j = y; j < y2; j++)
            for (int x = x0; x < x1; x++)
                buf[(j - y) * (x1 - x0) + x - x0] = be(color(x, j));
        box_lcd_draw(x0, y, x1, y2, buf);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static uint16_t bar (int x, int y) { return BARS[x / (W / 8)]; }
static uint16_t white (int x, int y) { return 0xffff; }

static int mark;
static uint16_t level (int x, int y) { return x >= mark - 4 && x < mark + 4 ? 0 : 0x8410; }

static void
audioTask (void *arg)
{
    static int16_t frames[2 * BLOCK];
    float phase = 0;

    for (;;)
    {
        for (int i = 0; i < BLOCK; i++)
        {
            const int16_t v = beeping ? (int16_t)(8000 * sinf(phase)) : 0;

            frames[2 * i] = frames[2 * i + 1] = v;
            phase = fmodf(phase + 2 * (float)M_PI * 880 / rate, 2 * (float)M_PI);
        }
        box_audio_write(frames, BLOCK);
    }
}

void
app_main (void)
{
    /* For the tests, in the simulator: the BOX has no environment. */
    if (getenv("HELLO_RATE"))
        rate = atoi(getenv("HELLO_RATE"));
    debugcon_start(rate, DEBUGCON_TOUCH | DEBUGCON_HOME, NULL);

    const unsigned opened = box_hal_open(BOX_HAL_SPEAKER | BOX_HAL_SCREEN | BOX_HAL_TOUCH |
                                         BOX_HAL_IMU, rate, 60);

    buf = heap_caps_malloc(W * DOT * 2, MALLOC_CAP_DMA);
    printf("hello: opened %#x\n", opened);
    if (opened & BOX_HAL_SPEAKER)
        xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 10, NULL, 1);
    fill(0, 0, W, BARS_H, bar);
    mark = -100;
    for (bool home = false;; vTaskDelay(pdMS_TO_TICKS(10)))
    {
        box_hal_contact at;
        float accel[3] = { -1, 0, 0 }, gyro[3];
        const bool touched = box_touch_read(&at, 1) > 0;
        const bool clear = box_button_down(BOX_HAL_HOME) && !home;

        home = box_button_down(BOX_HAL_HOME);
        beeping = touched;
        if (clear)
            fill(0, 0, W, BARS_H, bar);
        if (touched && at.y < BARS_H)
        {
            const int x = at.x - DOT / 2 < 0 ? 0 : at.x - DOT / 2 > W - DOT ? W - DOT : at.x - DOT / 2;
            const int y = at.y - DOT / 2 < 0 ? 0 : at.y - DOT / 2 > BARS_H - DOT ? BARS_H - DOT
                                                                                 : at.y - DOT / 2;

            fill(x, y, x + DOT, y + DOT, white);
        }
        box_imu_read(accel, gyro);

        const int m = W / 2 - (int)(accel[1] * (W / 2 - 8));

        if (m != mark)
        {
            mark = m;
            fill(0, BARS_H, W, H, level);
        }
    }
}
