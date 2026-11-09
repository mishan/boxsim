/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* box_hal.h on the desktop, for ESP-IDF's linux target: the screen a
   window, the speaker the desktop's audio (or a WAV), the touchscreen the
   mouse, the buttons and the IMU's pose keys, or a script for all of it.
   SDL lives on a thread of its own outside FreeRTOS, started before the
   scheduler: the linux port switches its tasks' threads with signals,
   which SDL's threads must never take. */
#include "box_hal.h"
#include "pose.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define W BOX_HAL_W
#define H BOX_HAL_H
#define AHEAD_US 50000                  /* audio written ahead of playing, at most */
#define STEP 5                          /* degrees an arrow key tips it */
#define TAP_US 50000                    /* a click's touch lasts at least this */

static int scale = 2;
static bool headless = false, mute = false;
static const char *script = NULL, *wavPath = NULL;
static char app[64] = "box";

static box_hal_taps taps;

/* What the program asked box_hal_open for, and what the SDL thread has
   set up of it. */
static atomic_uint wanted, served;
static int rate;

/* The input, as the SDL thread or the script last set it. */
static atomic_bool touching[2], held[2];
static atomic_int touchX[2], touchY[2], tilt, roll;
static _Atomic int64_t shakeUntil;
/* A click's press and release come together, too quick for a firmware
   polling the screen to see: its lift waits till liftAt. */
static int64_t downAt, liftAt;

/* ---- the screen ----------------------------------------------------------- */

/* Big-endian RGB565, as drawn. */
static pthread_mutex_t fbLock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t fb[W * H];
static atomic_bool fbDirty;

void
box_lcd_draw (int x0, int y0, int x1, int y1, const uint16_t *pixels)
{
    if (taps.drawn)
        taps.drawn(x0, y0, x1, y1, pixels);
    if (!(served & BOX_HAL_SCREEN) || x0 < 0 || y0 < 0 || x1 > W || y1 > H || x1 <= x0)
        return;
    /* Not switched out holding the lock, where a task of higher priority
       could wait on it for good. */
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    portENTER_CRITICAL(&mux);
    pthread_mutex_lock(&fbLock);
    for (int y = y0; y < y1; y++)
        memcpy(fb + y * W + x0, pixels + (y - y0) * (x1 - x0), (x1 - x0) * 2);
    pthread_mutex_unlock(&fbLock);
    portEXIT_CRITICAL(&mux);
    fbDirty = true;
}

/* As boxctl expands it, so the two agree to the bit. */
static bool
shot (const char *path)
{
    static uint8_t rgb[W * H * 3];
    FILE *f = fopen(path, "wb");

    pthread_mutex_lock(&fbLock);
    for (int i = 0; i < W * H; i++)
    {
        const unsigned v = (fb[i] >> 8 | fb[i] << 8) & 0xffff;
        const unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;

        rgb[3 * i] = r << 3 | r >> 2;
        rgb[3 * i + 1] = g << 2 | g >> 4;
        rgb[3 * i + 2] = b << 3 | b >> 2;
    }
    pthread_mutex_unlock(&fbLock);
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(rgb, 1, sizeof(rgb), f);
    return fclose(f) == 0;
}

/* ---- the speaker ---------------------------------------------------------- */

/* Frames from the program's audio task to SDL's, without a lock: a task
   holding one could be switched out mid-copy, and SDL's thread would
   stall behind it. */
static SDL_AudioDeviceID device;
static int16_t *ring;
static unsigned ringFrames;
static atomic_uint ringHead, ringTail;

static void SDLCALL
pull (void *user, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    const unsigned want = len / 4, t = ringTail;
    unsigned n = ringHead - t;

    n = n < want ? n : want;
    for (unsigned i = 0; i < n; i++)
        memcpy(out + 2 * i, ring + 2 * ((t + i) % ringFrames), 4);
    memset(out + 2 * n, 0, (want - n) * 4);
    ringTail = t + n;
}

static pthread_mutex_t wavLock = PTHREAD_MUTEX_INITIALIZER;
static FILE *wav;
static uint32_t wavFrames;

