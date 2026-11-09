/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* What box_hal reads, as the BOX gives it: the simulator's gravity in each
   pose, and the fingers in a touch controller's report. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../components/boxsim/sim/pose.h"
#include "../components/boxsim/tt21100.h"

static int failed;

static void
check (int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

/* A finger record: its type, its id with the tip and event bits, x, y. */
static void
record (uint8_t *t, int type, int idBits, int x, int y)
{
    memset(t, 0, 10);
    t[0] = type;
    t[1] = idBits;
    t[2] = x & 0xff, t[3] = x >> 8;
    t[4] = y & 0xff, t[5] = y >> 8;
}

int
main (void)
{
    static const struct { const char *pose; float tilt, roll, up[3]; } POSES[] = {
        { "upright, facing you, reads up as +x", 0, 0, { 1, 0, 0 } },
        { "the top tipped away, the screen up", 90, 0, { 0, 0, -1 } },
        { "the top tipped toward you", -90, 0, { 0, 0, 1 } },
        { "turned a quarter clockwise", 0, 90, { 0, -1, 0 } },
        { "turned a quarter anticlockwise", 0, -90, { 0, 1, 0 } },
    };
    for (size_t k = 0; k < sizeof(POSES) / sizeof(POSES[0]); k++)
    {
        float a[3];

        box_sim_gravity(POSES[k].tilt, POSES[k].roll, a);
        check(fabsf(a[0] - POSES[k].up[0]) < 1e-3f && fabsf(a[1] - POSES[k].up[1]) < 1e-3f &&
              fabsf(a[2] - POSES[k].up[2]) < 1e-3f, POSES[k].pose);
    }

    static const struct { const char *what; int type, idBits, kept; } RECORDS[] = {
        { "a finger down is a contact", 0, 0x80 | 5, 1 },
        { "a finger moving is a contact", 0, 0x80 | (2 << 5) | 5, 1 },
        { "a finger's tip off is no contact", 0, 5, 0 },
        { "a finger's lift-off event is no contact", 0, 0x80 | (3 << 5) | 5, 0 },
        { "a pen is no contact", 1, 0x80 | 5, 0 },
    };
    for (size_t k = 0; k < sizeof(RECORDS) / sizeof(RECORDS[0]); k++)
    {
        uint8_t report[17] = { 17, 0, 0, 0, 0, 1 };
        box_hal_contact c[2];

        record(report + 7, RECORDS[k].type, RECORDS[k].idBits, 10, 200);
        const int n = tt21100_contacts(report, sizeof(report), c, 2);
        check(n == RECORDS[k].kept && (!n || (c[0].id == 5 && c[0].x == BOX_HAL_W - 1 - 10 &&
                                               c[0].y == 200)), RECORDS[k].what);
    }

    printf("%d failed\n", failed);
    return failed != 0;
}
