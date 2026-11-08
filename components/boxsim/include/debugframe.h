/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* A debug console frame, as debugcon writes them among the log's text and
   boxctl.py picks them out of it. No ESP-IDF in it, so a desktop program
   can write the same frames to test boxctl.py with.

     0x1e 'T' 'B'  type  seq (u32)  len (u16)  payload (len)  crc32 (u32)

   little-endian, the CRC zlib's, over type to the payload's end. Log text
   has no 0x1e in it; a frame whose CRC fails is read as text. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEBUGFRAME_HEAD 10
#define DEBUGFRAME_MOST 1100            /* a payload's longest */

static inline uint32_t
debugframe_crc (uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];

    if (!table[1])
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;

            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static inline void
debugframe_put32 (uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The frame into `out', DEBUGFRAME_HEAD + len + 4 bytes; its length. */
static inline size_t
debugframe_encode (uint8_t *out, char type, uint32_t seq, const void *payload, size_t len)
{
    out[0] = 0x1e;
    out[1] = 'T';
    out[2] = 'B';
    out[3] = (uint8_t)type;
    debugframe_put32(out + 4, seq);
    out[8] = (uint8_t)len;
    out[9] = (uint8_t)(len >> 8);
    memcpy(out + DEBUGFRAME_HEAD, payload, len);
    debugframe_put32(out + DEBUGFRAME_HEAD + len,
                     debugframe_crc(0, out + 3, DEBUGFRAME_HEAD - 3 + len));
    return DEBUGFRAME_HEAD + len + 4;
}
