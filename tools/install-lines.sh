#!/usr/bin/env bash
# Installs Lines on an iPod nano 7th gen that runs the untethered homebrew firmware, with mtools, while the iPod is
# attached to Linux in disk mode and its data partition is NOT mounted.
#
# What it does: copies a prepared "apps directory" to the iPod data partition:
#     <apps-dir>/AllApps.pack        -> /Apps/AllApps-B.pack  (slot B)  or /Apps/AllApps.pack (slot A)
#     <apps-dir>/Executables/*.hbapp -> /Apps/Executables/
#     <apps-dir>/Icons/*.bin         -> /Apps/Icons/
# Every file is read back and compared by SHA-256; any failure restores the previous files (or deletes new ones).
# Overwritten files are also copied to the backup directory first. The pack is written last.
#
# What it does NOT do: it does not build the pack. The pack lists ALL apps of the slot, so it must be produced beforehand with the
# SDK's tools/build_apps.py including every app you want to keep (see the README, section "Install"). It never touches the firmware,
# the resident loaders, the marker file, /Apps/Data or /iPod_Control. After installing, restart the iPod and re-run the
# resident installer of the firmware project so the new app appears.
#
#   sudo ./install-lines.sh --apps-dir DIR --device /dev/disk/by-id/usb-Apple_iPod_<SERIAL>-0:0 [--usb-serial ID] [--expected-size BYTES]
#   ./install-lines.sh --apps-dir DIR --image rehearsal.img          (FAT32 image, no root, no device)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lines-common.sh
source "$HERE/lines-common.sh"

APPS_DIR=
usage() {
  cat >&2 <<EOF
usage: sudo $0 --apps-dir DIR --device DEV [--usb-serial ID] [--expected-size BYTES] [--slot auto|A|B] [--backup-dir DIR]
       $0 --apps-dir DIR --image IMG [--slot auto|A|B] [--backup-dir DIR]
EOF
  exit 2
}
while [[ $# -gt 0 ]]; do
  case $1 in
    --apps-dir) APPS_DIR=${2:-}; shift 2;;
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
[[ -n $APPS_DIR ]] || usage
ln_need_tools find sort date
ln_check_apps_dir "$APPS_DIR"
ln_open_target

# The slot's pack and the executables/icons we are about to overwrite are snapshotted for rollback.
stamp=$(date -u +%Y%m%dT%H%M%SZ)
mapfile -t EXES < <(find "$APPS_DIR/Executables" -maxdepth 1 -type f -name '*.hbapp' -printf '%f\n' | LC_ALL=C sort)
mapfile -t ICONS < <(find "$APPS_DIR/Icons" -maxdepth 1 -type f -name '*.bin' -printf '%f\n' | LC_ALL=C sort)
echo "apps   : ${#EXES[@]} executable(s), ${#ICONS[@]} icon(s), pack $(hash_of "$APPS_DIR/AllApps.pack")"

ln_snap "$PACK_DST"
if [[ -n ${HAD[$PACK_DST]} ]]; then
  cp "$tmp/old$PACK_DST" "$BACKUP_DIR/$(basename "$PACK_DST").before-lines.$stamp"
  echo "backup : $BACKUP_DIR/$(basename "$PACK_DST").before-lines.$stamp (${HAD[$PACK_DST]})"
fi
for f in "${EXES[@]}"; do ln_snap "/Apps/Executables/$f"; done
for f in "${ICONS[@]}"; do ln_snap "/Apps/Icons/$f"; done
for f in "${EXES[@]}"; do
  if [[ -n ${HAD["/Apps/Executables/$f"]} ]]; then cp "$tmp/old/Apps/Executables/$f" "$BACKUP_DIR/$f.before-lines.$stamp"; fi
done

run() {
  local f
  echo "== executables =="
  for f in "${EXES[@]}"; do ln_write_verify "$APPS_DIR/Executables/$f" "/Apps/Executables/$f" || return 1; done
  echo "== icons =="
  for f in "${ICONS[@]}"; do ln_write_verify "$APPS_DIR/Icons/$f" "/Apps/Icons/$f" || return 1; done
  echo "== pack (last) =="
  ln_write_verify "$APPS_DIR/AllApps.pack" "$PACK_DST" || return 1
}
if ! run; then ln_rollback; exit 1; fi
ln_flush
echo "INSTALL_OK: ${#EXES[@]} app(s) installed in slot $SLOT."
echo "Restart the iPod and re-run the firmware project's resident installer so the new app shows up. Do not eject before unmounting."
echo "To undo: sudo $HERE/remove-lines.sh --pack PACK_WITHOUT_LINES ... (the previous pack is in $BACKUP_DIR)"
