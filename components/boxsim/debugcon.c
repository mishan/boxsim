/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* The debug console: debugcon.h. */
#include "debugcon.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "box_hal.h"
#include "debugframe.h"

#define W BOX_HAL_W
#define H BOX_HAL_H
#define PIECE 1024                      /* of a screenshot, a frame each */
#define SLOT 128                        /* audio frames a slot */
#define SLOTS 256                       /* a second of the BOX's audio */
#define DRAIN_MOST 16                   /* slots sent between reads of commands */

/* ---- the link ------------------------------------------------------------- */

/* A frame is one write, never split by a line of log: the driver's ring
   takes a write whole or not at all, and the log's text goes through it
   too once the console uses the driver. */
#if CONFIG_IDF_TARGET_LINUX
/* The simulator's link, a pty for boxctl to open; the log stays on stdout. */
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static int pty = -1;

static bool
linkOpen (void)
{
    struct termios t;

    pty = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (pty < 0 || grantpt(pty) || unlockpt(pty) || tcgetattr(pty, &t))
        return false;
    /* Frames are binary: no echo, no newline translation. */
    cfmakeraw(&t);
    tcsetattr(pty, TCSANOW, &t);
    printf("debugcon: on %s\n", ptsname(pty));
    return true;
}

/* Polled, as a task blocked in read() would hold up the scheduler's other
   tasks till the next tick. */
static int
linkRead (uint8_t *b, int n, TickType_t t)
{
    for (TickType_t waited = 0;; waited++)
    {
        const ssize_t r = read(pty, b, n);

        if (r > 0 || waited >= t)
            return r > 0 ? (int)r : 0;
        vTaskDelay(1);
    }
}

static size_t
linkWrite (const uint8_t *b, size_t n)
{
    const ssize_t w = write(pty, b, n);

    return w > 0 ? (size_t)w : 0;
}

/* No one has the pty open while it hangs up. */
static bool
linkUp (void)
{
    struct pollfd p = { .fd = pty, .events = POLLOUT };

    return poll(&p, 1, 0) >= 0 && !(p.revents & POLLHUP);
}
#elif defined(DEBUGCON_UART)
#include "driver/uart.h"
#include "driver/uart_vfs.h"

static bool
linkOpen (void)
{
    if (uart_driver_install(UART_NUM_0, 1024, 8192, 0, NULL, 0) != ESP_OK)
        return false;
    uart_vfs_dev_use_driver(UART_NUM_0);
    return true;
}

static int linkRead (uint8_t *b, int n, TickType_t t) { return uart_read_bytes(UART_NUM_0, b, n, t); }
static size_t linkWrite (const uint8_t *b, size_t n) { const int w = uart_write_bytes(UART_NUM_0, b, n); return w > 0 ? (size_t)w : 0; }
static bool linkUp (void) { return true; }
#else
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static bool
linkOpen (void)
{
    usb_serial_jtag_driver_config_t c = { .tx_buffer_size = 8192, .rx_buffer_size = 512 };

    if (usb_serial_jtag_driver_install(&c) != ESP_OK)
        return false;
    usb_serial_jtag_vfs_use_driver();
    return true;
}

static int linkRead (uint8_t *b, int n, TickType_t t) { return usb_serial_jtag_read_bytes(b, n, t); }
/* Without waiting: a write waiting on the ring holds the driver's lock,
   and the log's every character would wait behind it. */
static size_t linkWrite (const uint8_t *b, size_t n) { const int w = usb_serial_jtag_write_bytes(b, n, 0); return w > 0 ? (size_t)w : 0; }
static bool linkUp (void) { return usb_serial_jtag_is_connected(); }
#endif

/* A frame sent whole within a fifth of a second, or given up on: a frame
   cut short the host reads as text. */
static bool
send (char type, uint32_t seq, const void *payload, size_t len)
{
    static uint8_t frame[DEBUGFRAME_HEAD + DEBUGFRAME_MOST + 4];
    const size_t n = debugframe_encode(frame, type, seq, payload, len);
    const int64_t until = esp_timer_get_time() + 200000;
    size_t sent = 0;

    while (linkUp())
    {
        sent += linkWrite(frame + sent, n - sent);
        if (sent == n)
            return true;
        if (esp_timer_get_time() >= until)
            break;
        vTaskDelay(1);
    }
    return false;
}

/* The command being run, which its reply starts with, so the host can tell
   it from one to a command before. */
static const char *asked = "";
static uint32_t replies = 0;

static void reply (const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
reply (const char *fmt, ...)
{
    char text[DEBUGFRAME_MOST];
    const int n = snprintf(text, sizeof(text), "%s: ", asked);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text + n, sizeof(text) - n, fmt, ap);
    va_end(ap);
    send('T', replies++, text, strlen(text));
}

/* ---- the audio ------------------------------------------------------------ */

/* Blocks from the audio task to this one, a slot each, with no lock: the
   audio task drops a block the ring has no room for, and counts it. */