/* The header rewritten with each write, so a WAV cut short by a kill
   still reads. */
static void
wavHeader (void)
{
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0........\x04\0\x10\0data";
    const uint32_t v[4] = { 36 + wavFrames * 4, (uint32_t)rate, (uint32_t)rate * 4,
                            wavFrames * 4 };
    const int at[4] = { 4, 24, 28, 40 };

    for (int k = 0; k < 4; k++)
        for (int b = 0; b < 4; b++)
            h[at[k] + b] = (uint8_t)(v[k] >> (8 * b));
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), wav);
    fseek(wav, 0, SEEK_END);
    fflush(wav);
}

/* Without a device to wait on, the clock's pace instead. */
static void
pace (int count)
{
    static double due = 0;
    const int64_t now = esp_timer_get_time();

    if (due < now)
        due = now;
    due += count * 1e6 / rate;
    while (due - esp_timer_get_time() > AHEAD_US)
        vTaskDelay(1);
}

void
box_audio_write (const int16_t *frames, int count)
{
    if (taps.written)
        taps.written(frames, count);
    if (!(served & BOX_HAL_SPEAKER))
        return;
    if (device)
    {
        while (count > 0)
        {
            const unsigned h = ringHead, room = ringFrames - (h - ringTail);
            const int n = (int)room < count ? (int)room : count;

            if (n == 0)
            {
                vTaskDelay(1);
                continue;
            }
            for (int i = 0; i < n; i++)
                memcpy(ring + 2 * ((h + i) % ringFrames), frames + 2 * i, 4);
            ringHead = h + n;
            frames += 2 * n;
            count -= n;
        }
        return;
    }
    pace(count);
    if (wav)
    {
        pthread_mutex_lock(&wavLock);
        fwrite(frames, 4, count, wav);
        wavFrames += count;
        wavHeader();
        pthread_mutex_unlock(&wavLock);
    }
}

/* ---- the touchscreen, the buttons, the IMU -------------------------------- */

int
box_touch_read (box_hal_contact *out, int most)
{
    int count = 0;

    for (int f = 0; f < 2 && count < most && (served & BOX_HAL_TOUCH); f++)
        if (touching[f])
            out[count++] = (box_hal_contact){ f, touchX[f], touchY[f] };
    if (most > 0 && taps.finger && taps.finger(&out[0].x, &out[0].y))
    {
        out[0].id = 31;
        count = 1;
    }
    return count;
}

/* The red button is the touch controller's, as on the BOX. */
bool
box_button_down (int button)
{
    const unsigned part = button == BOX_HAL_SIDE ? BOX_HAL_SIDE_BUTTON : BOX_HAL_TOUCH;
    const bool down = (served & part) && held[button == BOX_HAL_HOME];

    return down || (taps.held && taps.held(button));
}

bool
box_imu_read (float accel[3], float gyro[3])
{
    const bool jolt = esp_timer_get_time() < shakeUntil || (taps.jolt && taps.jolt());

    if (!(served & BOX_HAL_IMU) && !jolt)
        return false;

    box_sim_gravity(tilt, roll, accel);
    for (int k = 0; k < 3; k++)
    {
        accel[k] *= jolt ? 3 : 1;
        gyro[k] = 0;
    }
    return true;
}

void
box_hal_tap (const box_hal_taps *t)
{
    taps = *t;
}

/* The SDL thread sets up what's asked for: SDL is all on its thread. */
unsigned
box_hal_open (unsigned want, int r, int volume)
{
    want &= BOX_HAL_SPEAKER | BOX_HAL_SCREEN | BOX_HAL_IMU | BOX_HAL_TOUCH |
            BOX_HAL_SIDE_BUTTON;
    if (want & BOX_HAL_SPEAKER)
        rate = r;
    wanted |= want;
    while ((served & want) != want)
        vTaskDelay(1);
    return want;
}

/* ---- the SDL thread ------------------------------------------------------- */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

static void
quit (int status)
{
    pthread_mutex_lock(&wavLock);
    if (wav)
        fclose(wav);
    fflush(stdout);
    _exit(status);
}

