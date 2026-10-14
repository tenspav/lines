#!/usr/bin/env bash
# Removes Lines: writes a pack that does NOT contain it (--pack, built beforehand with the SDK's build_apps.py without
# lines, or the "before-lines" backup the installer made) and deletes /Apps/Executables/Lines.hbapp and
# /Apps/Icons/Lines.bin. The current pack is saved to the backup directory first; the new pack is read back and compared by
# SHA-256. It does NOT delete /Apps/Data/GBA (ROM folders, saves and states): your ROMs and saves stay on the iPod.
# It never touches the firmware, the loaders or the marker file.
#
#   sudo ./remove-lines.sh --pack PACK --device /dev/disk/by-id/usb-Apple_iPod_<SERIAL>-0:0 [--usb-serial ID] [--expected-size BYTES]
#   ./remove-lines.sh --pack PACK --image rehearsal.img          (FAT32 image, no root, no device)
# Optional: --pack-sha256 HASH   refuse unless the pack file has exactly this SHA-256.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lines-common.sh
source "$HERE/lines-common.sh"

PACK= PACK_SHA=
usage() {
  cat >&2 <<EOF
usage: sudo $0 --pack PACK --device DEV [--usb-serial ID] [--expected-size BYTES] [--pack-sha256 HASH] [--slot auto|A|B] [--backup-dir DIR]
       $0 --pack PACK --image IMG [--pack-sha256 HASH] [--slot auto|A|B] [--backup-dir DIR]
EOF
  exit 2
}
while [[ $# -gt 0 ]]; do
  case $1 in
    --pack) PACK=${2:-}; shift 2;;
    --pack-sha256) PACK_SHA=${2:-}; shift 2;;
    --device) DEVICE=${2:-}; shift 2;;
    --image) IMAGE=${2:-}; shift 2;;
    --usb-serial) USB_SERIAL=${2:-}; shift 2;;
    --expected-size) EXPECTED_SIZE=${2:-}; shift 2;;
    --slot) SLOT=${2:-}; shift 2;;
    --backup-dir) BACKUP_DIR=${2:-}; shift 2;;
    -h|--help) usage;;
    *) usage;;
  esac
done
[[ -n $PACK && -f $PACK ]] || die "--pack must be an existing pack file that does not contain Lines"
[[ -z $PACK_SHA || $(hash_of "$PACK") == "$PACK_SHA" ]] || die "pack SHA-256 does not match --pack-sha256"
ln_need_tools date
ln_open_target

stamp=$(date -u +%Y%m%dT%H%M%SZ)
ln_snap "$PACK_DST"
ln_snap "/Apps/Executables/$EXE_NAME"
ln_snap "/Apps/Icons/$ICON_NAME"
if [[ -n ${HAD[$PACK_DST]} ]]; then
  cp "$tmp/old$PACK_DST" "$BACKUP_DIR/$(basename "$PACK_DST").with-lines.$stamp"
  echo "backup : $BACKUP_DIR/$(basename "$PACK_DST").with-lines.$stamp (${HAD[$PACK_DST]})"
fi

run() {
  local f
  ln_write_verify "$PACK" "$PACK_DST" || return 1                         # the pack first: apps vanish from the home screen
  for f in "/Apps/Executables/$EXE_NAME" "/Apps/Icons/$ICON_NAME"; do
    if ln_exists "$f"; then
      WROTE+=("$f"); mdel -i "$PART" "::$f" || return 1; echo "removed $f"
    else echo "absent  $f"; fi
  done
  for f in "/Apps/Executables/$EXE_NAME" "/Apps/Icons/$ICON_NAME"; do
    if ln_exists "$f"; then echo "still present: $f" >&2; return 1; fi
  done
}
if ! run; then ln_rollback; exit 1; fi
ln_flush
echo "REMOVE_OK: Lines removed. /Apps/Data/GBA (ROMs, saves, states) was kept. Restart the iPod."
