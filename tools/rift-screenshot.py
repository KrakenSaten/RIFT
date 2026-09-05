#!/usr/bin/env python3
"""Capture the T-Deck's screen over USB as PNG, at 1x and 2x.

The device's own pixels, not a photograph: the firmware answers a companion
command with its composed 320x240 RGB565 frame (see ui-rift/RiftScreenDump.h).
Needs the project venv, which has pyserial and Pillow.

    .venv/Scripts/python.exe tools/rift-screenshot.py --port COM8 --screen nodes
    .venv/Scripts/python.exe tools/rift-screenshot.py --port COM8 --all --tag night

--screen current captures whatever is showing, overlays and popups included,
which is how the repeater panel, the conversation list and the alert box are
captured: put them up on the device, then run this. --all walks the five nav
screens in order. Day and night are the device's own setting; toggle it on SYSTEM
and capture again with a different --tag.

The port is opened with DTR and RTS low. Opening it with them asserted, as most
terminal programs do, resets the chip, and a reset is not what a screenshot wants.
"""
import argparse, os, struct, sys, time

import serial
from PIL import Image

SCREENS = {"rift": 0, "nodes": 1, "radar": 2, "comms": 3, "system": 4, "current": 0xFF}
MAGIC = b"RIFTSCRN"
CMD_RIFT_SCREEN_DUMP = 0xF0


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def frame(payload):
    return b"<" + struct.pack("<H", len(payload)) + payload


def capture(s, nav):
    s.reset_input_buffer()
    s.write(frame(bytes([CMD_RIFT_SCREEN_DUMP, nav & 0xFF])))
    buf = b""
    deadline = time.time() + 6.0
    while time.time() < deadline:
        buf += s.read(65536)
        i = buf.find(MAGIC)
        if i < 0:
            # keep the tail in case the marker straddles two reads
            if len(buf) > 8:
                buf = buf[-8:]
            continue
        buf = buf[i + len(MAGIC):]
        while len(buf) < 8 and time.time() < deadline:
            buf += s.read(65536)
        if len(buf) < 8:
            break
        w, h, bpp = struct.unpack("<HHB", buf[:5])
        if bpp != 16 or w == 0 or h == 0 or w > 1024 or h > 1024:
            sys.exit("bad header: %dx%d @ %d bpp" % (w, h, bpp))
        need = w * h * 2
        buf = buf[8:]
        deadline = time.time() + 10.0
        while len(buf) < need and time.time() < deadline:
            buf += s.read(65536)
        if len(buf) < need:
            sys.exit("short frame: %d of %d bytes" % (len(buf), need))
        return w, h, buf[:need]
    sys.exit("no frame from the device - is it running a build with the dump command, "
             "and is it in companion mode rather than CLI rescue?")


def to_image(w, h, raw):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = y * w * 2
        for x in range(w):
            v = raw[row + x * 2] | (raw[row + x * 2 + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            # bit replication, which is what the panel does
            px[x, y] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--screen", choices=sorted(SCREENS), default="current")
    ap.add_argument("--all", action="store_true", help="capture the five nav screens in order")
    ap.add_argument("--out", default="design/screens", help="directory for the PNGs")
    ap.add_argument("--tag", default="", help="appended to the filename, e.g. night or day")
    ap.add_argument("--name", default="", help="filename stem for a single capture (default: the screen name)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    targets = [n for n in ("rift", "nodes", "radar", "comms", "system")] if args.all else [args.screen]

    with open_port(args.port) as s:
        time.sleep(0.3)
        for name in targets:
            if args.all and name != targets[0]:
                time.sleep(1.2)   # let the screen settle after the switch
            w, h, raw = capture(s, SCREENS[name])
            img = to_image(w, h, raw)
            stem = args.name or name
            if args.tag:
                stem += "-" + args.tag
            p1 = os.path.join(args.out, stem + ".png")
            p2 = os.path.join(args.out, stem + "-2x.png")
            img.save(p1)
            img.resize((w * 2, h * 2), Image.NEAREST).save(p2)
            print("%s: %dx%d -> %s, %s" % (name, w, h, p1, p2))


if __name__ == "__main__":
    main()
