#!/usr/bin/env python3
"""Flash the RIFT firmware, splitting the write.

A single write of the whole app partition fails part way through with the USB
device dropping off the bus - see BUILDING.md for what was ruled out. Splitting
it into shorter writes works, because the threshold is the length of one transfer
rather than any address.

That threshold moves. 400 KB pieces were fine, then 200 KB, and then 188 KB
failed on the same board where 64 KB went straight through. So the size is not
something to remember: a failed write halves it and starts the image over, rather
than stopping with half an image on the device.

This lived as a local PowerShell script on one machine and had to be rebuilt by
hand on the next. It is in the repo so that stops happening.

    python tools/rift-flash.py --port COM5
    python tools/rift-flash.py --port COM5 --single    # try one write anyway
    python tools/rift-flash.py --dry-run               # build the chunks only

Every run ends by comparing the whole app region against the file: per-chunk
hashes have reported success on a flash the device was not running.

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
            return True
        if attempt < attempts:
            # the port often needs a moment before it will open again
            print("  attempt %d failed, retrying in 3s" % attempt)
            time.sleep(3)
    return False


def write_all(base, paths):
    """Write every chunk. False if one of them would not go through."""
    for addr, path, _ in paths:
        print("writing 0x%X" % addr)
        if not run(base + ["write_flash", hex(addr), path]):
            return False
    return True


def split_image(image, args, chunk_kb=None):
    """Cut the image into sector-aligned pieces and write them out as files."""
    kb = chunk_kb if chunk_kb else args.chunk_kb
    parts = args.parts
    if parts <= 0:
        target = max(kb, 1) * 1024
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

    total = sum(n for _, _, n in paths)
    if total != len(image):
        sys.exit("chunks total %d but image is %d - refusing to write" % (total, len(image)))
    print("%d chunks of about %d KB, covering the image exactly (%d bytes)"
          % (len(paths), kb, total))
    return paths


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. COM5")
    ap.add_argument("--firmware", default=".pio/build/LilyGo_TDeck_rift/firmware.bin")
    ap.add_argument("--single", action="store_true", help="one write instead of two")
    ap.add_argument("--parts", type=int, default=0,
                    help="number of writes to split into (default: from --chunk-kb)")
    ap.add_argument("--chunk-kb", type=int, default=64,
                    help="target size of each write in KB (default 64); halved automatically if a write fails")
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
    paths = split_image(image, args)

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
    # Halve the chunk size and start over rather than stopping with a partial
    # image on the device.
    #
    # The failure is a USB drop part way through one transfer, and the threshold
    # is the transfer length rather than the address - so the same image that will
    # not go in 188 KB pieces goes in 64 KB pieces on the next try. That threshold
    # moves: it was fine at 400 KB, then at 200 KB, and today 188 KB failed and 64
    # KB worked on the same board. Anything a human has to remember to pass here
    # is something that will be forgotten on the day the device is half-written.
    kb = args.chunk_kb
    while True:
        if write_all(base, paths):
            break
        if args.single or args.parts > 0 or kb <= 16:
            if _chunks_written:
                sys.exit("write failed and the image is now partly written, so run "
                         "this again before power-cycling")
            # Nothing reached the chip in THIS run. A previous run may still have
            # left a partial image, which is why this does not claim the device is
            # in any particular state.
            sys.exit("write failed and no chunk was written by this run")
        kb = kb // 2
        print("write failed at about %d KB per chunk - retrying the whole image "
              "at about %d KB" % (kb * 2, kb))
        paths = split_image(image, args, kb)
    print("done - %d write%s" % (len(paths), "" if len(paths) == 1 else "s"))

    # Per-chunk hashes are not proof that the device is running this image.
    #
    # A flash of the repeater-control build reported nine writes with every hash
    # verified, and the device kept running the previous version - reading app0
    # back afterwards found the old strings and none of the new ones. Whatever
    # went wrong, "done" was not evidence, and there was no way to tell from the
    # output that anything had.
    #
    # So the whole app region is compared against the file as a final step. This
    # is the only check here that answers the actual question: is what is in
    # flash the image that was just built.
    print("verifying the whole image against flash")
    if subprocess.call(base + ["verify_flash", hex(APP_OFFSET), args.firmware]) != 0:
        sys.exit("VERIFY FAILED - flash does not match %s, so the device is not "
                 "running this build. Run again." % args.firmware)
    print("verified: flash matches the built image")

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
        # The read above already said which slot is selected, so say what that
        # means rather than a generic warning. "may still boot the other slot"
        # when the selection was just read as app0 is alarming and wrong, and
        # this tool has cost hours by being alarming and wrong before.
        if slot == 0:
            print("  could not clear otadata, but it already selects app0 - "
                  "which is where this image was written")
        else:
            print("  FAILED to clear otadata, and it selects app%d - the device "
                  "will boot the other slot, so this image will not run" % slot)
        return
    print("  otadata cleared: the bootloader now takes app0, which is what was written")
    os.remove(dump)


if __name__ == "__main__":
    main()
