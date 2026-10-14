#!/usr/bin/env bash
# Copies one prepared GBA ROM folder (meta.bin + p000.bin ..., made by tools/prepare_rom.py) to /Apps/Data/GBA/<name> on
# the iPod with mtools, while the iPod is attached to Linux in disk mode and its data partition is NOT mounted.
# Every file is read back and compared by SHA-256. save.sav, state.bin, hot.bin, keep.bin and last.txt are never touched.
# A page that already exists with different content is refused unless --force (a changed ROM invalidates its state.bin).
# Identical files are skipped, so running it twice is harmless.
#
#   sudo ./copy-rom.sh ROMDIR [--name NAME] [--force] --device /dev/disk/by-id/usb-Apple_iPod_<SERIAL>-0:0 \
#        [--usb-serial ID] [--expected-size BYTES]
#   ./copy-rom.sh ROMDIR [--name NAME] [--force] --image rehearsal.img        (FAT32 image, no root, no device)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lines-common.sh
source "$HERE/lines-common.sh"

SRC= NAME= FORCE=0
usage() {
  cat >&2 <<EOF
usage: sudo $0 ROMDIR [--name NAME] [--force] --device DEV [--usb-serial ID] [--expected-size BYTES] [--backup-dir DIR]
       $0 ROMDIR [--name NAME] [--force] --image IMG
EOF
  exit 2
}
[[ $# -ge 1 ]] || usage
SRC=$1; shift
while [[ $# -gt 0 ]]; do
  case $1 in
    --name) NAME=${2:-}; shift 2;;
    --force) FORCE=1; shift;;
    --device) DEVICE=${2:-}; shift 2;;
    --image) IMAGE=${2:-}; shift 2;;
    --usb-serial) USB_SERIAL=${2:-}; shift 2;;
    --expected-size) EXPECTED_SIZE=${2:-}; shift 2;;
    --backup-dir) BACKUP_DIR=${2:-}; shift 2;;
    -h|--help) usage;;
    *) usage;;
  esac
done
ln_need_tools stat od mmd
[[ -d $SRC ]] || die "ROM folder not found: $SRC"
SRC=$(cd "$SRC" && pwd)
[[ -n $NAME ]] || NAME=$(basename "$SRC")
[[ $NAME =~ ^[a-z0-9][a-z0-9_-]{0,30}$ ]] || die "name must be 1-31 chars of a-z 0-9 _ - (got '$NAME')"
[[ -f $SRC/meta.bin && -f $SRC/p000.bin ]] || die "$SRC lacks meta.bin / p000.bin (run tools/prepare_rom.py first)"
[[ $(stat -c %s "$SRC/meta.bin") == 8 ]] || die "meta.bin must be 8 bytes"
SIZE=$(od -An -tu4 -N4 "$SRC/meta.bin" | tr -d ' ')
(( SIZE > 0 && SIZE <= 33554432 )) || die "ROM size in meta.bin out of range ($SIZE)"
NPAGES=$(( (SIZE + 32767) / 32768 ))
pname() { if (( $1 >= 1000 )); then printf 'p%04d.bin' "$1"; else printf 'p%03d.bin' "$1"; fi; }
for ((i = 0; i < NPAGES; i++)); do
  f=$SRC/$(pname "$i")
  [[ -f $f && $(stat -c %s "$f") == 32768 ]] || die "missing or wrong-size page: $(pname "$i")"
done
extra=$(ls "$SRC" | grep -vxE 'meta\.bin|p[0-9]{3,4}\.bin' || true)
if [[ -n $extra ]]; then echo "note: ignoring non-page files: $(echo "$extra" | tr '\n' ' ')"; fi

ln_open_target
DST=/Apps/Data/GBA/$NAME
for d in /Apps/Data /Apps/Data/GBA "$DST"; do ln_exists "$d" || mmd -i "$PART" "::$d"; done
written=0 skipped=0
put() {   # put FILE: copy (or skip if identical) and verify
  local f=$1 h want
  want=$(hash_of "$SRC/$f")
  if ln_exists "$DST/$f"; then
    h=$(ln_read_hash "$DST/$f") || die "cannot read $DST/$f"
    if [[ $h == "$want" ]]; then skipped=$((skipped + 1)); return 0; fi
    [[ $FORCE == 1 ]] || die "$DST/$f exists with different content (use --force; a different ROM invalidates state.bin)"
  fi
  mcopy -o -i "$PART" "$SRC/$f" "::$DST/$f" || die "copy failed: $DST/$f"
  [[ $(ln_read_hash "$DST/$f") == "$want" ]] || die "readback mismatch: $DST/$f"
  written=$((written + 1))
}
put meta.bin
for ((i = 0; i < NPAGES; i++)); do put "$(pname "$i")"; done
ln_flush
echo "ROM_OK: $DST  size=$SIZE pages=$NPAGES written=$written skipped(identical)=$skipped; saves untouched"
