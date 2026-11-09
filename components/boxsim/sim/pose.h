/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
#pragma once
#include <math.h>

/* What the BOX's accelerometer reads at rest, in g, tilted (the top away
   from you) and rolled (clockwise) so many degrees from upright and
   facing you: up is the sensor's +x, the screen faces -z, and turning it
   clockwise brings its -y up. */
static inline void
box_sim_gravity (float tilt, float roll, float accel[3])
{
    const float t = tilt * (float)M_PI / 180, r = roll * (float)M_PI / 180;

    accel[2] = -sinf(t);
    accel[1] = -sinf(r);
    accel[0] = sqrtf(fmaxf(0, 1 - accel[1] * accel[1] - accel[2] * accel[2]));
}
