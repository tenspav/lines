#!/usr/bin/env bash
# Front-end test: menu, per-ROM saves/states, page cache, memory release (src/lines.c, emulator core stubbed).
# All ROMs are synthetic and generated here. Needs gcc, python3 and the NanoApps SDK headers (hb_* declarations).
#   tests/run_ui_tests.sh --sdk /path/to/NanoApps
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$HERE")
SDK=
while [[ $# -gt 0 ]]; do
  case $1 in
    --sdk) SDK=${2:-}; shift 2;;
    *) echo "usage: $0 --sdk NANOAPPS_DIR" >&2; exit 2;;
  esac
done
[[ -f $SDK/sdk/hb_raw_surface.h ]] || { echo "need --sdk pointing at a NanoApps checkout (sdk/hb_raw_surface.h)" >&2; exit 2; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
D=$W/root/Apps/Data/GBA
P=$REPO/tools/prepare_rom.py; M=$HERE/mkrom.py
mkdir -p "$D" "$W/shots" "$W/roms"
python3 "$M" "$W/roms/big.gba" $((4*1024*1024)) --save flash128
python3 "$M" "$W/roms/ft.gba" 65536 --save flash64
python3 "$M" "$W/roms/hw.gba" 4096 --save none
python3 "$M" "$W/roms/sr.gba" 65536 --save sram
python3 "$P" "$W/roms/big.gba" --name big_rom --out "$D" | grep -E 'save|verif'
python3 "$P" "$W/roms/ft.gba" --name flash_test --out "$D" | grep -E 'save|verif'
python3 "$P" "$W/roms/hw.gba" --name hello_world --out "$D" | grep -E 'save|verif'
python3 "$P" "$W/roms/sr.gba" --name sram_test --out "$D" | grep -E 'save|verif'
# a save.sav that already exists for big_rom (must be preserved)
python3 - "$D/big_rom/save.sav" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes((q * 7) & 0xFF for q in range(0x20000)))
PY
mkdir "$D/huge_rom"; printf '\x00\x00\x10\x02\x03\x00\x00\x00' > "$D/huge_rom/meta.bin"   # 33 MB
head -c 32768 /dev/zero > "$D/huge_rom/p000.bin"
mkdir "$D/empty_dir"                                                                     # junk: no meta
mkdir "$D/no_pages"; printf '\x00\x00\x40\x00\x00\x00\x00\x00' > "$D/no_pages/meta.bin"   # junk: meta without pages
echo "not a rom" > "$D/readme.txt"
echo "stale" > "$D/keep.bin"
gcc -O1 -g -DLINES_HOST -DIDLE_FREE_US=20000000u -Wall -Wno-unused-function -Wno-format-truncation \
    -I"$REPO/src" -I"$SDK/sdk" -o "$W/lines_host" "$HERE/lines_host.c"
RC=0
(cd "$W" && ./lines_host "$W/root" "$W/shots" > "$W/out.log") || RC=$?
grep -E '^(CHECK|bad frees|sizeof|heap|final|  sw|  big_rom)' "$W/out.log"
echo "exit code $RC"
exit $RC
