#!/usr/bin/env bash
# Build and run every native test suite with zig c++ — there is no system g++ on
# the dev box, so `pio test` cannot link here. Mirrors the two native envs in
# platformio.ini: same include paths and same source files for each suite.
# googletest and base64 come from .pio/libdeps/native (fetched by pio once).
set -uo pipefail
cd "$(dirname "$0")/.."

ZIG="$(.venv/Scripts/python.exe -c 'import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),"zig.exe"))')"
GT=.pio/libdeps/native/googletest/googletest
OUT="${TMPDIR:-/tmp}/rift-tests"
mkdir -p "$OUT"

# [env:native] compiles these three; ConfigSerializer.cpp relies on <cstdlib>
# arriving transitively under g++, hence -include cstdlib below.
SRC="src/Utils.cpp src/Packet.cpp src/helpers/ConfigSerializer.cpp"
total=0
fail=0
for dir in test/test_*; do
  name=$(basename "$dir")
  srcs=$(find "$dir" -name '*.cpp' | tr '\n' ' ')
  [ -n "$srcs" ] || continue
  # test_kiss_modem has its own env: different include path and source file.
  if [ "$name" = "test_kiss_modem" ]; then
    incs="-Itest/mocks -Isrc -Iexamples/kiss_modem"
    srcfiles="examples/kiss_modem/KissModem.cpp"
  else
    incs="-Isrc -Itest/mocks"
    srcfiles="$SRC"
  fi
  if ! "$ZIG" c++ -std=c++17 -w -include cstdlib -I"$GT/include" -I"$GT" \
      $incs -I"$dir" -I.pio/libdeps/native/base64/src \
      $srcs $srcfiles "$GT/src/gtest-all.cc" -o "$OUT/$name.exe" >"$OUT/$name.log" 2>&1; then
    echo "$name: BUILD FAILED"; tail -15 "$OUT/$name.log"; fail=1; continue
  fi
  # Run, and give a silent run one more go. A freshly linked binary on Windows is
  # sometimes still locked when it is invoked and produces nothing at all; a real
  # crash reproduces on the retry, so this cannot hide one. Without it the total
  # was intermittently short by whole suites.
  out=$("$OUT/$name.exe" 2>&1)
  rc=$?
  if [ $rc -ne 0 ] || ! echo "$out" | grep -q 'PASSED  \]'; then
    sleep 1
    out=$("$OUT/$name.exe" 2>&1)
    rc=$?
  fi
  # the PASSED line is written last, so it cannot be cut short by interleaving
  n=$(echo "$out" | grep -Eo 'PASSED  \] [0-9]+ test' | tail -1 | grep -Eo '[0-9]+')
  if echo "$out" | grep -q '\[  FAILED  \]'; then
    echo "$name: TESTS FAILED"; echo "$out" | grep -E '^\[  FAILED  \]' | head -20; fail=1
  elif [ $rc -ne 0 ] || [ -z "$n" ]; then
    # No count means the binary never reported a run - it crashed, or was still
    # locked by the linker that had just written it. Counting that as zero passed
    # and carrying on printed a green total that was ten tests short of the truth.
    echo "$name: NO RESULT (exit $rc)"; echo "$out" | tail -5; fail=1
  else
    printf '%-26s %s tests passed\n' "$name" "$n"
    total=$((total + n))
  fi
done
echo "----"
# The verdict goes on the last line, not just in the exit code. Piping this
# through `tail -2` has hidden a failing suite three times: the count came out
# short, the NO RESULT line above was cut off, and the pipe replaced the exit
# status with tail's. A summary that reads "206 tests passed" when a suite did
# not run is the same class of mistake as a measurement that cannot fail.
if [ $fail -ne 0 ]; then
  echo "FAILED - $total tests passed, but at least one suite did not"
else
  echo "$total tests passed"
fi
exit $fail
