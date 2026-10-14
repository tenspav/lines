# Third-party code, data and provenance

Lines' own code is licensed under the MIT License (`LICENSE`). This file lists what is included, what is only
required to build, and the result of the provenance review of the emulator code.

## Included in this repository

Nothing from other projects is copied into this repository: no emulator source, no SDK files, no BIOS image, no ROM
data, no fonts (the on-screen font is a 3x5 bitmap table written for this project in `src/lines.c`).

## Not included, required to build

| Component | Where | License | Notes |
|---|---|---|---|
| NanoApps SDK (`sdk/`, `tools/`, LVGL fork) | [nfzerox/NanoApps](https://github.com/nfzerox/NanoApps); the fork inside [olievans123/nano7-untethered](https://github.com/olievans123/nano7-untethered) under `nanoapps/` | MIT, (c) 2026 nfzerox | **Not copied here.** `tools/build.sh` copies `src/` into `<sdk>/apps/lines` and runs the SDK's own `make`; `src/Makefile` includes `../../sdk/hb_app.mk` from that tree. |
| GNU Arm Embedded toolchain | your system | GPL/LGPL, used as a tool | Not distributed. |
| Untethered firmware and loader | olievans123/nano7-untethered | see that repository | Not included. Lines is only an app that the loader starts. |

To fit the app into an SDK tree by hand: copy `src/` to `<sdk>/apps/lines/` so that `<sdk>/apps/lines/Makefile`
sits next to `<sdk>/sdk/hb_app.mk`, then run `make` there (this is exactly what `tools/build.sh` does).

## Provenance review of the emulator code

The core (`src/cpu.c`, `src/mem.c`, `src/ppu.c`, `src/bios.c`, `src/gba.h`) and the front end (`src/lines.c`) were
written for this project. A review for derivation from other emulators (mGBA, gpSP, VisualBoyAdvance, NanoBoyAdvance,
TinyGBA, Peanut-GB and similar) looked for: copyright or license headers, author names, project names, characteristic
identifiers and table layouts, copied comments, and large literal tables. Result: no license header, author name or
project name of another emulator appears in the sources, and no large table was found. The only data tables are
the small ones listed below.

Items that deserve a note, all of them facts about the hardware or about the public GBA documentation rather than
code taken from another program:

| Item | Where | Note |
|---|---|---|
| 6-instruction IRQ trampoline (push registers, load the user handler address from `0x03FFFFFC`, call it, pop, return from the exception) | `src/mem.c` (`IRQ_HANDLER`) | The documented behaviour of the console's BIOS interrupt entry, written as ARM machine code words. It is a functional necessity and common to every GBA emulator that does not ship the BIOS image. |
| ArcTan polynomial coefficients (8 constants) | `src/bios.c` (`arctan_poly`) | The coefficients of the BIOS `ArcTan` approximation are published in public GBA hardware documentation (GBATEK). They are numeric facts about the hardware's behaviour. |
| `GetBiosChecksum` return value `0xBAAE187F` | `src/bios.c` | A single published 32-bit constant identifying the original BIOS revision; the BIOS image itself is not present. |
| Instruction word signatures for the sound mixer routine of a widely used commercial sound driver (6 + 4 words) and a few code-address constants of the optional title-specific speedup | `src/cpu.c` | Short instruction sequences used only to recognise code already running in the user's own ROM, so that the mixing loop can be skipped (the app has no audio output). No ROM data beyond these few words is stored anywhere, and the speedup activates only when the loaded ROM's header game code and the expected code at the entry points match. |
| The software BIOS | `src/bios.c` | An original high-level implementation: each SWI is implemented from its documented behaviour (Div, Sqrt, ArcTan, CpuSet, CpuFastSet, BgAffineSet, ObjAffineSet, LZ77, run-length, Huffman, IntrWait and others). `g->bios[]` is zero-filled and receives only the few vector words above; no bytes of the official BIOS are embedded, and the BIOS image is not required. |

## Not distributed

- No ROM images, no save states, no BIOS image and no pre-built `.hbapp` are in this repository. Games are not
  included; use your own dumps of cartridges you own.
- The test suite generates synthetic ROMs and programs (`tests/mkrom.py`, `tests/core_test.c`); it contains no data
  from commercial ROMs.

## Conclusion

No code derived from another emulator was found. The project is released under the MIT License, the same license as
the NanoApps SDK it is built against. If you know of an overlap that this review missed, please open an issue.
