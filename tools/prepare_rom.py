#!/usr/bin/env python3
"""prepare_rom.py - convert a .gba ROM into the folder layout the Lines app (iPod nano 7) reads.

    python3 prepare_rom.py ROM.gba [--name my_game] [--out DIR] [--force]

Output  DIR/<name>/meta.bin   8 bytes: u32 LE ROM size, u8 save type (0 none, 1 SRAM, 2 Flash64K,
                                       3 Flash128K, 4 EEPROM), 3 zero bytes
        DIR/<name>/p000.bin.. the ROM cut in 32 KiB pages (last one zero padded); p1000.. for pages >= 1000
Default DIR = ./GBA.
On the iPod this folder becomes /Apps/Data/GBA/<name>/ (tools/copy-rom.sh does the copy); the app creates
save.sav and state.bin next to the pages, so every ROM has its own saves.

The save type is detected exactly like gba_detect_save() in src/mem.c (first 4-byte aligned
FLASH/SRAM/EEPROM library tag in the ROM). Limits: ROM <= 32 MiB (GBA address space); folder name
<= 31 chars of [a-z0-9_-] (it is the name shown in the menu). EEPROM games load but cannot save yet.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

PAGE = 1 << 15
MAX_ROM = 32 * 1024 * 1024


def detect_save(rom):
    n = len(rom)
    best = None
    for tag in (b"FLASH", b"SRAM", b"EEPROM"):
        i = rom.find(tag)
        while i != -1:
            if i % 4 == 0 and i + 8 < n:
                if best is None or i < best[0]:
                    best = (i, tag)
                break
            i = rom.find(tag, i + 1)
    if best is None:
        return 0
    i, tag = best
    if tag == b"FLASH":
        return 3 if rom[i + 5:i + 7] == b"1M" else 2
    return 1 if tag == b"SRAM" else 4


def slug(path):
    base = os.path.splitext(os.path.basename(path))[0]
    base = re.sub(r"[\(\[].*?[\)\]]", " ", base)          # drop "(USA)" "[!]" tags
    s = re.sub(r"[^a-z0-9]+", "_", base.lower()).strip("_")
    return s[:31].rstrip("_") or "rom"


def page_name(i):
    return "p%03d.bin" % i


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--name", help="folder / menu name (default: from the file name)")
    ap.add_argument("--out", help="output root (default ./GBA)")
    ap.add_argument("--force", action="store_true", help="overwrite an existing folder")
    a = ap.parse_args()

    with open(a.rom, "rb") as f:
        rom = f.read()
    if len(rom) < 0xC0:
        sys.exit("ERROR: file too small to be a GBA ROM (%d bytes)" % len(rom))
    if len(rom) > MAX_ROM:
        sys.exit("ERROR: ROM of %d bytes > 32 MiB (the GBA address space limit)" % len(rom))
    if rom[0xB2] != 0x96:
        print("WARNING: header byte 0xB2 != 0x96: this may not be a valid GBA ROM")
    name = a.name or slug(a.rom)
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,30}", name):
        sys.exit("ERROR: invalid name '%s' (use a-z 0-9 _ -, up to 31 characters)" % name)

    root = a.out or "GBA"
    dst = os.path.join(root, name)
    if os.path.exists(dst) and not a.force:
        sys.exit("ERROR: %s already exists (use --force to overwrite)" % dst)
    os.makedirs(dst, exist_ok=True)

    st = detect_save(rom)
    npages = (len(rom) + PAGE - 1) // PAGE
    for i in range(npages):
        chunk = rom[i * PAGE:(i + 1) * PAGE]
        with open(os.path.join(dst, page_name(i)), "wb") as f:
            f.write(chunk + b"\0" * (PAGE - len(chunk)))
    with open(os.path.join(dst, "meta.bin"), "wb") as f:
        f.write(struct.pack("<IB3x", len(rom), st))

    # verify: reassemble from disk
    h = hashlib.sha1()
    left = len(rom)
    for i in range(npages):
        with open(os.path.join(dst, page_name(i)), "rb") as f:
            d = f.read()
        assert len(d) == PAGE
        h.update(d[:min(PAGE, left)])
        left -= PAGE
    ok = h.hexdigest() == hashlib.sha1(rom).hexdigest()
    title = rom[0xA0:0xAC].rstrip(b"\0").decode("ascii", "replace")
    code = rom[0xAC:0xB0].decode("ascii", "replace")
    names = {0: "none", 1: "SRAM", 2: "Flash 64K", 3: "Flash 128K", 4: "EEPROM (not saved by the app yet)"}
    print("ROM     : %s  (%s / %s)" % (os.path.basename(a.rom), title, code))
    print("folder  : %s" % dst)
    print("size    : %d bytes, %d pages of 32 KiB, %.1f MB on the iPod" % (len(rom), npages, npages * PAGE / 1048576))
    print("save    : %s" % names[st])
    print("verify  : %s" % ("OK (sha1 of the pages == ROM)" if ok else "FAILED"))
    print("next    : copy it to the iPod with  tools/copy-rom.sh %s --device DEV" % dst)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
