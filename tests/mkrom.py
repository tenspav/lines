#!/usr/bin/env python3
"""mkrom.py - write a small SYNTHETIC GBA ROM image for the tests (no commercial data).

    mkrom.py OUT.gba SIZE_BYTES [--save none|sram|flash64|flash128]

The image has a branch-to-self at the entry point, a header whose byte 0xB2 is 0x96, a title of "SYNTHTEST",
a deterministic pseudo-random body, and (optionally) the library tag string that save-type detection looks for.
"""
import argparse
import struct

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("size", type=int)
ap.add_argument("--save", default="none", choices=["none", "sram", "flash64", "flash128"])
a = ap.parse_args()
if a.size < 0x1000:
    raise SystemExit("size must be at least 4096")
rom = bytearray(a.size)
x = 12345
for i in range(0xC0, a.size):            # xorshift filler
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    rom[i] = x & 0xFF
rom[0:4] = struct.pack("<I", 0xEAFFFFFE)  # b .
rom[0xA0:0xAC] = b"SYNTHTEST\0\0\0"
rom[0xAC:0xB0] = b"SYTH"
rom[0xB2] = 0x96
tag = {"none": b"", "sram": b"SRAM_V113\0\0\0", "flash64": b"FLASH_V126\0\0",
       "flash128": b"FLASH1M_V103\0"}[a.save]
if tag:
    rom[0x400:0x400 + len(tag)] = tag
open(a.out, "wb").write(rom)
