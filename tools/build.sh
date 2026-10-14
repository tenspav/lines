#!/usr/bin/env bash
# Builds Lines for the iPod nano 7 inside a NanoApps SDK checkout.
#
# The SDK is not part of this repository (see THIRD_PARTY.md). This script copies src/ into <sdk>/apps/lines
# (replacing a previous copy), runs the SDK's make there, and copies the result to --out.
#
#   tools/build.sh --sdk /path/to/NanoApps [--out DIR]
#
# Result: DIR/lines.hbapp (and lines.reloc.elf), then checked with tools/check_arm.sh.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$HERE")
SDK= OUT=$PWD/build

usage() { echo "usage: $0 --sdk NANOAPPS_DIR [--out DIR]" >&2; exit 2; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
while [[ $# -gt 0 ]]; do
  case $1 in
    --sdk) SDK=${2:-}; shift 2;;
    --out) OUT=${2:-}; shift 2;;
    *) usage;;
  esac
done
[[ -n $SDK ]] || usage
[[ -f $SDK/sdk/hb_app.mk ]] || die "$SDK does not look like a NanoApps checkout (sdk/hb_app.mk missing)"
command -v arm-none-eabi-gcc >/dev/null || die "arm-none-eabi-gcc not found"

APP=$SDK/apps/lines
rm -rf "$APP"
mkdir -p "$APP"
cp -a "$REPO"/src/. "$APP"/
make -C "$APP" 2>&1 | tee "$APP/build.log"

mkdir -p "$OUT"
cp "$APP/build/lines.hbapp" "$OUT/"
[[ -f $APP/build/lines.reloc.elf ]] && cp "$APP/build/lines.reloc.elf" "$OUT/"
echo "built: $OUT/lines.hbapp ($(stat -c %s "$OUT/lines.hbapp") bytes)"
if [[ -f $OUT/lines.reloc.elf ]]; then "$HERE/check_arm.sh" "$OUT/lines.reloc.elf"; fi
