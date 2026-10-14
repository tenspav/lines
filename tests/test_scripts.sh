#!/usr/bin/env bash
# Tests install-lines.sh, update-lines.sh, remove-lines.sh and copy-rom.sh against a FAT32 image (no iPod, no root).
# Needs: mtools (mcopy, mdir, mdel, mmd), dosfstools (mkfs.vfat), coreutils.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
T=$(dirname "$HERE")/tools
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
export MTOOLS_SKIP_CHECK=1
IMG=$W/ipod.img
BK=$W/backups
PASS=0; FAILN=0
ok()    { PASS=$((PASS+1)); echo "[PASS] $*"; }
bad()   { FAILN=$((FAILN+1)); echo "[FAIL] $*"; }
check() { if eval "$2"; then ok "$1"; else bad "$1   (condition: $2)"; fi; }

# --- fake apps directories (opaque bytes: the scripts never parse them)
mkapps() {   # mkapps DIR GENERATION  (generation changes the Lines executable and the pack)
  local d=$1 g=$2
  rm -rf "$d"; mkdir -p "$d/Executables" "$d/Icons"
  head -c 3000 /dev/zero | tr '\0' "a" > "$d/Executables/Other.hbapp"
  head -c 4000 /dev/zero | tr '\0' "b" > "$d/Executables/Game Two.hbapp"
  { echo "lines build $g"; head -c 5000 /dev/zero | tr '\0' "n"; } > "$d/Executables/Lines.hbapp"
  head -c 900 /dev/zero | tr '\0' "i" > "$d/Icons/Other.bin"
  head -c 900 /dev/zero | tr '\0' "j" > "$d/Icons/Game Two.bin"
  head -c 900 /dev/zero | tr '\0' "k" > "$d/Icons/Lines.bin"
  { echo "pack with lines $g"; head -c 2000 /dev/zero | tr '\0' "p"; } > "$d/AllApps.pack"
}
mkapps "$W/apps1" 1; mkapps "$W/apps2" 2
echo "pack WITHOUT lines" > "$W/pack_without.pack"
mkimg() {
  rm -f "$IMG"; truncate -s 64M "$IMG"; mkfs.vfat -F 32 -n IPOD "$IMG" >/dev/null
  mmd -i "$IMG" ::/Apps ::/Apps/Startup ::/Apps/Executables ::/Apps/Icons ::/Apps/Data ::/Apps/Data/GBA ::/Apps/Data/GBA/demo ::/iPod_Control
  echo marker > "$W/marker"; echo slotA > "$W/slotA"; echo oldB > "$W/oldB"; echo savedata > "$W/save.sav"
  mcopy -i "$IMG" "$W/marker" ::/Apps/Startup/resident-B.active
  mcopy -i "$IMG" "$W/slotA" ::/Apps/AllApps.pack
  mcopy -i "$IMG" "$W/oldB" ::/Apps/AllApps-B.pack
  mcopy -i "$IMG" "$W/save.sav" ::/Apps/Data/GBA/demo/save.sav
  mcopy -i "$IMG" "$W/apps1/Executables/Other.hbapp" ::/Apps/Executables/Other.hbapp
}
rd() { rm -f "$W/rd"; mcopy -n -i "$IMG" "::$1" "$W/rd" 2>/dev/null && sha256sum < "$W/rd" | cut -c1-64; }
hs() { sha256sum < "$1" | cut -c1-64; }
snapshot() { local d; d=$(mktemp -d); mcopy -s -n -i "$IMG" ::/ "$d" >/dev/null 2>&1; (cd "$d" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) | sha256sum | cut -c1-16; rm -rf "$d"; }
SHIM=$W/shim; mkdir -p "$SHIM"
mkshim() {   # mkshim PATTERN: the first overwrite (mcopy -o) of a destination matching PATTERN fails
  rm -f "$SHIM/once"
  cat > "$SHIM/mcopy" <<EOF
#!/bin/bash
w=0; for a in "\$@"; do [ "\$a" = -o ] && w=1; done
for a in "\$@"; do case "\$a" in *$1) [ \$w = 1 ] && [ ! -f $SHIM/once ] && { touch $SHIM/once; echo "injected failure" >&2; exit 1; };; esac; done
exec $(command -v mcopy) "\$@"
EOF
  chmod +x "$SHIM/mcopy"
}

