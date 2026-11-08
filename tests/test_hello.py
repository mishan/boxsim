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

    @unittest.skipUnless(subprocess.run([sys.executable, "-c", "import serial"],
                                        capture_output=True).returncode == 0, "no pyserial")
    def test_console(self):
        import serial
        sim = subprocess.Popen([ELF, "--headless"], stdout=subprocess.PIPE, text=True)
        try:
            for line in sim.stdout:
                m = re.match(r"debugcon: on (\S+)", line)
                if m:
                    break
            box = boxctl.Box(serial.serial_for_url(m[1], 115200, timeout=0.05))
            self.assertEqual(box.ask("tap 300 100"), "ok")
            time.sleep(0.1)
            shot = boxctl.Shot()
            box.ask("screenshot", 10, shot.add)
            rgb = boxctl.rgb565be_to_rgb(shot.pixels())
            px = lambda x, y: tuple(rgb[3 * (y * W + x):3 * (y * W + x) + 3])
            self.assertEqual((px(300, 100), px(300, 120), px(20, 100)), (WHITE, BARS[7], BARS[0]))
        finally:
            sim.kill()
            sim.wait()


if __name__ == "__main__":
    unittest.main()