static void
title (void)
{
    char t[128];

    snprintf(t, sizeof(t), "boxsim: %s  tilt %+d roll %+d", app, (int)tilt, (int)roll);
    SDL_SetWindowTitle(window, t);
}

static void
present (void)
{
    static uint16_t native[W * H];

    pthread_mutex_lock(&fbLock);
    for (int i = 0; i < W * H; i++)
        native[i] = fb[i] >> 8 | fb[i] << 8;
    pthread_mutex_unlock(&fbLock);
    SDL_UpdateTexture(texture, NULL, native, W * 2);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static void
screenOpen (void)
{
    if (headless)
        return;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    window = SDL_CreateWindow("boxsim", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              W * scale, H * scale, 0);
    /* Software, so the pixels on screen are the ones drawn, scaled exactly. */
    renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
    texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                           SDL_TEXTUREACCESS_STREAMING, W, H) : NULL;
    if (!texture)
    {
        fprintf(stderr, "boxsim: no window: %s\n", SDL_GetError());
        exit(1);
    }
    title();
    present();
    printf("boxsim: mouse touches; S side button, H red button, arrows tilt and roll, "
           "R level, X shake; ctrl-click a second finger\n");
}

static void
speakerOpen (void)
{
    if (wavPath)
    {
        wav = fopen(wavPath, "wb");
        if (!wav)
        {
            perror(wavPath);
            exit(1);
        }
        wavHeader();
        return;
    }
    if (headless || mute)
        return;

    SDL_AudioSpec want = { .freq = rate, .format = AUDIO_S16SYS, .channels = 2,
                           .samples = 512, .callback = pull };

    ringFrames = (int64_t)rate * AHEAD_US / 1000000;
    ring = calloc(ringFrames, 4);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
        device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (device)
        SDL_PauseAudioDevice(device, 0);
    else
        printf("boxsim: no audio (%s); playing to nowhere\n", SDL_GetError());
}

static void
finger (int f, bool down, int x, int y)
{
    touchX[f] = x < 0 ? 0 : x >= W ? W - 1 : x;
    touchY[f] = y < 0 ? 0 : y >= H ? H - 1 : y;
    touching[f] = down;
}

static void
pose (int t, int r)
{
    tilt = t < -90 ? -90 : t > 90 ? 90 : t;
    roll = r < -90 ? -90 : r > 90 ? 90 : r;
    if (window)
        title();
}

static void
event (const SDL_Event *e)
{
    const int x = e->button.x / scale, y = e->button.y / scale;

    switch (e->type)
    {
    case SDL_QUIT:
        quit(0);
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button != SDL_BUTTON_LEFT)
            break;
        if (SDL_GetModState() & KMOD_CTRL)
            finger(1, !touching[1], x, y);
        else
        {
            finger(0, true, x, y);
            downAt = esp_timer_get_time();
            liftAt = 0;
        }
        break;
    case SDL_MOUSEMOTION:
        if (touching[0] && (e->motion.state & SDL_BUTTON_LMASK))
            finger(0, true, e->motion.x / scale, e->motion.y / scale);
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT)
            liftAt = downAt + TAP_US;
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
        const bool down = e->type == SDL_KEYDOWN;

        switch (e->key.keysym.sym)
        {
        case SDLK_s: held[0] = down; break;
        case SDLK_h: held[1] = down; break;
        case SDLK_UP: if (down) pose(tilt + STEP, roll); break;
        case SDLK_DOWN: if (down) pose(tilt - STEP, roll); break;
        case SDLK_RIGHT: if (down) pose(tilt, roll + STEP); break;
        case SDLK_LEFT: if (down) pose(tilt, roll - STEP); break;
        case SDLK_r: if (down) pose(0, 0); break;
        case SDLK_x: if (down) shakeUntil = esp_timer_get_time() + 100000; break;
        }
        break;
    }
    case SDL_WINDOWEVENT:
        fbDirty = true;
        break;
    }
}

/* ---- the script: "MS COMMAND ARGS; ...", MS after the program starts ------ */

