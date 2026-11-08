#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Misha Nasledov. See LICENSE.
"""The BOX's debug console (debugcon.c) from the desktop, over the
USB serial link it logs on: the screen as a PNG, the audio as a WAV, the
touchscreen and buttons played, and the log. See README.md."""

import argparse
import shutil
import struct
import subprocess
import sys
import time
import wave
import zlib

MAGIC = b"\x1eTB"
HEAD = 10
MOST = 1100        # a payload's longest, as debugframe.h has it


class Frame:
    def __init__(self, kind, seq, payload):
        self.kind, self.seq, self.payload = kind, seq, payload

    def __repr__(self):
        return f"Frame({self.kind!r}, {self.seq}, {len(self.payload)} bytes)"


class Reader:
    """Frames and lines of log out of the bytes as they come, in pieces of
    any size: a line, a str; a frame whose CRC holds, a Frame. A frame cut
    short or garbled is read as text, and the search goes on past its
    first byte."""

    def __init__(self):
        self.buf = bytearray()
        self.line = bytearray()

    def _text(self, data, out):
        self.line += data
        while b"\n" in self.line:
            i = self.line.index(b"\n")
            out.append(self.line[:i].decode("utf-8", "replace").rstrip("\r"))
            del self.line[:i + 1]

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                # The start of a frame, perhaps, the rest still to come.
                keep = next((k for k in (2, 1) if self.buf.endswith(MAGIC[:k])), 0)
                self._text(self.buf[:len(self.buf) - keep], out)
                del self.buf[:len(self.buf) - keep]
                return out
            self._text(self.buf[:i], out)
            del self.buf[:i]
            if len(self.buf) < HEAD:
                return out
            n = self.buf[8] | self.buf[9] << 8
            if n <= MOST and len(self.buf) < HEAD + n + 4:
                return out
            if n > MOST or zlib.crc32(self.buf[3:HEAD + n]) != \
                    struct.unpack_from("<I", self.buf, HEAD + n)[0]:
                self._text(self.buf[:1], out)
                del self.buf[:1]
                continue
            out.append(Frame(chr(self.buf[3]), struct.unpack_from("<I", self.buf, 4)[0],
                             bytes(self.buf[HEAD:HEAD + n])))
            del self.buf[:HEAD + n + 4]


class Shot:
    """A screenshot's frames, put together: its header, then its pieces."""

    def __init__(self):
        self.w = self.h = self.size = self.crc = None
        self.pieces = {}

    def add(self, f):
        if f.kind == "S":
            self.w, self.h, self.size, self.crc = struct.unpack_from("<HHII", f.payload)
            self.format = f.payload[12:].decode()
            self.pieces = {}
        elif f.kind == "P" and self.size is not None:
            self.pieces[struct.unpack_from("<I", f.payload)[0]] = f.payload[4:]

    def pixels(self):
        """The image's bytes, or ValueError saying what's wrong with it."""
        if self.size is None:
            raise ValueError("no screenshot header")
        data, at = bytearray(), 0
        while at < self.size:
            if at not in self.pieces:
                raise ValueError(f"screenshot piece at {at} missing")
            data += self.pieces[at]
            at = len(data)
        data = bytes(data[:self.size])
        if zlib.crc32(data) != self.crc:
            raise ValueError("screenshot CRC mismatch")
        if self.format != "rgb565be" or self.size != self.w * self.h * 2:
            raise ValueError(f"screenshot format {self.format} unknown")
        return data


