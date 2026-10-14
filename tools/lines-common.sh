#!/usr/bin/env bash
# Shared helpers for install-lines.sh, update-lines.sh and remove-lines.sh. Source it, do not run it.
#
# Target selection (set by the caller's option parser before calling ln_open_target):
#   DEVICE         block device of the iPod, e.g. /dev/disk/by-id/usb-Apple_iPod_<SERIAL>-0:0   (needs root)
#   IMAGE          FAT32 image file instead of a device (dry-run rehearsal, no root, no device)
#   USB_SERIAL     optional: refuse unless the device reports exactly this USB serial number
#   EXPECTED_SIZE  optional: refuse unless the whole disk has exactly this size in bytes
#   SLOT           A, B or auto (default auto: B when /Apps/Startup/resident-B.active exists, otherwise A)
#   BACKUP_DIR     where backups of overwritten files are kept (default ./lines-backup)
#
# After ln_open_target: PART (the data partition or the image), PACK_DST (/Apps/AllApps.pack or /Apps/AllApps-B.pack),
# tmp (scratch dir), and the functions ln_exists, ln_read_hash, ln_snap, ln_write_verify, ln_rollback below.
set -euo pipefail
umask 077

MARKER=/Apps/Startup/resident-B.active
EXE_NAME="Lines.hbapp"
ICON_NAME="Lines.bin"
BACKUP_DIR=${BACKUP_DIR:-./lines-backup}
LOCK_DIR=${LINES_LOCK_DIR:-/var/lock}
DEVICE= IMAGE= USB_SERIAL= EXPECTED_SIZE= SLOT=auto

die() { printf 'REFUSED: %s\n' "$*" >&2; exit 1; }
hash_of() { sha256sum -- "$1" | awk '{print $1}'; }

ln_need_tools() {
  local tool
  for tool in sha256sum awk mdir mcopy mdel readlink mktemp "$@"; do
    command -v "$tool" >/dev/null 2>&1 || die "missing tool: $tool"
  done
}

# Validates the target and sets PART. Device mode refuses unless: root, block device, model iPod, optional serial and size
# match, no partition of the disk is mounted, data partition exists, and the maintenance lock is free.
ln_open_target() {
  [[ -n $DEVICE || -n $IMAGE ]] || die "give --device DEV or --image FILE"
  [[ -z $DEVICE || -z $IMAGE ]] || die "use --device OR --image, not both"
  case $SLOT in auto|A|B) ;; *) die "--slot must be auto, A or B";; esac
  if [[ -n $DEVICE ]]; then
    local dev props model serial size node
    [[ $EUID == 0 ]] || die "root required for --device (use sudo)"
    ln_need_tools blockdev udevadm lsblk findmnt flock
    dev=$(readlink -f -- "$DEVICE")
    [[ -b $dev ]] || die "$DEVICE is not a block device"
    props=$(udevadm info --query=property --name "$dev")
    model=$(sed -n 's/^ID_MODEL=//p' <<<"$props" | head -n1)
    serial=$(sed -n 's/^ID_SERIAL_SHORT=//p' <<<"$props" | head -n1)
    size=$(blockdev --getsize64 "$dev")
    [[ $model == iPod ]] || die "$dev is not an iPod (ID_MODEL='$model')"
    [[ -z $USB_SERIAL || $serial == "$USB_SERIAL" ]] || die "USB serial mismatch: device reports '$serial', expected '$USB_SERIAL'"
    [[ -z $EXPECTED_SIZE || $size == "$EXPECTED_SIZE" ]] || die "disk size mismatch: device is $size bytes, expected $EXPECTED_SIZE"
    echo "device : $dev  model=$model  usb-serial=$serial  size=$size"
    PART="${dev}1"
    [[ -b $PART ]] || die "data partition $PART not found"
    while read -r node; do
      [[ -z $(findmnt -rn -S "$node" -o TARGET 2>/dev/null || true) ]] || die "$node is mounted: unmount it first (do not eject the iPod)"
    done < <(lsblk -nrpo NAME "$dev")
    mkdir -p "$LOCK_DIR"
    exec 9>"$LOCK_DIR/lines-maintenance.lock"
    flock -n 9 || die "another maintenance operation holds the lock"
  else
    [[ -f $IMAGE ]] || die "image file not found: $IMAGE"
    PART=$IMAGE
    export MTOOLS_SKIP_CHECK=1
  fi
  mkdir -p "$BACKUP_DIR"
  tmp=$(mktemp -d)
  trap 'rm -rf -- "$tmp"' EXIT
  ln_exists "/Apps" || die "no /Apps directory on the target: not a nano 7 with the homebrew firmware?"
  case $SLOT in
    B) ln_exists "$MARKER" || die "slot B requested but $MARKER is missing"; PACK_DST=/Apps/AllApps-B.pack;;
    A) PACK_DST=/Apps/AllApps.pack;;
    auto) if ln_exists "$MARKER"; then SLOT=B; PACK_DST=/Apps/AllApps-B.pack; else SLOT=A; PACK_DST=/Apps/AllApps.pack; fi;;
  esac
  echo "slot   : $SLOT  (app pack: $PACK_DST)"
}