static int rate = 0;
static int16_t (*ring)[2 * SLOT];
static uint16_t ringCount[SLOTS];
static uint32_t ringSeq[SLOTS];
static uint32_t head = 0, tail = 0;
static volatile bool streaming = false;
static volatile uint32_t audioSeq = 0, audioDropped = 0;
static uint32_t audioSent = 0, droppedAtOn = 0;

static void
written (const int16_t *frames, int count)
{
    while (streaming && count > 0)
    {
        const int n = count < SLOT ? count : SLOT;
        const uint32_t h = __atomic_load_n(&head, __ATOMIC_RELAXED);

        if (h - __atomic_load_n(&tail, __ATOMIC_ACQUIRE) == SLOTS)
            audioDropped++;
        else
        {
            memcpy(ring[h % SLOTS], frames, n * 4);
            ringCount[h % SLOTS] = (uint16_t)n;
            ringSeq[h % SLOTS] = audioSeq;
            __atomic_store_n(&head, h + 1, __ATOMIC_RELEASE);
        }
        audioSeq++;
        frames += 2 * n;
        count -= n;
    }
}

/* What the ring holds, sent, DRAIN_MOST slots at most, so a link slower
   than the audio, which never empties the ring, still leaves time to read
   commands; a block the link would not take in a fifth of a second ends
   the stream, the host gone. */
static void
drain (void)
{
    uint8_t payload[4 + 4 * SLOT];

    for (uint32_t t = tail, end = tail + DRAIN_MOST;
         t != end && t != __atomic_load_n(&head, __ATOMIC_ACQUIRE); t++)
    {
        const int n = ringCount[t % SLOTS];

        debugframe_put32(payload, (uint32_t)rate);
        memcpy(payload + 4, ring[t % SLOTS], n * 4);
        if (!streaming || !send('A', ringSeq[t % SLOTS], payload, 4 + n * 4))
            streaming = false;
        else
            audioSent++;
        __atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);
    }
}

