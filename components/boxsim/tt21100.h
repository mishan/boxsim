/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* The fingers in a TT21100 touch report of n bytes: ten-byte records
   after a seven-byte header. */
#pragma once
#include <stdint.h>

#include "box_hal.h"

static inline int
tt21100_contacts (const uint8_t *report, int n, box_hal_contact *out, int most)
{
    int count = 0;

    for (int r = 0; r < (report[5] & 0x1f) && 7 + 10 * (r + 1) <= n; r++)
    {
        const uint8_t *t = report + 7 + 10 * r;

        if ((t[0] & 0x07) != 0 || count >= most)    /* a finger, not a pen */
            continue;
        /* A finger's last record says it has lifted: its tip off, or its
           event 3. */
        if (!(t[1] & 0x80) || ((t[1] >> 5) & 3) == 3)
            continue;
        out[count].id = t[1] & 0x1f;
        /* Mirrored across the columns, as the screen is drawn. */
        out[count].x = BOX_HAL_W - 1 - (t[2] | (t[3] << 8));
        out[count].y = t[4] | (t[5] << 8);
        count++;
    }
    return count;
}