echo "### install"
mkimg; BEFORE_OLD=$(rd /Apps/AllApps-B.pack)
"$T/install-lines.sh" --apps-dir "$W/apps1" --image "$IMG" --backup-dir "$BK" > "$W/i.out" 2>&1; rc=$?
tail -6 "$W/i.out"
check "install: rc 0 and INSTALL_OK" "[ $rc = 0 ] && grep -q INSTALL_OK $W/i.out"
check "install: slot B detected from the marker" "grep -q 'slot   : B' $W/i.out"
check "install: pack, 3 executables and 3 icons written and verified" "[ \"\$(rd /Apps/AllApps-B.pack)\" = \"\$(hs $W/apps1/AllApps.pack)\" ] && [ \"\$(rd '/Apps/Executables/Game Two.hbapp')\" = \"\$(hs '$W/apps1/Executables/Game Two.hbapp')\" ] && [ \"\$(rd /Apps/Executables/Lines.hbapp)\" = \"\$(hs $W/apps1/Executables/Lines.hbapp)\" ] && [ \"\$(rd /Apps/Icons/Lines.bin)\" = \"\$(hs $W/apps1/Icons/Lines.bin)\" ]"
check "install: slot A pack, marker and saved game untouched" "[ \"\$(rd /Apps/AllApps.pack)\" = \"\$(hs $W/slotA)\" ] && [ \"\$(rd /Apps/Startup/resident-B.active)\" = \"\$(hs $W/marker)\" ] && [ \"\$(rd /Apps/Data/GBA/demo/save.sav)\" = \"\$(hs $W/save.sav)\" ]"
check "install: previous pack saved in the backup dir with the same hash" "[ \"\$(hs \$(ls $BK/AllApps-B.pack.before-lines.* | head -1))\" = \"$BEFORE_OLD\" ]"

echo "### rollback: failure injected while writing the Lines icon"
mkimg; S0=$(snapshot); mkshim "Icons/Lines.bin"
PATH="$SHIM:$PATH" "$T/install-lines.sh" --apps-dir "$W/apps1" --image "$IMG" --backup-dir "$BK" > "$W/r.out" 2>&1; rc=$?
tail -3 "$W/r.out"
check "rollback: rc != 0, ROLLBACK_OK and the image is identical to the initial state" "[ $rc != 0 ] && grep -q ROLLBACK_OK $W/r.out && [ $S0 = \$(snapshot) ]"

