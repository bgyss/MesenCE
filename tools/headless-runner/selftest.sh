#!/usr/bin/env bash
# Self-test for bin/mesen-headless (mise run test:headless). Uses only ROMs built from source
# in this repository: the NES DMA ROM (cc65) and the fork's SNES/SMS test ROMs.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
bin=bin/mesen-headless
work="$(mktemp -d "${TMPDIR:-/tmp}/mesen-headless-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT
fail=0
check() { if eval "$2"; then echo "pass $1"; else echo "FAIL $1"; fail=1; fi; }

mkdir -p bin/rom
ca65 docs/research/tla/rom/dmc_oam_overlap.s -o bin/rom/dmc_oam_overlap.o
ld65 -C docs/research/tla/rom/nrom.cfg bin/rom/dmc_oam_overlap.o -o bin/rom/dmc_oam_overlap.nes
nes=bin/rom/dmc_oam_overlap.nes
snes=bin/test-roms/sa1_timing.sfc
sms=bin/test-roms/z80_timing.sms

echo '{"frames":{"10":{"1":["START"],"2":["A"]},"12":{"1":[]}}}' > "$work/input.json"
for n in a b; do
  "$bin" run --rom "$nes" --frames 120 --out "$work/nes-$n" --input "$work/input.json" --allow-dirty
done
artifacts() { python3 -c "import json,sys;print(json.dumps(json.load(open(sys.argv[1]))['artifacts']))" "$1"; }
check "NES run is deterministic (all artifact hashes equal)" '[ "$(artifacts "$work/nes-a/manifest.json")" = "$(artifacts "$work/nes-b/manifest.json")" ]'
check "NES output validates" '"$bin" validate "$work/nes-a" >/dev/null 2>&1'

"$bin" run --rom "$snes" --frames 60 --out "$work/snes" --allow-dirty
check "SNES output validates" '"$bin" validate "$work/snes" >/dev/null 2>&1'
"$bin" run --rom "$sms" --frames 60 --out "$work/sms" --allow-dirty
check "SMS output validates" '"$bin" validate "$work/sms" >/dev/null 2>&1'

echo tamper >> "$work/snes/final.png"
check "validate rejects a modified artifact" '! "$bin" validate "$work/snes" >/dev/null 2>&1'

"$bin" run --rom "$nes" --frames 60 --out "$work/setinput" --lua tests/headless/setinput_port.lua --allow-dirty
check "emu.setInput addresses the requested port" 'grep -q "port1.start=false port2.start=true" "$work/setinput/lua.log"'

set +e
"$bin" run --rom "$nes" --frames 60 --out "$work/luaerr" --lua tests/headless/lua_error.lua --allow-dirty
code=$?
"$bin" run --rom "$snes" --frames 100000000 --out "$work/timeout" --timeout 1 --allow-dirty
tcode=$?
"$bin" run --rom "$nes" --frames 10 --out "$work/dirty" >/dev/null 2>&1
dcode=$?
set -e
check "a Lua error fails the run (exit 65, stop code 65)" '[ $code -eq 65 ] && grep -q "\"stop_code\": 65" "$work/luaerr/manifest.json"'
check "timeout is reported as incomplete (exit 3) and fails validation" '[ $tcode -eq 3 ] && ! "$bin" validate "$work/timeout" >/dev/null 2>&1'
if grep -q '"dirty": true' "$work/nes-a/manifest.json"; then
  check "a dirty build refuses a qualified capture" '[ $dcode -ne 0 ]'
fi

mkdir -p "$work/compat/home"
cat > "$work/compat/stop.lua" <<'EOF'
local n = 0
emu.addEventCallback(function() n = n + 1; if n == 30 then emu.stop(0) end end, emu.eventType.endFrame)
EOF
"$bin" nnes-compat "$nes" "$work/compat/home" "$work/compat/stop.lua" "$work/compat/result.json" "$work/compat/audio.wav" 2>/dev/null
check "nativekit NES host contract (nnes-compat)" 'grep -q "\"stop_code\":0" "$work/compat/result.json" && [ -s "$work/compat/final.mss" ] && [ -s "$work/compat/ram.bin" ]'

exit $fail