/* A pause that goes on streaming. */
static void
wait (int ms)
{
    const int64_t until = esp_timer_get_time() + ms * 1000LL;

    while (esp_timer_get_time() < until)
    {
        drain();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* ---- the screen ----------------------------------------------------------- */

/* The screen as drawn, big-endian RGB565 as the panel takes it, and a copy
   of it to send while the screen goes on being drawn. */
static SemaphoreHandle_t shadowLock;
static uint16_t *shadow, *shot;

static void
drawn (int x0, int y0, int x1, int y1, const uint16_t *pixels)
{
    if (shadow && x0 >= 0 && y0 >= 0 && x1 <= W && y1 <= H && x1 > x0)
    {
        xSemaphoreTake(shadowLock, portMAX_DELAY);
        for (int y = y0; y < y1; y++)
            memcpy(shadow + y * W + x0, pixels + (y - y0) * (x1 - x0), (x1 - x0) * 2);
        xSemaphoreGive(shadowLock);
    }
}

static void
screenshot (uint32_t number)
{
    const uint8_t *bytes = (const uint8_t *)shot;
    uint8_t payload[4 + PIECE];

    xSemaphoreTake(shadowLock, portMAX_DELAY);
    memcpy(shot, shadow, W * H * 2);
    xSemaphoreGive(shadowLock);

    const uint32_t crc = debugframe_crc(0, bytes, W * H * 2);
    uint8_t info[12 + 8] = { W & 0xff, W >> 8, H & 0xff, H >> 8 };

    debugframe_put32(info + 4, W * H * 2);
    debugframe_put32(info + 8, crc);
    memcpy(info + 12, "rgb565be", 8);
    send('S', number, info, sizeof(info));
    for (uint32_t at = 0; at < W * H * 2; at += PIECE)
    {
        debugframe_put32(payload, at);
        memcpy(payload + 4, bytes + at, PIECE);
        send('P', at / PIECE, payload, sizeof(payload));
        drain();
    }
    reply("%u, %dx%d rgb565be, crc %08lx", (unsigned)number, W, H, (unsigned long)crc);
}

/* ---- the hand ------------------------------------------------------------- */

static volatile bool sideDown = false, homeDown = false, fingerDown = false;
static volatile int fingerX = 0, fingerY = 0;
static volatile int64_t shakeUntil = 0;

static bool held (int button) { return button == BOX_HAL_SIDE ? sideDown : homeDown; }
static bool jolt (void) { return esp_timer_get_time() < shakeUntil; }

static bool
finger (int *x, int *y)
{
    if (!fingerDown)
        return false;
    *x = fingerX;
    *y = fingerY;
    return true;
}

/* ---- the commands --------------------------------------------------------- */

static unsigned inputs;
static void (*appStatus) (char *, size_t);

static const char HELP[] =
    "screenshot             the screen, as drawn\n"
    "audio on | audio off   the codec's frames, streamed\n"
    "tap X Y                a finger down at X Y, lifted 80 ms later\n"
    "down X Y | move X Y | up\n"
    "press side | hold side the side button, 100 ms or 800 ms\n"
    "home                   the red button\n"
    "shake                  a shake, as the IMU would feel one\n"
    "status | help";

/* The commands for inputs a firmware may not read. */
static const struct { const char *word; unsigned input; const char *what; } NEEDS[] = {
    { "tap", DEBUGCON_TOUCH, "touchscreen" }, { "down", DEBUGCON_TOUCH, "touchscreen" },
    { "move", DEBUGCON_TOUCH, "touchscreen" }, { "up", DEBUGCON_TOUCH, "touchscreen" },
    { "home", DEBUGCON_HOME, "red button" }, { "shake", DEBUGCON_SHAKE, "IMU" } };

static void
run (char *line)
{
    static uint32_t shots = 0;
    char word[16] = "", arg[16] = "";
    int x = 0, y = 0;
    const int n = sscanf(line, "%15s %15s", word, arg);
    const bool xy = sscanf(line, "%*s %d %d", &x, &y) == 2 && x >= 0 && x < W && y >= 0 &&
                    y < H;

    if (n < 1)
        return;
    asked = line;
    for (size_t i = 0; i < sizeof(NEEDS) / sizeof(NEEDS[0]); i++)
        if (!strcmp(word, NEEDS[i].word) && !(inputs & NEEDS[i].input))
        {
            reply("no %s on this firmware", NEEDS[i].what);
            return;
        }
    if (!strcmp(word, "screenshot") || !strcmp(word, "shot"))
    {
        if (shadow)
            screenshot(shots++);
        else
            reply("no memory for it");
    }
    else if (!strcmp(word, "audio") && !strcmp(arg, "on"))
    {
        droppedAtOn = audioDropped;
        audioSent = 0;
        __atomic_store_n(&tail, __atomic_load_n(&head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        streaming = ring != NULL;
        if (ring)
            reply("%d Hz, 2 channels", rate);
        else
            reply("no memory for it");
    }
    else if (!strcmp(word, "audio") && !strcmp(arg, "off"))
    {
        streaming = false;
        reply("%u blocks sent, %u dropped", (unsigned)audioSent,
              (unsigned)(audioDropped - droppedAtOn));
    }
    else if ((!strcmp(word, "tap") || !strcmp(word, "down") || !strcmp(word, "move")))
    {
        if (!xy)
        {
            reply("X 0 to %d, Y 0 to %d", W - 1, H - 1);
            return;
        }
        fingerX = x;
        fingerY = y;
        fingerDown = true;
        if (!strcmp(word, "tap"))
        {
            wait(80);
            fingerDown = false;
        }
        reply("ok");
    }
    else if (!strcmp(word, "up"))
    {
        fingerDown = false;
        reply("ok");
    }
    else if ((!strcmp(word, "press") || !strcmp(word, "hold")) && !strcmp(arg, "side"))
    {
        sideDown = true;
        wait(word[0] == 'p' ? 100 : 800);
        sideDown = false;
        reply("ok");
    }
    else if (!strcmp(word, "home"))
    {
        homeDown = true;
        wait(100);
        homeDown = false;
        reply("ok");
    }
    else if (!strcmp(word, "shake"))
    {
        shakeUntil = esp_timer_get_time() + 50000;
        reply("ok");
    }
    else if (!strcmp(word, "status"))
    {
        char app[400] = "";

        if (appStatus)
            appStatus(app, sizeof(app));
        reply("%s  free internal %u psram %u  audio %s, %u dropped", app,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
              streaming ? "streaming" : "off", (unsigned)audioDropped);
    }
    else if (!strcmp(word, "help"))
        reply("%s", HELP);
    else
        reply("not a command; try help");
}

static void
consoleTask (void *arg)
{
    char line[128];
    int len = 0;

    for (;;)
    {
        uint8_t in[64];
        const int n = linkRead(in, sizeof(in), pdMS_TO_TICKS(streaming ? 2 : 20));

        for (int i = 0; i < n; i++)
            if (in[i] == '\n' || in[i] == '\r')
            {
                line[len] = 0;
                if (len > 0)
                    run(line);
                len = 0;
            }
            else if (len < (int)sizeof(line) - 1)
                line[len++] = (char)in[i];
        drain();
    }
}

void
debugcon_start (int r, unsigned in, void (*status) (char *, size_t))
{
    rate = r;
    inputs = in;
    appStatus = status;
    shadowLock = xSemaphoreCreateMutex();
    ring = heap_caps_calloc(SLOTS, sizeof(*ring), MALLOC_CAP_SPIRAM);
    shot = heap_caps_calloc(W * H, 2, MALLOC_CAP_SPIRAM);
    if (shot)
        shadow = heap_caps_calloc(W * H, 2, MALLOC_CAP_SPIRAM);
    if (!shadowLock || !linkOpen())
    {
        printf("debugcon: no console\n");
        return;
    }
    static const box_hal_taps taps = { drawn, written, finger, held, jolt };

    box_hal_tap(&taps);
    xTaskCreatePinnedToCore(consoleTask, "debugcon", 6144, NULL, 2, NULL, 0);
}
