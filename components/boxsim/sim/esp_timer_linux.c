/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* ESP-IDF's linux target has esp_timer.h but nothing behind it: the time
   since the program started, in microseconds, as the BOX counts it since
   boot. */
#include <time.h>

#include "esp_timer.h"

static int64_t
now (void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static int64_t start;

/* Ahead of the simulator's own constructor, whose thread reads the time. */
__attribute__((constructor(101))) static void
started (void)
{
    start = now();
}

int64_t
esp_timer_get_time (void)
{
    return now() - start;
}