echo "### update"
mkimg; "$T/install-lines.sh" --apps-dir "$W/apps1" --image "$IMG" --backup-dir "$BK" >/dev/null 2>&1
echo "state" > "$W/state.bin"; mcopy -o -i "$IMG" "$W/state.bin" ::/Apps/Data/GBA/demo/state.bin
snapo() { local d; d=$(mktemp -d); mcopy -s -n -i "$IMG" ::/ "$d" >/dev/null 2>&1; (cd "$d" && find . -type f ! -name Lines.hbapp ! -name Lines.bin ! -name AllApps-B.pack -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) | sha256sum | cut -c1-16; rm -rf "$d"; }
O0=$(snapo)
"$T/update-lines.sh" --apps-dir "$W/apps2" --image "$IMG" --backup-dir "$BK" > "$W/u.out" 2>&1; rc=$?
tail -5 "$W/u.out"
check "update: rc 0, UPDATE_OK, new executable in place (read back)" "[ $rc = 0 ] && grep -q UPDATE_OK $W/u.out && [ \"\$(rd /Apps/Executables/Lines.hbapp)\" = \"\$(hs $W/apps2/Executables/Lines.hbapp)\" ]"
check "update: other executables, state.bin and slot A untouched" "[ $O0 = \$(snapo) ]"
check "update: previous executable saved in the backup dir" "[ \"\$(hs \$(ls -t $BK/Lines.hbapp.before-update.* | head -1))\" = \"\$(hs $W/apps1/Executables/Lines.hbapp)\" ]"
"$T/update-lines.sh" --apps-dir "$W/apps2" --image "$IMG" --backup-dir "$BK" > "$W/u2.out" 2>&1
check "update: idempotent (second run rewrites nothing)" "grep -q 'same  /Apps/Executables/Lines.hbapp' $W/u2.out && ! grep -q '^ok ' $W/u2.out"
mcopy -o -i "$IMG" "$W/apps1/Executables/Lines.hbapp" ::/Apps/Executables/Lines.hbapp; S0=$(snapshot); mkshim "Executables/Lines.hbapp"
PATH="$SHIM:$PATH" "$T/update-lines.sh" --apps-dir "$W/apps2" --image "$IMG" --backup-dir "$BK" > "$W/u3.out" 2>&1; rc=$?
check "update with an injected failure: rc != 0, ROLLBACK_OK, image unchanged" "[ $rc != 0 ] && grep -q ROLLBACK_OK $W/u3.out && [ $S0 = \$(snapshot) ]"
mkimg; "$T/update-lines.sh" --apps-dir "$W/apps2" --image "$IMG" --backup-dir "$BK" > "$W/u4.out" 2>&1
check "update without Lines installed is refused (use install)" "[ \$? != 0 ] && grep -q 'not installed' $W/u4.out"

echo "### remove"
mkimg; "$T/install-lines.sh" --apps-dir "$W/apps1" --image "$IMG" --backup-dir "$BK" >/dev/null 2>&1
"$T/remove-lines.sh" --pack "$W/pack_without.pack" --pack-sha256 "$(hs $W/pack_without.pack)" --image "$IMG" --backup-dir "$BK" > "$W/m.out" 2>&1; rc=$?
tail -4 "$W/m.out"
check "remove: rc 0 and REMOVE_OK" "[ $rc = 0 ] && grep -q REMOVE_OK $W/m.out"
check "remove: pack replaced; Lines executable and icon gone; other apps and the saved game stay" "[ \"\$(rd /Apps/AllApps-B.pack)\" = \"\$(hs $W/pack_without.pack)\" ] && ! mdir -i $IMG ::/Apps/Executables/Lines.hbapp >/dev/null 2>&1 && ! mdir -i $IMG ::/Apps/Icons/Lines.bin >/dev/null 2>&1 && [ -n \"\$(rd '/Apps/Executables/Game Two.hbapp')\" ] && [ -n \"\$(rd /Apps/Data/GBA/demo/save.sav)\" ]"
"$T/remove-lines.sh" --pack "$W/pack_without.pack" --pack-sha256 deadbeef --image "$IMG" --backup-dir "$BK" > "$W/m2.out" 2>&1
check "remove: wrong --pack-sha256 is refused" "[ \$? != 0 ] && grep -q 'does not match' $W/m2.out"

