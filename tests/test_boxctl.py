# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Misha Nasledov. See LICENSE.
"""boxctl.py read against tests/fakebox.c's streams, which frame what they
send with the firmware's own debugframe.h. Builds fakebox into
$FAKEBOX_DIR (else a temporary directory) with cc:
    python3 -m unittest tests/test_boxctl.py"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import boxctl  # noqa: E402

W, H = 320, 240


def fake(args):
    exe = os.path.join(os.environ.get("FAKEBOX_DIR") or tempfile.mkdtemp(), "fakebox")
    if not hasattr(fake, "built"):
        subprocess.run(["cc", "-std=c99", "-Wall", "-Werror", "-o", exe,
                        os.path.join(HERE, "fakebox.c")], check=True)
        fake.built = exe
    return subprocess.run([fake.built] + args, check=True, capture_output=True).stdout


def read(stream, chunk):
    reader, out = boxctl.Reader(), []
    for i in range(0, len(stream), chunk):
        out += reader.feed(stream[i:i + chunk])
    return out


def pattern():
    """fakebox's screen as RGB: (x, y, x ^ y) cut to 5, 6 and 5 bits and
    their top bits repeated below them."""
    out = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = (x & 0xff) >> 3, y >> 2, ((x ^ y) & 0xff) >> 3
            out += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    return bytes(out)


# Each: what fakebox sends; the log lines read; the screenshot, or what's
# wrong with it; the audio blocks and how many of them missing; replies.
CASES = [
    ("log around and inside frames",
     ["log", "boot", "half", "dxbell load 12%", "shot", "log", "after",
      "reply", "screenshot: 0, 320x240"],
     ["boot", "dxbell load 12%", "after"], "ok", None, ["screenshot: 0, 320x240"]),
    ("audio with a block not sent",
     ["log", "a", "audio", "10", "4", "log", "b"], ["a", "b"], None, (10, 1), []),
    ("audio with a block cut short",
     ["cut", "3", "audio", "8", "99", "log", "b"], ["b"], None, (8, 1), []),
    ("screenshot with a piece garbled",
     ["corrupt", "5", "shot", "log", "after"], ["after"], "piece at 4096 missing", None, []),
    ("screenshot header garbled",
     ["corrupt", "0", "shot", "log", "after"], ["after"], "no screenshot header", None, []),
]


class TestReader(unittest.TestCase):
    def test_streams(self):
        for name, args, lines, shot, audio, replies in CASES:
            stream = fake(args)
            for chunk in (1, 7, 4096, len(stream)):
                with self.subTest(name, chunk=chunk):
                    events = read(stream, chunk)
                    got = [e for e in events if isinstance(e, str)]
                    frames = [e for e in events if not isinstance(e, str)]
                    if "cut" in args or "corrupt" in args:
                        # Each a line's end, in order, among the garbled
                        # frame's bytes read as text.
                        ends = iter(got)
                        for want in lines:
                            self.assertTrue(any(g.endswith(want) for g in ends), (want, got))
                    else:
                        self.assertEqual(got, lines)
                    self.assertEqual([f.payload.decode() for f in frames if f.kind == "T"],
                                     replies)
                    if shot is not None:
                        s = boxctl.Shot()
                        for f in frames:
                            s.add(f)
                        if shot == "ok":
                            self.assertEqual(boxctl.rgb565be_to_rgb(s.pixels()), pattern())
                        else:
                            with self.assertRaisesRegex(ValueError, shot):
                                s.pixels()
                    if audio is not None:
                        a = boxctl.Audio()
                        for f in frames:
                            a.add(f)
                        blocks, missing = audio
                        self.assertEqual((a.frames(), a.missing), (blocks * 128, missing))
                        samples = struct.unpack(f"<{blocks * 256}h", a.pcm())
                        gaps = [b for b in range(blocks)
                                if samples[b * 256:(b + 1) * 256] == (0,) * 256]
                        self.assertEqual(len(gaps), missing)
                        for b in range(blocks):
                            if b not in gaps:
                                self.assertEqual(samples[b * 256:(b + 1) * 256],
                                                 tuple(range(b * 256, (b + 1) * 256)))


class FakeSerial:
    def __init__(self, data):
        self.data, self.sent = data, b""

    def read(self, n):
        out, self.data = self.data[:n], self.data[n:]
        return out

    def write(self, b):
        self.sent += b


class TestAsk(unittest.TestCase):
    def test_reply_to_this_command_not_one_before(self):
        box = boxctl.Box(FakeSerial(fake(["reply", "tap 1 2: ok", "log", "x",
                                          "reply", "status: screen life"])))
        self.assertEqual(box.ask("  status "), "screen life")
        self.assertEqual(box.ser.sent, b"status\n")


if __name__ == "__main__":
    unittest.main()