def rgb565be_to_rgb(data):
    out = bytearray(len(data) // 2 * 3)
    for i in range(len(data) // 2):
        v = data[2 * i] << 8 | data[2 * i + 1]
        r, g, b = v >> 11, (v >> 5) & 63, v & 31
        out[3 * i:3 * i + 3] = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return bytes(out)


def png(w, h, rgb):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    rows = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


class Audio:
    """Audio frames put together in order, a gap where blocks went
    missing filled with silence as long as a block, and counted."""

    def __init__(self):
        self.rate = None
        self.blocks = []
        self.next = None
        self.missing = 0

    def add(self, f):
        if f.kind != "A":
            return
        self.rate = struct.unpack_from("<I", f.payload)[0]
        pcm = f.payload[4:]
        if self.next is not None and f.seq != self.next:
            gap = (f.seq - self.next) & 0xffffffff
            if gap > 1 << 31:          # one from before, late: not ours
                return
            self.missing += gap
            self.blocks += [bytes(len(pcm))] * gap
        self.blocks.append(pcm)
        self.next = (f.seq + 1) & 0xffffffff

    def frames(self):
        return sum(len(b) for b in self.blocks) // 4

    def pcm(self):
        return b"".join(self.blocks)


class Box:
    def __init__(self, ser, verbose=False):
        self.ser = ser
        self.reader = Reader()
        self.verbose = verbose

    def events(self, seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            for e in self.reader.feed(self.ser.read(65536)):
                if isinstance(e, str):
                    if self.verbose:
                        print(e, file=sys.stderr)
                else:
                    yield e

    def ask(self, line, seconds=5, each=None):
        """The command sent; its reply, the frames before it to `each'."""
        line = " ".join(line.split())
        self.ser.write(line.encode() + b"\n")
        for f in self.events(seconds):
            if f.kind == "T":
                text = f.payload.decode("utf-8", "replace")
                if text.startswith(line + ": "):
                    return text[len(line) + 2:]
            elif each:
                each(f)
        raise SystemExit(f"boxctl: no reply to '{line}'")


SHOTBOX = (0, 11, 0)       # the first whose compare and listen take these


def shotbox(*args):
    """shotbox (the screenshot and recording checker) run on what boxctl
    wrote; its exit status, or None where it isn't installed."""
    exe = shutil.which("shotbox")
    if not exe:
        print("boxctl: shotbox isn't installed; not checked", file=sys.stderr)
        return None
    said = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout.split()
    try:
        version = tuple(int(v) for v in said[-1].split("."))
    except (IndexError, ValueError):
        version = ()
    if version < SHOTBOX:
        raise SystemExit(f"boxctl: shotbox {'.'.join(map(str, SHOTBOX))} or later wanted, "
                         f"not {' '.join(said) or 'one that says no version'}")
    return subprocess.run([exe] + list(args)).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-p", "--port", default="/dev/ttyACM0",
                    help="a device, or a pyserial URL such as socket://localhost:5555")
    ap.add_argument("-v", "--verbose", action="store_true", help="the log too, to stderr")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("shot")
    s.add_argument("out")
    s.add_argument("--compare", metavar="REF.png", help="shotbox compare it with this")
    s.add_argument("--max", type=int, help="pixels allowed to differ (shotbox's 0)")
    a = sub.add_parser("audio")
    a.add_argument("seconds", type=float)
    a.add_argument("out")
    a.add_argument("--check", action="store_true",
                   help="shotbox listen to it, for dropouts, clipping and silence")
    a.add_argument("--gap", metavar="MS", help="for --check: silence this long is a dropout")
    for name in ("tap", "down", "move"):
        t = sub.add_parser(name)
        t.add_argument("x", type=int)
        t.add_argument("y", type=int)
    for name in ("press", "hold"):
        sub.add_parser(name).add_argument("button", choices=["side"])
    for name in ("up", "home", "shake", "status", "help", "log"):
        sub.add_parser(name)
    sub.add_parser("raw").add_argument("command")
    args = ap.parse_args()

    import serial
    # DTR and RTS left as the port opens them, both on: dropping either
    # alone resets the BOX.
    box = Box(serial.serial_for_url(args.port, 115200, timeout=0.05),
              args.verbose or args.cmd == "log")
    if args.cmd == "log":
        for _ in box.events(float("inf")):
            pass
    elif args.cmd == "shot":
        shot = Shot()
        print(box.ask("screenshot", 15, shot.add))
        try:
            data = shot.pixels()
        except ValueError as e:
            raise SystemExit(f"boxctl: {e}")
        open(args.out, "wb").write(png(shot.w, shot.h, rgb565be_to_rgb(data)))
        if args.compare:
            more = ["--max", str(args.max)] if args.max is not None else []
            sys.exit(shotbox("compare", args.compare, args.out, *more) or 0)
    elif args.cmd == "audio":
        audio = Audio()
        print(box.ask("audio on"))
        for f in box.events(args.seconds + 10):
            audio.add(f)
            if audio.rate and audio.frames() >= audio.rate * args.seconds:
                break
        print(box.ask("audio off"))
        if not audio.rate:
            raise SystemExit("boxctl: no audio came (no speaker?)")
        with wave.open(args.out, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(audio.rate)
            w.writeframes(audio.pcm())
        # A block missing is silence in its place, which listen hears as a
        # gap too: the count says which of its gaps the link made.
        print(f"{audio.frames()} frames at {audio.rate} Hz, "
              f"{audio.missing} blocks missing (silence in their place)")
        gap = ["--gap", args.gap] if args.gap else []
        heard = shotbox("listen", args.out, *gap) if args.check else 0
        if audio.missing or heard:
            sys.exit(1)
    else:
        line = args.command if args.cmd == "raw" else " ".join(
            [args.cmd] + [str(v) for k, v in vars(args).items()
                          if k in ("x", "y", "button")])
        print(box.ask(line, 15))


if __name__ == "__main__":
    main()