echo "### refusals"
"$T/install-lines.sh" --apps-dir "$W/apps1" --image "$W/missing.img" > "$W/x1.out" 2>&1
check "image that does not exist" "[ \$? != 0 ] && grep -q REFUSED $W/x1.out"
"$T/install-lines.sh" --apps-dir "$W/apps1" --device /dev/null > "$W/x2.out" 2>&1
check "--device without root" "[ \$? != 0 ] && grep -q 'root required' $W/x2.out"
mkimg; mdel -i "$IMG" ::/Apps/Startup/resident-B.active
"$T/install-lines.sh" --apps-dir "$W/apps1" --image "$IMG" --slot B --backup-dir "$BK" > "$W/x3.out" 2>&1
check "--slot B without the marker" "[ \$? != 0 ] && grep -q 'slot B requested' $W/x3.out"
"$T/install-lines.sh" --apps-dir "$W/apps1" --image "$IMG" --backup-dir "$BK" > "$W/x4.out" 2>&1
check "without the marker, auto selects slot A (/Apps/AllApps.pack)" "grep -q 'slot   : A' $W/x4.out && [ \"\$(rd /Apps/AllApps.pack)\" = \"\$(hs $W/apps1/AllApps.pack)\" ]"
mkapps "$W/apps3" 3; rm "$W/apps3/Icons/Lines.bin"
"$T/install-lines.sh" --apps-dir "$W/apps3" --image "$IMG" --backup-dir "$BK" > "$W/x5.out" 2>&1
check "apps dir without the Lines icon is refused" "[ \$? != 0 ] && grep -q 'Lines.bin missing' $W/x5.out"
echo "### copy-rom"
RD=$W/rom_demo; mkdir -p "$RD"
python3 - "$RD" <<'PY'
import struct, sys
d = sys.argv[1]
open(d + "/meta.bin", "wb").write(struct.pack("<IB3x", 40000, 2))      # 40000 bytes -> 2 pages, Flash64
for i in range(2):
    open(d + "/p%03d.bin" % i, "wb").write(bytes([(i * 37 + k) & 0xFF for k in range(32768)]))
PY
mkimg
mmd -i "$IMG" ::/Apps/Data/GBA ::/Apps/Data/GBA/my_game
echo "keep me" > "$W/mysave"; mcopy -i "$IMG" "$W/mysave" ::/Apps/Data/GBA/my_game/save.sav
"$T/copy-rom.sh" "$RD" --name my_game --image "$IMG" > "$W/c1.out" 2>&1; rc=$?
tail -2 "$W/c1.out"
check "copy-rom: rc 0, ROM_OK, 3 files written" "[ $rc = 0 ] && grep -q 'ROM_OK' $W/c1.out && grep -q 'written=3' $W/c1.out"
check "copy-rom: meta.bin and pages read back identical" "[ \"\$(rd /Apps/Data/GBA/my_game/meta.bin)\" = \"\$(hs $RD/meta.bin)\" ] && [ \"\$(rd /Apps/Data/GBA/my_game/p001.bin)\" = \"\$(hs $RD/p001.bin)\" ]"
check "copy-rom: existing save.sav untouched" "[ \"\$(rd /Apps/Data/GBA/my_game/save.sav)\" = \"\$(hs $W/mysave)\" ]"
"$T/copy-rom.sh" "$RD" --name my_game --image "$IMG" > "$W/c2.out" 2>&1
check "copy-rom: second run skips identical files" "grep -q 'written=0 skipped(identical)=3' $W/c2.out"
head -c 32768 /dev/zero | tr '\0' z > "$W/p0_other.bin"; cp -r "$RD" "$W/rom_demo2"; cp "$W/p0_other.bin" "$W/rom_demo2/p000.bin"
"$T/copy-rom.sh" "$W/rom_demo2" --name my_game --image "$IMG" > "$W/c3.out" 2>&1
check "copy-rom: changed page refused without --force" "[ \$? != 0 ] && grep -q 'different content' $W/c3.out"
"$T/copy-rom.sh" "$RD" --name "Bad Name" --image "$IMG" > "$W/c4.out" 2>&1
check "copy-rom: invalid name refused" "[ \$? != 0 ] && grep -q REFUSED $W/c4.out"
rm "$W/rom_demo2/p001.bin"
"$T/copy-rom.sh" "$W/rom_demo2" --name other_game --image "$IMG" > "$W/c5.out" 2>&1
check "copy-rom: missing page refused" "[ \$? != 0 ] && grep -q 'missing or wrong-size page' $W/c5.out"
echo "SUMMARY: $PASS PASS, $FAILN FAIL"
exit $FAILN