ln_exists() { mdir -i "$PART" "::$1" >/dev/null 2>&1; }
ln_read_hash() { rm -f "$tmp/read"; mcopy -n -i "$PART" "::$1" "$tmp/read" 2>/dev/null || return 1; hash_of "$tmp/read"; }

# Pack directory sanity: AllApps.pack plus Executables/Lines.hbapp and Icons/Lines.bin.
ln_check_apps_dir() {
  local d=$1
  [[ -n $d && -d $d ]] || die "--apps-dir must be the directory that contains AllApps.pack, Executables/ and Icons/"
  [[ -f $d/AllApps.pack ]] || die "$d/AllApps.pack missing"
  [[ -f $d/Executables/$EXE_NAME ]] || die "$d/Executables/$EXE_NAME missing (was lines included when the pack was built?)"
  [[ -f $d/Icons/$ICON_NAME ]] || die "$d/Icons/$ICON_NAME missing"
}

declare -A HAD=()
WROTE=()
# ln_snap <path on iPod>: remember the current content (and whether it existed) for rollback.
ln_snap() {
  local dst=$1 f="$tmp/old$1"
  mkdir -p "$(dirname "$f")"
  if ln_exists "$dst"; then mcopy -n -i "$PART" "::$dst" "$f"; HAD[$dst]=$(hash_of "$f"); else HAD[$dst]=; fi
}
# ln_write_verify <local file> <path on iPod> [skip-if-equal]: write, read back, compare SHA-256.
ln_write_verify() {
  local src=$1 dst=$2 skip=${3:-} want
  want=$(hash_of "$src")
  if [[ -n $skip && ${HAD[$dst]:-} == "$want" ]]; then echo "same  $dst  $want (not rewritten)"; return 0; fi
  WROTE+=("$dst")
  mcopy -o -i "$PART" "$src" "::$dst" || return 1
  [[ $(ln_read_hash "$dst") == "$want" ]] || { echo "readback mismatch for $dst" >&2; return 1; }
  echo "ok    $dst  $want"
}
ln_rollback() {
  echo "ERROR: rolling back..." >&2
  local d
  for d in ${WROTE[@]+"${WROTE[@]}"}; do
    if [[ -n ${HAD[$d]:-} ]]; then mcopy -o -i "$PART" "$tmp/old$d" "::$d" || echo "  failed to restore $d" >&2
    else mdel -i "$PART" "::$d" 2>/dev/null || true; fi
  done
  echo "ROLLBACK_OK: previous files restored (copies of overwritten files are also in $BACKUP_DIR)." >&2
}
ln_flush() { sync; if [[ -n $DEVICE ]]; then blockdev --flushbufs "$PART" || true; fi; }
