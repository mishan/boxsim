/* SPDX-License-Identifier: MIT
   Copyright (c) 2026 Misha Nasledov. See LICENSE. */
/* A debug console on the link the firmware logs on (the USB-Serial/JTAG
   port; UART0 built with DEBUGCON_UART, for QEMU; a pty in the simulator):
   from the desktop, the screen as drawn, the audio as written to the
   codec, and the touchscreen and buttons played as if by hand.
   tools/boxctl.py drives it; its frames are debugframe.h's, among the
   log's text. See README.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The inputs a firmware reads, besides the side button, which all do. */
enum { DEBUGCON_TOUCH = 1, DEBUGCON_HOME = 2, DEBUGCON_SHAKE = 4 };

/* The console's task, on core 0 below everything else, and its taps on
   box_hal.h, which see what is drawn and played and press as a hand
   would. `status' writes the firmware's own state into a line for
   `status'. */
void debugcon_start (int rate, unsigned inputs, void (*status) (char *line, size_t size));

#ifdef __cplusplus
}
#endif
