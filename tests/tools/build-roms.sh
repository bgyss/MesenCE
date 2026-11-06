#!/usr/bin/env bash
# Build the fork's test ROMs (tests/<system>/src) into bin/test-roms.
# SMS: WLA-DX (wla-z80/wlalink). SNES: cc65 (ca65/ld65). Both come from flake.nix.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/bin/test-roms"
build="$out/build"
mkdir -p "$build"

python3 "$root/tests/tools/font.py" wla > "$build/font.inc"
python3 "$root/tests/tools/font.py" ca65 > "$build/font65.inc"

for src in "$root"/tests/SMS/src/*.s; do
  [ -e "$src" ] || continue
  name="$(basename "$src" .s)"
  (cd "$build" && wla-z80 -I "$build" -o "$name.o" "$src" && printf '[objects]\n%s.o\n' "$name" > "$name.link" && wlalink -r "$name.link" "$out/$name.sms")
  echo "built bin/test-roms/$name.sms"
done

for src in "$root"/tests/SNES/src/*.s; do
  [ -e "$src" ] || continue
  name="$(basename "$src" .s)"
  ca65 -I "$build" -I "$root/tests/SNES/src" -o "$build/$name.o" "$src"
  ld65 -C "$root/tests/SNES/src/${name}.cfg" -o "$out/$name.sfc" "$build/$name.o"
  python3 "$root/tests/tools/snes_checksum.py" "$out/$name.sfc"
  echo "built bin/test-roms/$name.sfc"
done