static void
step (const char *s)
{
    char word[16] = "", arg[256] = "";
    int x = 0, y = 0;
    const int n = sscanf(s, "%15s %255s %d", word, arg, &y);
    const int b = !strcmp(arg, "home");

    x = atoi(arg);
    if (!strcmp(word, "down") || !strcmp(word, "move"))
        finger(0, true, x, y);
    else if (!strcmp(word, "up"))
        touching[0] = false;
    else if (!strcmp(word, "hold") || !strcmp(word, "release"))
        held[b] = word[0] == 'h';
    else if (!strcmp(word, "tilt"))
        pose(x, roll);
    else if (!strcmp(word, "roll"))
        pose(tilt, x);
    else if (!strcmp(word, "shake"))
        shakeUntil = esp_timer_get_time() + 100000;
    else if (!strcmp(word, "shot") && n >= 2)
    {
        if (!shot(arg))
            perror(arg);
    }
    else if (!strcmp(word, "quit"))
        quit(0);
    else
        fprintf(stderr, "boxsim: script: what is '%s'?\n", s);
}

/* Each step due run, the script left at the next. */
static void
scriptRun (void)
{
    while (script && *script)
    {
        char *end;
        const long ms = strtol(script, &end, 10);
        const char *semi = strchr(end, ';');
        const size_t len = semi ? (size_t)(semi - end) : strlen(end);
        char s[300];

        if (end == script || esp_timer_get_time() < ms * 1000LL)
            return;
        snprintf(s, sizeof(s), "%.*s", (int)len, end);
        script = semi ? semi + 1 : end + len;
        step(s);
    }
}

static void *
sdlThread (void *arg)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!headless && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        fprintf(stderr, "boxsim: %s (--headless for none)\n", SDL_GetError());
        exit(1);
    }
    for (;;)
    {
        const unsigned todo = wanted & ~served;

        if (todo & BOX_HAL_SCREEN)
            screenOpen();
        if (todo & BOX_HAL_SPEAKER)
            speakerOpen();
        served |= todo;

        SDL_Event e;

        if (window)
            while (SDL_PollEvent(&e))
                event(&e);
        if (liftAt && esp_timer_get_time() >= liftAt)
        {
            touching[0] = false;
            liftAt = 0;
        }
        if (window && fbDirty)
        {
            fbDirty = false;
            present();
        }
        scriptRun();
        usleep(4000);
    }
    return NULL;
}

static const char USAGE[] =
    "%s, the ESP32-S3-BOX firmware in boxsim's window\n"
    "  --scale N      the window N times the screen's 320x240 (2)\n"
    "  --mute         no sound\n"
    "  --headless     no window and no sound; the script plays it\n"
    "  --script S     timed input, \"MS COMMAND; ...\", MS after the start:\n"
    "                 down X Y, move X Y, up, hold side|home, release side|home,\n"
    "                 tilt DEG, roll DEG, shake, shot FILE.ppm, quit\n"
    "BOXSIM_AUDIO=wav:FILE writes the sound to a WAV instead.\n";

/* glibc hands a constructor main's arguments. Before the scheduler starts,
   so the thread is no FreeRTOS task's and takes none of its signals. */
__attribute__((constructor)) static void
boxsim (int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    const char *audio = getenv("BOXSIM_AUDIO");

    snprintf(app, sizeof(app), "%.*s", (int)strcspn(base, "."), base);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--scale") && i + 1 < argc && atoi(argv[i + 1]) > 0)
            scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mute"))
            mute = true;
        else if (!strcmp(argv[i], "--headless"))
            headless = true;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            script = argv[++i];
        else
        {
            printf(USAGE, app);
            exit(strcmp(argv[i], "--help") != 0);
        }
    if (audio && !strncmp(audio, "wav:", 4))
        wavPath = audio + 4;

    sigset_t all, was;
    pthread_t t;

    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &was);
    if (pthread_create(&t, NULL, sdlThread, NULL) != 0)
        exit(1);
    pthread_sigmask(SIG_SETMASK, &was, NULL);
}
