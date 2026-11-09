# boxsim

Board support for the Espressif ESP32-S3-BOX (the original one, not the
S3-BOX-3 or Lite), and a simulator that runs the same firmware on a Linux
desktop, in a window.

A firmware plays the BOX through `box_hal.h`: the 320x240 LCD, the
speaker, the touchscreen and its red button, the side button and the
IMU. On the BOX, `box_hal_esp32.c` drives them with ESP-IDF and
esp_codec_dev. Built for ESP-IDF's `linux` target, `box_hal_sim.c` stands
in: the screen is a window, the speaker the desktop's audio, the
touchscreen the mouse, and the buttons and the IMU's pose keys. FreeRTOS
tasks, semaphores, `esp_timer_get_time` and `heap_caps_malloc` all work
as they do on the BOX, so the firmware runs unchanged.

A debug console (`debugcon.h`) comes with it, on the BOX's USB serial
link or the simulator's pty. `tools/boxctl.py` drives it from the
desktop: screenshots, the audio as written, and taps and presses.

    components/boxsim/       the ESP-IDF component
      include/box_hal.h      the board, as a firmware plays it
      include/debugcon.h     the debug console; debugframe.h, its frames
      box_hal_esp32.c        the BOX
      sim/                   the simulator, for the linux target only
    tools/boxctl.py          the debug console, from the desktop
    examples/hello/          bars, a dot and a beep where it's touched, a level
    tests/                   boxctl's reader; hello, run headless

## Using it

A firmware takes boxsim as an ESP-IDF component, pinned to a release,
from the component manager or as a git submodule. From the component
manager, in main's `idf_component.yml`:

    dependencies:
      boxsim:
        git: https://github.com/mishan/boxsim.git
        path: components/boxsim
        version: v0.1.0

`version` is a tag or a commit, and the firmware's lock file records the
commit it came to, so its builds take the same boxsim, and the same
esp_codec_dev, until it's changed. As a submodule instead:

    git submodule add https://github.com/mishan/boxsim.git boxsim
    git -C boxsim checkout v0.1.0

and `set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boxsim/components")`
in the firmware's top-level `CMakeLists.txt`, before the `include`.

Either way, in that `CMakeLists.txt`, after the `include` and before
`project()`:

    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    if(IDF_TARGET STREQUAL "linux")
      # Its own lock file, as the components differ, and only what main
      # needs, as most of ESP-IDF has no linux port.
      idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.linux")
      idf_build_set_property(MINIMAL_BUILD ON)
    endif()

and `REQUIRES boxsim` in main's `idf_component_register`.
`examples/hello` takes boxsim from this tree and sets both properties
for every target. Releases are tagged `vMAJOR.MINOR.PATCH`, the version
in `components/boxsim/idf_component.yml`.

## Building

With ESP-IDF 6.1 (`. ~/esp/esp-idf/export.sh`), for the BOX:

    cd examples/hello
    idf.py set-target esp32s3
    idf.py -p /dev/ttyACM0 flash monitor

For the desktop, ESP-IDF's linux target, which is a preview, wants
pkg-config, SDL2's headers (libsdl2-dev; sdl2-compat over SDL3 works)
and libbsd's (libbsd-dev). A build directory and sdkconfig of its own
keep it apart from the BOX's:

    idf.py --preview -B build-sim -D SDKCONFIG=build-sim/sdkconfig set-target linux
    idf.py -B build-sim -D SDKCONFIG=build-sim/sdkconfig build
    build-sim/hello.elf

## The simulator

The window is titled `boxsim: NAME  tilt T roll R`, NAME the program's
own (`hello.elf` is `hello`), and the pose in degrees after it. All of
the window is the screen, at twice its size by default, every pixel
of the BOX's a square of the window's, so a screenshot of the window is
exactly what the firmware drew.

