#!/usr/bin/env python3
"""Flash the RIFT firmware, splitting the write.

A single write of the whole app partition fails reproducibly at roughly 1.2 MB
with the USB device dropping off the bus - see BUILDING.md for what was ruled
out. Two writes of half the image each complete in a few seconds with their
hashes verified, and esptool verifies per chunk, so the pair covers the whole
image.

This lived as a local PowerShell script on one machine and had to be rebuilt by
hand on the next. It is in the repo so that stops happening.

    python tools/rift-flash.py --port COM5
    python tools/rift-flash.py --port COM5 --single    # try one write anyway
    python tools/rift-flash.py --dry-run               # build the chunks only

--single exists so the workaround can be retested cheaply. The day it succeeds,
this script has outlived its purpose.
"""

import argparse
import glob
import os
import subprocess
import sys

APP_OFFSET = 0x10000
SPLIT_AT = 786432          # 0xC0000, sector-aligned
CHIP = "esp32s3"


def find(pattern, what):
    hits = glob.glob(pattern)
    if not hits:
        sys.exit("could not find %s (looked for %s)" % (what, pattern))
    return hits[0]


def esptool_cmd():
    core = os.environ.get("PLATFORMIO_CORE_DIR", os.path.expanduser("~/.platformio"))
    tool = find(os.path.join(core, "packages", "tool-esptoolpy*", "esptool.py"), "esptool")
    return [sys.executable, tool]


def run(cmd):
    print("  " + " ".join(str(c) for c in cmd[2:] if not str(c).endswith("esptool.py")))
    if subprocess.call(cmd) != 0:
        sys.exit("write failed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. COM5")
    ap.add_argument("--firmware", default=".pio/build/LilyGo_TDeck_rift/firmware.bin")
    ap.add_argument("--single", action="store_true", help="one write instead of two")
    ap.add_argument("--dry-run", action="store_true", help="build the chunks, write nothing")
    args = ap.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit("no firmware at %s - build it first" % args.firmware)

    image = open(args.firmware, "rb").read()
    print("firmware: %s (%d bytes, %.2f MB)" % (args.firmware, len(image), len(image) / 1048576.0))

    if args.single or len(image) <= SPLIT_AT:
        chunks = [(APP_OFFSET, image)]
    else:
        chunks = [(APP_OFFSET, image[:SPLIT_AT]),
                  (APP_OFFSET + SPLIT_AT, image[SPLIT_AT:])]

    out = os.path.join(os.path.dirname(args.firmware) or ".", "rift-chunks")
    os.makedirs(out, exist_ok=True)

    paths = []
    for addr, data in chunks:
        p = os.path.join(out, "0x%X.bin" % addr)
        open(p, "wb").write(data)
        paths.append((addr, p, len(data)))
        print("  0x%06X  %8d bytes  %s" % (addr, len(data), p))

    total = sum(n for _, _, n in paths)
    if total != len(image):
        sys.exit("chunks total %d but image is %d - refusing to write" % (total, len(image)))
    print("chunks cover the image exactly (%d bytes)" % total)

    if args.dry_run:
        print("dry run: nothing written")
        return
    if not args.port:
        sys.exit("--port is required unless --dry-run")

    # --before default_reset on both halves: leaving the first in the bootloader
    # and reconnecting with --before no_reset was tried, and the reconnect timed
    # out. See BUILDING.md.
    base = esptool_cmd() + ["--chip", CHIP, "--port", args.port, "--before", "default_reset"]
    for addr, path, _ in paths:
        print("writing 0x%X" % addr)
        run(base + ["write_flash", hex(addr), path])
    print("done - %d write%s" % (len(paths), "" if len(paths) == 1 else "s"))


if __name__ == "__main__":
    main()
