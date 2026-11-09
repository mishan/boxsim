# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Misha Nasledov. See LICENSE.
"""examples/hello in the simulator, headless: what it draws and plays, as
the sim's script and WAV see it and as boxctl sees it over the console's
pty. $HELLO_ELF is the linux build, $HELLO_OUT where its files go."""

import array
import os
import re
import subprocess
import sys
import time
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import boxctl  # noqa: E402

ELF = os.environ.get("HELLO_ELF", "")
OUT = os.environ.get("HELLO_OUT", ".")
W, H = 320, 240
BARS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255),
        (255, 255, 0), (0, 255, 255), (255, 0, 255), (0, 0, 0)]
WHITE, BLACK, GRAY = (255, 255, 255), (0, 0, 0), (132, 130, 132)


def ppm(path):
    data = open(path, "rb").read()
    head = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    assert (int(head[1]), int(head[2])) == (W, H)
    rgb = data[head.end():]
    return lambda x, y: tuple(rgb[3 * (y * W + x):3 * (y * W + x) + 3])


def tone(pcm, rate, t0, t1):
    """The peak and the frequency, by zero crossings, between t0 and t1 s."""
    seg = pcm[int(t0 * rate):int(t1 * rate)]
    crossings = sum(1 for a, b in zip(seg, seg[1:]) if a < 0 <= b)
    return max(abs(v) for v in seg), crossings / (t1 - t0)


needs_serial = unittest.skipUnless(
    subprocess.run([sys.executable, "-c", "import serial"], capture_output=True).returncode == 0,
    "no pyserial")


@unittest.skipUnless(os.path.exists(ELF), "no $HELLO_ELF")
class Hello(unittest.TestCase):
    def test_script(self):
        a, b, w = (os.path.join(OUT, n) for n in ("a.ppm", "b.ppm", "hello.wav"))
        for f in (a, b, w):
            if os.path.exists(f):
                os.remove(f)
        subprocess.run([ELF, "--headless", "--script",
                        f"400 shot {a}; 500 down 60 50; 700 up; 900 roll 30; "
                        f"1200 shot {b}; 1300 quit"],
                       env=dict(os.environ, BOXSIM_AUDIO="wav:" + w),
                       check=True, timeout=20, capture_output=True)

        before, after = ppm(a), ppm(b)
        self.assertEqual([before(40 * k + 20, 100) for k in range(8)], BARS)
        self.assertEqual(before(60, 50), BARS[1])
        self.assertEqual((before(160, 220), before(140, 220)), (BLACK, GRAY))
        self.assertEqual(after(60, 50), WHITE)
        # Rolled 30 degrees, the mark half way to the right end.
        self.assertEqual((after(236, 220), after(160, 220)), (BLACK, GRAY))

        with wave.open(w) as f:
            rate = f.getframerate()
            self.assertEqual((rate, f.getnchannels()), (16000, 2))
            pcm = array.array("h", f.readframes(f.getnframes()))[0::2]
        # Down from 0.5 s to 0.7 s, give or take the 50 ms written ahead.
        self.assertEqual(tone(pcm, rate, 0.0, 0.35)[0], 0)
        peak, hz = tone(pcm, rate, 0.58, 0.64)
        self.assertEqual(peak, 8000)
        self.assertAlmostEqual(hz, 880, delta=40)
        self.assertEqual(tone(pcm, rate, 0.85, 1.1)[0], 0)

    def test_48k_on_a_device(self):
        """At 48 kHz, played on a device (SDL's disk driver) rather than
        paced to the clock: the beep on time, from the device's ring."""
        raw = os.path.join(OUT, "hello48.raw")
        if os.path.exists(raw):
            os.remove(raw)
        subprocess.run([ELF, "--script", "500 down 60 50; 700 up; 1100 quit"],
                       env=dict(os.environ, HELLO_RATE="48000", SDL_VIDEODRIVER="dummy",
                                SDL_AUDIODRIVER="disk", SDL_AUDIO_DRIVER="disk",
                                SDL_DISKAUDIOFILE=raw, SDL_AUDIO_DISK_OUTPUT_FILE=raw),
                       check=True, timeout=20, capture_output=True)
        with open(raw, "rb") as f:
            pcm = array.array("h", f.read())[0::2]
        self.assertEqual(tone(pcm, 48000, 0.0, 0.35)[0], 0)
        peak, hz = tone(pcm, 48000, 0.58, 0.64)
        self.assertGreater(peak, 7900)
        self.assertAlmostEqual(hz, 880, delta=40)

    def console(self):
        """hello run headless, and its console's pty, opened."""
        import serial
        sim = subprocess.Popen([ELF, "--headless"], stdout=subprocess.PIPE, text=True)
        self.addCleanup(sim.wait)
        self.addCleanup(sim.kill)
        for line in sim.stdout:
            m = re.match(r"debugcon: on (\S+)", line)
            if m:
                break
        return boxctl.Box(serial.serial_for_url(m[1], 115200, timeout=0.05))

    @needs_serial
    def test_console(self):
        box = self.console()
        self.assertEqual(box.ask("tap 300 100"), "ok")
        time.sleep(0.1)
        shot = boxctl.Shot()
        box.ask("screenshot", 10, shot.add)
        rgb = boxctl.rgb565be_to_rgb(shot.pixels())
        px = lambda x, y: tuple(rgb[3 * (y * W + x):3 * (y * W + x) + 3])
        self.assertEqual((px(300, 100), px(300, 120), px(20, 100)), (WHITE, BARS[7], BARS[0]))

    @needs_serial
    def test_console_reads_commands_while_a_slow_link_streams(self):
        """Audio read at 25 KB/s, slower than the 66 KB/s it streams at:
        `audio off', sent once the pty's buffer is full, is still read, and
        every byte sent is a whole frame."""
        box = self.console()
        self.assertEqual(box.ask("audio on"), "16000 Hz, 2 channels")
        replies, text, asked, start = [], [], False, time.monotonic()
        while not replies and time.monotonic() - start < 12:
            if not asked and time.monotonic() - start > 3:
                box.ser.write(b"audio off\n")
                asked = True
            for e in box.reader.feed(box.ser.read(256)):
                if isinstance(e, str):
                    text.append(e)
                elif e.kind == "T":
                    replies.append(e.payload.decode())
            time.sleep(0.01)
        self.assertEqual(text, [])
        self.assertRegex(" ".join(replies), r"^audio off: \d+ blocks sent")

    @needs_serial
    def test_console_ends_the_stream_when_the_link_stalls(self):
        box = self.console()
        box.ask("audio on")
        time.sleep(2.5)
        self.assertIn("audio off", box.ask("status"))

if __name__ == "__main__":
    unittest.main()
