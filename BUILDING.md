# Building RIFT

Moved out of the README, which was carrying this alongside three other jobs and
had already drifted from `HANDOFF.md` twice. **`HANDOFF.md` is the working notes
and the more detailed of the two** — if the two disagree, that one is closer to
what someone actually hit.

---

## Environment

Three things that will otherwise waste your time.

**1. PlatformIO in its own virtual environment.** Installing it into a shared or
global Python can upgrade packages other tools depend on — it has happened here.

```bash
python -m venv C:/dev/RIFT/.venv
C:/dev/RIFT/.venv/Scripts/python.exe -m pip install platformio
```

**2. `PLATFORMIO_CORE_DIR` must be an ASCII-only path.** The Windows toolchain
fails to find headers if the core directory contains non-ASCII characters — for
example a user profile like `C:\Users\AndréWågen\.platformio`. The failure
presents as missing `Arduino.h` / `Stream.h`, which is thoroughly misleading.

```bash
export PLATFORMIO_CORE_DIR="C:/dev/.platformio"
```

**3. The unit tests need a host compiler**, which PlatformIO does not bring with
it. Without one, `pio test -e native` fails with `'g++' is not recognized` and the
suite can only run in CI — which is how a missing `main()` once reached the
branch. On Windows, MSYS2 is the shortest route: install it from
[msys2.org](https://www.msys2.org), then from the UCRT64 shell

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

and put `C:\msys64\ucrt64\bin` on PATH. Verified with GCC 16.1.0 and 16.2.0.

If you would rather not install a toolchain, `tools/run-native-tests.sh` builds
the same suites with `zig c++` from the `ziglang` pip package, which is already
in the venv. It mirrors both native environments and prints one line per suite.

---

## Commands

```bash
pio test -e native -e native_kiss_modem
```

They cover the places that have actually been wrong — base64 key
validation, `path_len` decoding, hash collision resolution, the mesh-activity
thresholds, screen-transition hooks, the UTF-8 to CP437 translation, the origin
decoration, the channel colours — rather than aiming at coverage. A count here
dates quickly; `pio test` is the answer.

```bash
pio run -e LilyGo_TDeck_rift
```

**Always build all five T-Deck environments before committing.** RIFT edits shared
code — `MyMesh` and `AbstractUITask` are compiled into every companion-radio
build — so the four stock environments are the regression check:

```bash
pio run -e LilyGo_TDeck_rift -e LilyGo_TDeck_companion_radio_usb -e LilyGo_TDeck_companion_radio_ble -e LilyGo_TDeck_repeater -e LilyGo_TDeck_kiss_modem
```

### A single image for a web flasher

`firmware.bin` is the app alone and belongs at `0x10000` — which is only correct on
a device whose partition table already matches this one. RIFT's table is its own:
app0 6.2 MB at `0x10000`, app1 at `0x650000`, spiffs 3.4 MB at `0xC90000`,
coredump at `0xFF0000`, so it wants the 16 MB flash the original T-Deck has. Give
anyone the app alone and it lands in whatever table is already on their device.

What a flasher wants is the merged image, which carries the bootloader, the
partition table, `boot_app0` and the app in one file, **flashed to `0x0`**:

```bash
pio run -e LilyGo_TDeck_rift -t mergebin
```

`mergebin` is a custom target, not a post-build step — `merge-bin.py` registers it
that way — so a plain `pio run` does not produce it and the file is simply absent
until asked for. `rift-release.yml` calls it and publishes the result as
`rift-merged.bin`; `flasher/manifest.json` is where the `0x0` offset is declared
for ESP Web Tools.

Worth telling anyone you hand the file to: erase the flash on a first install.
Leftover `nvs` and `spiffs` from a different partition layout is a source of
behaviour nobody can explain afterwards, and the project's own manifest sets
`new_install_prompt_erase` for that reason.

---

## Uploading

```bash
pio run -e LilyGo_TDeck_rift -t upload --upload-port COM5
```

**"Could not open COM5, the port doesn't exist" while the port is clearly
listed?** Something is holding it. Usually a `pio device monitor`, or the browser
tab with the web flasher — WebSerial keeps the port until the tab closes.

**Upload dies partway through, with the USB device dropping off the bus?** Use the
tool, not `pio run -t upload`.

The venv interpreter is not optional here, and neither is exporting
`PLATFORMIO_CORE_DIR` from step 2: esptool imports pySerial from whichever
interpreter runs the tool, and the core dir decides *which* esptool is found. A
bare `python` on a machine with more than one costs an hour, because the failure
arrives as twelve tracebacks above a summary that says no chunk was written -
which reads as a board that will not take an image. The tool now refuses that case
up front instead of retrying it at three chunk sizes.

```bash
.venv/Scripts/python.exe tools/rift-flash.py --port COM5
```

`pio run -t upload` cannot write the whole app partition in one operation on this
device. It fails part way through with a pySerial `PermissionError` on reopening
the port, or `The chip stopped responding`.

**The threshold is the transfer length, not the flash address** — established by
splitting at a different boundary and watching the same address go through. So it
is the transport rather than the image.

**The split is automatic and self-correcting; you should not need `--parts` or
`--chunk-kb`.** The tool aims for 64KB per write, computes the count from the
image size, and on a failed write halves the size and starts the image over
rather than stopping with half an image on the device.

The threshold moves, which is why it is not a number to remember. 405KB per write
was fine at a 1.54MB image. At 1.55MB, 400KB failed on the third write while
200KB went through. Later 188KB failed on the same board where 64KB succeeded
immediately. Every fixed value here has stopped working eventually.

**Every run ends by comparing the whole app region against the file.** Per-chunk
hashes are not evidence: a flash once reported nine writes with every hash
verified while the device kept running the previous version, which cost two days
of debugging a feature that was never on the device.

Ruled out **for this failure**, so nobody repeats them: a different cable,
`upload_speed` (baud is a no-op on native USB CDC), and `--no-stub` — which only
moved the failure earlier. Note that `--no-stub` *is* the fix for the other upload
failure below; it does nothing for this one. The cause is still unknown; the first
upload of a session has sometimes gone through whole, which argues for something
state-dependent.

A failed chunk is retried four times, three seconds apart, because the error is
often transient. If it exhausts the retries the image is partly written and the
device will not boot — **run the tool again before power-cycling.** It is not
bricked: the ROM bootloader is in ROM, untouched, and the port still enumerates.

`--chunk-kb` overrides the target size and `--parts N` forces a count. `--dry-run`
writes the chunks out and checks they cover the image without touching the device.
`--single` forces one write, which is how the failure was characterised and the
cheap way to retest it — **the day `--single` succeeds, this tool has outlived its
purpose.**

**Two different upload failures, and they need different flags.** Read the output
before reaching for either - they look nothing alike.

*Part way through a write*, after several chunks have gone through: that is the
transfer-length problem above. The retry usually clears it; a smaller `--chunk-kb`
if it does not.

**Black screen and dead touch after a flash that reported success?** Fixed, but
worth knowing the shape of it. esptool resets the chip into the ROM download
loader to work and hard-resets out of it when it finishes — *when it finishes*. An
invocation that fails never reaches its reset, and the last thing the tool did was
`erase_region` on otadata, which fails under `--no-stub`. So a flash that verified
perfectly left the chip parked in the loader: screen black, touch dead, USB still
enumerated, nothing in the output saying so. It looks exactly like a firmware that
will not boot.

Power-cycling fixes it, which is why it took two rounds to notice. The tool now
ends with `run` in a `finally`, so it happens after a failed write and a failed
verify too.

To tell the two apart: talk to the chip *without* a reset.

```bash
.venv/Scripts/python.exe $PLATFORMIO_CORE_DIR/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM5 --before no_reset chip_id
```

If it answers, the chip is in the loader and the image is probably fine — only a
chip already sitting there responds with no reset handshake. If it cannot connect,
something is running and the black screen is the firmware's problem.

*At the stub, on the very first chunk, every retry*: esptool connects, prints the
chip revision and MAC, uploads and runs the stub, and then says
`Unable to verify flash chip connection (No serial data received)`. The reset
handshake is fine and the stub is what goes quiet. Pass `--no-stub`:

```bash
.venv/Scripts/python.exe tools/rift-flash.py --port COM5 --no-stub
```

That drives the ROM bootloader directly. Slower, which is why it is not the default.

Isolate it with one command rather than guessing, since it says whether the device
can be talked to at all:

```bash
.venv/Scripts/python.exe $PLATFORMIO_CORE_DIR/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM5 chip_id
```

If that reports the MAC and then fails, it is the stub. If it never connects,
something else has the port.

Note that a failure at the connection stage has written **nothing**, whatever the
tool's exit message says - it warns about a partly written image because that is the
common case, not because it checked.

**First boot after switching firmware may reformat SPIFFS**, which takes one to
two minutes. The boot screen says `Formatting SPIFFS` when that is happening —
wait it out rather than resetting. An ordinary boot takes about five seconds; if it
takes materially longer, SYSTEM shows `boot:` and `slowest:` so the phase
responsible can be named rather than guessed at.

---

## Screenshots

The device's own pixels, over USB, as PNG at 1× and 2×:

```bash
.venv/Scripts/python.exe tools/rift-screenshot.py --port COM8 --all --tag night
```

`--all` walks the five nav screens; `--screen current` captures whatever is
showing, overlays and popups included, so the repeater panel or the conversation
list is captured by putting it up on the device first. Night and day are the
device's own setting - toggle on SYSTEM and capture again with another `--tag`.
Files land in `design/screens/`. The firmware side is a companion command
(`ui-rift/RiftScreenDump.h`); the device has to be in companion mode, not CLI
rescue, and the port is opened with DTR and RTS low so the chip is not reset.

---

## Other environments

`LilyGo_TDeck_rift` is additive. The four upstream T-Deck environments
(`_companion_radio_usb`, `_companion_radio_ble`, `_repeater`, `_kiss_modem`) are
unchanged and still build. The UI is selected purely by build flags: `ui-new`,
`ui-orig`, `ui-tiny` and `ui-rift` are parallel implementations behind the same
`UIScreen` seam.

Feature flags: `RIFT_DISPLAY`, `RIFT_INPUT_KEYBOARD`, `RIFT_INPUT_TRACKBALL`,
`RIFT_INPUT_TOUCH`, `RIFT_RADAR`. Each guards its own code, so features can be
disabled independently when bisecting a problem.

**Warnings are on for RIFT's own code** (`-Wall -Wextra`, with `-w` unflagged) and
it compiles clean. `-Werror` is off while ~210 warnings in shared MeshCore code
are not ours to fix.

---

## CI

`.github/workflows/rift-build-check.yml` runs on push and pull request against
`rift-tdeck`: it builds `LilyGo_TDeck_rift` plus all four stock T-Deck
environments, and runs the native unit tests. Upstream's own `pr-build-check.yml`
triggers only on `main`/`dev` and its matrix contains no T-Deck environment at
all, so before this file nothing in CI compiled RIFT. It is a separate workflow
rather than an edit to upstream's, so merges from upstream stay conflict-free.

`rift-release.yml` gates the builds on the tests, publishes the browser flasher
from the same binary attached to the release, and refuses a release if the tag and
`RIFT_VERSION` disagree.

### Supply chain

Third-party actions in RIFT's own workflows are pinned to commit SHAs, with the
version noted beside each. A tag is a movable pointer, and these workflows are
what produce the binaries people flash. Permissions are read-only by default; only
the job that creates the release and pushes to `gh-pages` is granted write.

Two gaps, stated rather than papered over:

- The shared `.github/actions/setup-build-environment` is upstream's and still
  refers to `actions/cache@v5` and `actions/setup-python@v6` by tag. Pinning it
  there would diverge from upstream and change their workflows too.
- The browser flasher loads ESP Web Tools 10.4.0 from unpkg. The version is
  pinned, but the `?module` form resolves that library's own dependencies from the
  CDN at load time using ranges. Removing that runtime dependency means bundling
  the library into this repository, which needs an npm build step that does not
  exist here.
