/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* A stand-in for the BOX's console, for test_boxctl.py: its arguments in
   order, each written to stdout as debugcon.c would write it, the frames
   by debugframe.h.
     log TEXT      a line of log
     half TEXT     the start of a line, its end to come after the next frame
     shot          a screenshot, pixel (x, y) the color (x, y, x ^ y)
     audio N GAP   blocks 0 to N - 1 of a ramp, block GAP not sent
     reply TEXT    a command's reply
     corrupt K     the Kth frame from here, 0 the next, a byte of its
                   payload changed
     cut K         the Kth frame from here cut off halfway */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../components/boxsim/include/debugframe.h"

#define W 320
#define H 240

static int corrupt = -1, cut = -1;
static const char *rest = NULL;

static void
frame (char type, uint32_t seq, const void *payload, size_t len)
{
    static uint8_t out[DEBUGFRAME_HEAD + DEBUGFRAME_MOST + 4];
    size_t n = debugframe_encode(out, type, seq, payload, len);

    if (corrupt-- == 0)
        out[DEBUGFRAME_HEAD + len / 2] ^= 0x10;
    if (cut-- == 0)
        n /= 2;
    fwrite(out, 1, n, stdout);
    if (rest)
        printf("%s\r\n", rest);
    rest = NULL;
}

static uint16_t
rgb (int r, int g, int b)
{
    const uint16_t c = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));

    return (uint16_t)((c >> 8) | (c << 8));
}

int
main (int argc, char **argv)
{
    static uint16_t screen[W * H];

    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "log"))
            printf("%s\r\n", argv[++i]);
        else if (!strcmp(argv[i], "half"))
        {
            const char *s = argv[++i];
            const int k = (int)strlen(s) / 2;

            printf("%.*s", k, s);
            rest = s + k;
        }
        else if (!strcmp(argv[i], "reply"))
        {
            frame('T', 0, argv[i + 1], strlen(argv[i + 1]));
            i++;
        }
        else if (!strcmp(argv[i], "corrupt"))
            corrupt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "cut"))
            cut = atoi(argv[++i]);
        else if (!strcmp(argv[i], "shot"))
        {
            uint8_t info[20] = { W & 0xff, W >> 8, H & 0xff, H >> 8 }, piece[4 + 1024];
            const uint8_t *bytes = (const uint8_t *)screen;

            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    screen[y * W + x] = rgb(x, y, (x ^ y) & 0xff);
            debugframe_put32(info + 4, sizeof(screen));
            debugframe_put32(info + 8, debugframe_crc(0, bytes, sizeof(screen)));
            memcpy(info + 12, "rgb565be", 8);
            frame('S', 0, info, sizeof(info));
            for (uint32_t at = 0; at < sizeof(screen); at += 1024)
            {
                debugframe_put32(piece, at);
                memcpy(piece + 4, bytes + at, 1024);
                frame('P', at / 1024, piece, sizeof(piece));
            }
        }
        else if (!strcmp(argv[i], "audio"))
        {
            const int n = atoi(argv[i + 1]), gap = atoi(argv[i + 2]);
            uint8_t block[4 + 512];

            i += 2;
            for (int s = 0; s < n; s++)
            {
                debugframe_put32(block, 32000);
                for (int k = 0; k < 256; k++)
                {
                    const int16_t v = (int16_t)(s * 256 + k);

                    memcpy(block + 4 + 2 * k, &v, 2);
                }
                if (s != gap)
                    frame('A', s, block, sizeof(block));
            }
        }
    return 0;
}
