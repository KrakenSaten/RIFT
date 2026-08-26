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
import struct
import subprocess
import time
import sys

APP_OFFSET = 0x10000
APP_END = 0x650000         # app0 is 6400 KB; app1 starts here (partition table)
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


_chunks_written = 0


def run(cmd, attempts=4):
    """Write one chunk, retrying a failed attempt.

    The failure this exists for is a pySerial PermissionError on opening the port -
    "the device does not recognise the command" - part way through a multi-chunk
    write. It is transient: the same chunk goes through on a retry seconds later.
    Without a retry the tool exited on the first one and left the device with some
    chunks written and some not, which is a device that does not boot, and the fix
    was to notice and run the whole thing again by hand.

    Each esptool invocation resets the board, so a retry is a clean attempt at that
    chunk rather than a resumption of a partial one.
    """
    global _chunks_written
    print("  " + " ".join(str(c) for c in cmd[2:] if not str(c).endswith("esptool.py")))
    for attempt in range(1, attempts + 1):
        if subprocess.call(cmd) == 0:
            _chunks_written += 1
            return
        if attempt < attempts:
            # the port often needs a moment before it will open again
            print("  attempt %d failed, retrying in 3s" % attempt)
            time.sleep(3)
    if _chunks_written:
        sys.exit("write failed after %d attempts - the image is now partly written, "
                 "so run this again before power-cycling" % attempts)
    # Nothing reached the chip, so the device is untouched. Saying "partly
    # written" here sends the reader looking for a half-flashed device that
    # does not exist.
    sys.exit("write failed after %d attempts and no chunk was written - the "
             "device is untouched" % attempts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. COM5")
    ap.add_argument("--firmware", default=".pio/build/LilyGo_TDeck_rift/firmware.bin")
    ap.add_argument("--single", action="store_true", help="one write instead of two")
    ap.add_argument("--parts", type=int, default=0,
                    help="number of writes to split into (default: from --chunk-kb)")
    ap.add_argument("--chunk-kb", type=int, default=192,
                    help="target size of each write in KB (default 192)")
    ap.add_argument("--dry-run", action="store_true", help="build the chunks, write nothing")
    ap.add_argument("--keep-otadata", action="store_true",
                    help="do not clear the OTA boot selection after writing app0")
    ap.add_argument("--no-stub", action="store_true",
                    help="skip esptool's stub loader; slower, but the only thing that "
                         "works when the stub starts and then goes quiet")
    args = ap.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit("no firmware at %s - build it first" % args.firmware)

    image = open(args.firmware, "rb").read()
    print("firmware: %s (%d bytes, %.2f MB)" % (args.firmware, len(image), len(image) / 1048576.0))

    # Refuse before touching the device rather than half-writing something wrong.
    # 0xE9 is the ESP image magic - the same check that caught a 404 page being
    # served as a firmware binary from the flasher page.
    if not image or image[0] != 0xE9:
        sys.exit("not an ESP32 image: first byte is 0x%02X, expected 0xE9"
                 % (image[0] if image else 0))
    end = APP_OFFSET + len(image)
    if end > APP_END:
        sys.exit("image ends at 0x%X, past the app partition at 0x%X - %d bytes too big"
                 % (end, APP_END, end - APP_END))
    print("fits app0: 0x%X..0x%X of 0x%X (%.0f%% used)"
          % (APP_OFFSET, end, APP_END, 100.0 * len(image) / (APP_END - APP_OFFSET)))

    # Derived from a target chunk size rather than a fixed count, because the count
    # that works is a function of how big the firmware has become. A long write fails
    # part way through - see BUILDING.md - and the threshold is the transfer length,
    # not the address. At 1.54MB four parts was fine; at 1.55MB four parts is 400KB
    # each and fails, while eight parts at 200KB each goes through. A fixed default
    # is therefore something that silently stops working as the image grows, which is
    # the worst kind of default.
    parts = args.parts
    if parts <= 0:
        target = max(args.chunk_kb, 1) * 1024
        parts = max(1, (len(image) + target - 1) // target)

    if args.single or parts <= 1 or len(image) <= 4096:
        chunks = [(APP_OFFSET, image)]
    else:
        # sector-aligned cuts, so no write straddles a 4KB erase boundary
        step = ((len(image) // parts) + 4095) // 4096 * 4096
        chunks = []
        off = 0
        while off < len(image):
            chunks.append((APP_OFFSET + off, image[off:off + step]))
            off += step

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

    # Fail before writing anything if the board is not there.
    # Without this the first chunk burns four attempts and twelve seconds on a
    # port that does not exist, and reports a partly-written image.
    try:
        from serial.tools import list_ports
        ports = [p.device for p in list_ports.comports()]
        if ports and args.port not in ports:
            sys.exit("%s is not connected - available: %s"
                     % (args.port, ", ".join(ports)))
    except ImportError:
        pass  # no pySerial: let esptool report it instead

    # --before default_reset on both halves: leaving the first in the bootloader
    # and reconnecting with --before no_reset was tried, and the reconnect timed
    # out. See BUILDING.md.
    base = esptool_cmd() + ["--chip", CHIP, "--port", args.port, "--before", "default_reset"]

    # A second, different failure than the long-write one above, and it needs a
    # different flag. Symptom: esptool connects, identifies the chip, reads the MAC,
    # uploads and runs the stub - and then "Unable to verify flash chip connection
    # (No serial data received)" on the very first chunk, every retry. So the reset
    # handshake is fine and the stub is what goes quiet.
    #
    # --no-stub drives the ROM bootloader directly and works. It is slower, which is
    # why it is not the default. Reach for it when the failure is at the stub rather
    # than part way through a write - the two look nothing alike in the output.
    if args.no_stub:
        base += ["--no-stub"]
    for addr, path, _ in paths:
        print("writing 0x%X" % addr)
        run(base + ["write_flash", hex(addr), path])
    print("done - %d write%s" % (len(paths), "" if len(paths) == 1 else "s"))

    if not args.keep_otadata:
        settle_boot_slot(base, os.path.dirname(args.firmware) or ".")


# The partition table has two app slots - app0 at 0x10000 and app1 at 0x650000 -
# and an otadata partition that chooses between them. This tool only ever writes
# app0. If otadata selects app1, every write here lands in a partition nothing
# boots: esptool reports success, verifies the hash, and the device keeps running
# whatever is in the other slot. That failure is completely silent, and it looked
# from the outside exactly like a feature that had not been built.
#
# So the boot selection is reported and then cleared. With otadata erased the
# bootloader falls back to the first app partition, which is the one written
# above. Nothing else is touched: nvs keeps the identity and prefs, spiffs keeps
# the contacts and message history. Only the choice of slot is reset.
OTADATA_OFFSET = 0xE000
OTADATA_SIZE = 0x2000


def settle_boot_slot(base, workdir):
    dump = os.path.join(workdir, "rift-otadata.bin")
    if subprocess.call(base + ["read_flash", hex(OTADATA_OFFSET), hex(OTADATA_SIZE), dump]) != 0:
        print("could not read otadata - boot slot not verified")
        return

    try:
        blob = open(dump, "rb").read()
    except OSError:
        print("could not open the otadata dump - boot slot not verified")
        return

    # Two 32-byte entries, one per sector. ota_seq first, then a 20-byte label,
    # then ota_state. An erased sector reads as 0xFFFFFFFF, which means unused.
    seqs = []
    for sector in (0, 0x1000):
        entry = blob[sector:sector + 32]
        if len(entry) < 32:
            seqs.append(None)
            continue
        seq = struct.unpack("<I", entry[0:4])[0]
        seqs.append(None if seq == 0xFFFFFFFF else seq)

    live = [s for s in seqs if s is not None]
    if not live:
        print("boot slot: otadata is empty, so the bootloader already takes app0")
        os.remove(dump)
        return

    # ESP-IDF takes the highest sequence number; the slot is (seq - 1) modulo the
    # number of OTA app partitions, which is two here.
    slot = (max(live) - 1) % 2
    print("boot slot: otadata selects app%d (seq %s)"
          % (slot, ", ".join(str(s) for s in live)))
    if slot != 0:
        print("  app1 was booting, so the write above would have had no effect")

    if subprocess.call(base + ["erase_region", hex(OTADATA_OFFSET), hex(OTADATA_SIZE)]) != 0:
        print("  FAILED to clear otadata - the device may still boot the other slot")
        return
    print("  otadata cleared: the bootloader now takes app0, which is what was written")
    os.remove(dump)


if __name__ == "__main__":
    main()
