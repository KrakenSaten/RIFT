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
  out=$("$OUT/$name.exe" 2>&1)
  n=$(echo "$out" | grep -Eo '^\[==========\] [0-9]+ test' | head -1 | grep -Eo '[0-9]+')
  if echo "$out" | grep -q '\[  FAILED  \]'; then
    echo "$name: TESTS FAILED"; echo "$out" | grep -E '^\[  FAILED  \]' | head -20; fail=1
  else
    printf '%-26s %s tests passed\n' "$name" "${n:-?}"
    total=$((total + ${n:-0}))
  fi
done
echo "----"
echo "$total tests passed"
exit $fail