| | |
|---|---|
| left button, dragged | a finger on the screen |
| ctrl-click | a second finger, held there till the next ctrl-click |
| S | the side button, while held |
| H | the red button under the screen, while held |
| up, down | tilt the top away, toward you, 5 degrees a press |
| right, left | roll it like a wheel, 5 degrees a press |
| R | level again: upright, facing you |
| X | a shake, a 3 g jolt for a tenth of a second |

A click lasts at least 50 ms, so a firmware that polls the screen sees
it. The IMU reads (1, 0, 0) g upright, as the BOX does, and its gyro
reads nothing.

    --scale N      the window N times the screen's 320x240 (2)
    --mute         no sound
    --headless     no window and no sound; a script plays it
    --script S     timed input: "MS COMMAND; ..."
    BOXSIM_AUDIO=wav:FILE   the sound to a WAV instead

A script's steps are each at so many milliseconds after the program
starts: `down X Y`, `move X Y`, `up`, `hold side|home`,
`release side|home`, `tilt DEG`, `roll DEG`, `shake`, `shot FILE.ppm`
(the screen, 320x240) and `quit`. `--headless` with a script and a WAV
is how the tests check a firmware without a display:

    BOXSIM_AUDIO=wav:out.wav build-sim/hello.elf --headless \
        --script "500 down 60 50; 700 up; 1000 shot out.ppm; 1100 quit"

The speaker plays at the rate the firmware opened it at, through SDL
(PipeWire or PulseAudio underneath), and `box_audio_write` waits as it
would on the BOX, with at most 50 ms written ahead. With no device to
wait on (muted, headless or writing a WAV) it keeps to the clock's
pace instead, so the firmware runs in real time either way.

How it works: ESP-IDF's linux target builds the firmware against the
host's C library, its FreeRTOS kernel the POSIX port, each task a
thread. That port runs one core and switches tasks with signals, so SDL
has a thread of its own outside FreeRTOS, started before the scheduler
with every signal blocked, and the firmware's tasks hand it pixels,
audio and input without waiting on it. A few shims cover what the linux
target lacks: `esp_timer_get_time` (headers only there), and
`xTaskCreatePinnedToCore`, whose second core the port asserts on, wrapped
at link time to run every task on the one core. Only `box_hal.h` stands
in for the hardware: a firmware that drives GPIO, I2C or the LCD itself
won't build for linux.

## Debug console

`debugcon_start` starts a console task that takes commands on the link
the firmware logs on and answers among the log's text. On the BOX that
is the USB-Serial/JTAG port; in the simulator, a pty, named as the
program starts (`debugcon: on /dev/pts/N`), while the log stays on the
terminal. `tools/boxctl.py` speaks it, with pyserial:

    tools/boxctl.py -p /dev/ttyACM0 status
    boxctl.py shot screen.png        # the screen as drawn, a 320x240 PNG
    boxctl.py audio 5 out.wav        # 5 s of exactly what the codec is sent
    boxctl.py tap 160 120            # a finger down there, lifted 80 ms later
    boxctl.py down X Y / move X Y / up
    boxctl.py press side             # 100 ms
    boxctl.py hold side              # 800 ms
    boxctl.py home                   # the red button
    boxctl.py shake
    boxctl.py raw "help"             # any command, its reply printed
    boxctl.py log                    # the log, as the monitor would show it

