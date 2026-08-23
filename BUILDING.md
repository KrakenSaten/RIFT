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

---

## Commands

```bash
pio test -e native -e native_kiss_modem
```

133 test cases. They cover the places that have actually been wrong — base64 key
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

---

## Uploading

```bash
pio run -e LilyGo_TDeck_rift -t upload --upload-port COM5
```

**"Could not open COM5, the port doesn't exist" while the port is clearly
listed?** Something is holding it. Usually a `pio device monitor`, or the browser
tab with the web flasher — WebSerial keeps the port until the tab closes.

**Upload dies partway through, with the USB device dropping off the bus?** Use the
tool, not `pio run -t upload`:

```bash
python tools/rift-flash.py --port COM5
```

`pio run -t upload` cannot write the whole app partition in one operation on this
device. It fails part way through with a pySerial `PermissionError` on reopening
the port, or `The chip stopped responding`.

**The threshold is the transfer length, not the flash address** — established by
splitting at a different boundary and watching the same address go through. So it
is the transport rather than the image.

**The split is automatic; you should not need `--parts`.** The tool aims for 192KB
per write and computes the count from the image size, because the count that works
is a function of how big the firmware has become. At 1.54MB, four parts of 405KB
was fine. At 1.55MB, four parts is 400KB each and fails on the third, while eight
parts at 200KB goes through — so a fixed count is a default that silently stops
working as the image grows.

Ruled out, so nobody repeats them: a different cable, `upload_speed` (baud is a
no-op on native USB CDC), and `--no-stub`. The cause is still unknown; the first
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

*At the stub, on the very first chunk, every retry*: esptool connects, prints the
chip revision and MAC, uploads and runs the stub, and then says
`Unable to verify flash chip connection (No serial data received)`. The reset
handshake is fine and the stub is what goes quiet. Pass `--no-stub`:

```bash
python tools/rift-flash.py --port COM5 --no-stub
```

That drives the ROM bootloader directly. Slower, which is why it is not the default.

Isolate it with one command rather than guessing, since it says whether the device
can be talked to at all:

```bash
python $PLATFORMIO_CORE_DIR/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM5 chip_id
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