`shot out.png --compare ref.png [--max N]` runs
[shotbox](https://github.com/mishan/shotbox)'s `compare` on the
screenshot, and `audio SECONDS out.wav --check` its `listen` on the
recording, which finds dropouts, clipping and silence, if shotbox 0.11.0
or later is installed. A block the firmware dropped is silence in the
WAV, which `listen` hears as a gap too, so boxctl's count of them comes
beside it; `--gap MS` is `listen`'s, for a longer one.

`-p` names the port (`/dev/ttyACM0` by default); `-v` prints the log
while a command runs. boxctl leaves DTR and RTS as the port opens them,
both on, which doesn't reset the BOX as the monitor's start does. Only
one program can have the port open, so quit the monitor first.

The console taps `box_hal.h` (`box_hal_tap`): it sees what is drawn and
written to the speaker, its finger stands in for the screen's contacts
while it is down, the side and red buttons are pressed alongside the real
ones, and a shake is a jolt in the IMU's reading, so they play as a
hand's would. `status` says what the firmware's own `status` callback
writes, and the free heap.

The replies, the screenshot and the audio come back as frames among the
log's text (`debugframe.h`): `0x1e 'T' 'B'`, a type, a sequence number,
a length, the payload and a CRC-32, which boxctl checks, reading a frame
that fails as text. A screenshot is an `S` frame (size, format and the
whole image's CRC) and its pieces; audio a frame a block, 128 stereo
frames, numbered, so a missing one is a gap boxctl fills with silence and
counts. The audio task never waits on the link: a block the link has no
room for is dropped, and `audio off` and `status` say how many. The
screenshot is the pixels as handed to the panel, before the SPI sends
them, big-endian RGB565 as the panel takes them.

The console is built in unless `CONFIG_BOXSIM_DEBUGCON` is off (in
`idf.py menuconfig`, boxsim, or `CONFIG_BOXSIM_DEBUGCON=n` in a
firmware's `sdkconfig.defaults`), for a release, say: `debugcon_start`
does nothing then, and none of the console is in the firmware. It's on
by default, as development wants it.

QEMU has no USB-Serial/JTAG; built with `DEBUGCON_UART=1` in the
environment, the console is on UART0 instead, which QEMU's `-serial` can
give boxctl as `socket://localhost:PORT`.

## Driving it with shotbox

[shotbox](https://github.com/mishan/shotbox) runs a program in a sealed
X session and takes pictures of it; the simulator's window is found by
its title. A screenshot of a firmware once it's drawn:

    shotbox shoot out.png --window 'boxsim:.*' -- build-sim/hello.elf

Playing it by hand, in a session with sound (`--audio`), from a script
run as `shotbox run --audio -- sh script.sh`:

    build-sim/hello.elf & sim=$!
    shotbox wait window 'boxsim:.*'
    shotbox click 200 300 --window 'boxsim:.*' --hold 2 &   # a finger, 2 s
    shotbox record touch.wav --seconds 1.5                   # what it plays
    shotbox key --hold 0.9 s                                 # the side button
    shotbox key Right Right                                  # roll 10 degrees
    shotbox capture after.png --window 'boxsim:.*'
    kill $sim

then `shotbox listen touch.wav` for its level, clipping and dropouts.
The window's coordinates are the screen's times the scale: at the
default 2, the screen's (100, 150) is the window's (200, 300).
`--scale 1` makes a screenshot the BOX's 320x240 itself.

## Tests

    . ~/esp/esp-idf/export.sh
    cmake -B build -G Ninja && ctest --test-dir build --output-on-failure

`boxctl` checks boxctl's reading against `tests/fakebox.c`, a fake
console that writes frames with `debugframe.h`. `hello-build` builds
`examples/hello` for linux, and `hello` runs it: headless with a
script, checking the bars, a touch's dot and the level after a roll in
its screenshots, and the beep's timing, level and pitch in its WAV; the
beep again at 48 kHz, played by SDL's dummy video and disk audio
drivers; and with boxctl over the console's pty: tapping, taking a
screenshot, a line too long, and streaming audio to a reader that
stalls or reads too slowly. `hello-nocon-build` builds it without the
console, which `hello` checks isn't there.

## Third-party

boxsim builds on these, as dependencies; none of their source is in this
repository.

- [ESP-IDF](https://github.com/espressif/esp-idf), Apache-2.0
- its FreeRTOS kernel, MIT
- [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev),
  Apache-2.0, fetched by the component manager for the BOX only
- [SDL2](https://www.libsdl.org/) (or sdl2-compat over SDL3), zlib
- [pyserial](https://github.com/pyserial/pyserial), BSD-3-Clause, for boxctl

A firmware built with boxsim, for the BOX or the desktop, links ESP-IDF
and carries its Apache-2.0 terms as well as boxsim's.

## License

MIT; see LICENSE.
