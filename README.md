<h1 align="center">Lines</h1>

<p align="center">
  A Game Boy Advance emulator for the iPod nano 7th generation, running straight on the device.
</p>

<p align="center">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-green">
  <img alt="Device: iPod nano 7th gen" src="https://img.shields.io/badge/device-iPod%20nano%207G-lightgrey">
  <img alt="Language: C" src="https://img.shields.io/badge/language-C-orange">
</p>

Lines is a homebrew app that plays GBA ROMs you provide on an iPod nano 7th generation. It has an ARM7TDMI interpreter
(ARM and Thumb), a scanline renderer, a software BIOS, battery saves (SRAM and Flash), save states, a ROM menu and
multitouch on-screen controls. The emulator core is original work written for this project.

> **Requires a modified iPod.** Lines only runs on an iPod nano 7th generation that already runs the untethered
> homebrew firmware. A stock iPod cannot run it. See [Before you start](#before-you-start).

## Features

- **GBA core in C.** ARM and Thumb interpreter, tiled, affine and bitmap backgrounds, sprites, windows, blending,
  timers, DMA, interrupts, and a high-level software BIOS. No BIOS file is needed.
- **ROM menu.** Every folder in `/Apps/Data/GBA` that holds a prepared ROM shows up in a list. Tap to play.
- **Saves and states per ROM.** Each game keeps its own `save.sav` (SRAM or Flash) and `state.bin` next to its pages.
  The state is written automatically every minute and when you release memory.
- **Fits the device.** ROM data is read from disk in 32 KiB pages through a cache that is sized at run time from the
  free heap and replaced by least-recently-used. ROMs up to 32 MiB work.
- **Touch controls.** D-pad, A, B, L, R, Select and Start with multitouch, in portrait or landscape (`ROT`).
- **Releases its memory.** Holding `EXIT` (or ten minutes without touching the screen) saves, frees the whole emulator
  block and shows a screen that says it is safe to leave.
- **Adaptive speed.** Frame pacing targets the console's 59.7 Hz and skips drawing frames when the device falls behind.

## Before you start

Lines is a third-party app. It needs the iPod to be modified first with the **untethered homebrew firmware**, which
adds the loader that starts apps like this one. That setup is a separate project and is not part of Lines:

1. Go to **[olievans123/nano7-untethered](https://github.com/olievans123/nano7-untethered)** and follow the
   instructions in its README to modify the iPod. Read all of it first, including its warnings.
2. Once the iPod boots with that firmware and can start apps, come back here to build and install Lines.

Modifying an iPod can render it unusable and voids any warranty. Back up the iPod before you begin, and do not use an
iPod you cannot afford to lose.

## Quick start

1. [Modify the iPod](#before-you-start) with the untethered firmware.
2. [Build](#build) Lines and [install](#install) it (needs a Linux PC).
3. [Convert your ROM](#adding-roms) with `tools/prepare_rom.py` and copy it with `tools/copy-rom.sh`.
4. Open **Lines** on the iPod and tap the game.

## Requirements

| What | Details |
|---|---|
| iPod | nano 7th generation with the untethered homebrew firmware from [olievans123/nano7-untethered](https://github.com/olievans123/nano7-untethered) |
| Linux PC | to build and install: `arm-none-eabi-gcc`, `make`, `python3`, `mtools` (`mcopy`, `mdir`, `mdel`, `mmd`), `dosfstools` for the tests, optionally `udevadm` |
| SDK | the NanoApps SDK used by that firmware project (`nanoapps/` in nano7-untethered, or [nfzerox/NanoApps](https://github.com/nfzerox/NanoApps)). It is not copied here; run its `./start build` once so it fetches its dependencies |
| ROMs | your own dumps of cartridges you own. None are included |

> **Use at your own risk.** The installer rewrites the iPod's app pack. It keeps a backup of every file it overwrites
> and rolls back on a failed write, but make your own backup of the iPod first. See [Risks](#risks).

## Build

```sh
tools/build.sh --sdk /path/to/NanoApps --out build
# -> build/lines.hbapp
```

`tools/build.sh` copies `src/` into `<sdk>/apps/lines`, runs the SDK's `make` there, copies the result to `--out` and
checks the ELF with `tools/check_arm.sh` (core symbols present, no VFP/NEON instructions: the app uses soft-float
code). The SDK itself is never modified outside `apps/lines`.

To fit the app into an SDK tree by hand, copy `src/` to `<sdk>/apps/lines/` and run `make` in that directory.

## Install

The SDK packs every app of a slot into one `AllApps.pack`, so the pack must be built with **all** the apps you want to
keep, not only Lines. The installer does not merge into an existing pack; it writes the pack you give it.

1. Build the apps you use in the SDK (Lines is built above), then build the pack:

   ```sh
   cd /path/to/NanoApps
   python3 tools/build_apps.py blocks gameboy probe lines   # list every app you want, plus lines
   ```

   The result is `/tmp/hbapps/Apps`, containing `AllApps.pack`, `Executables/` and `Icons/`.

2. Attach the iPod in disk mode and **unmount its data partition** (do not eject the iPod).

3. Optional rehearsal on a FAT32 image, no iPod needed:

   ```sh
   tools/install-lines.sh --apps-dir /tmp/hbapps/Apps --image test.img
   ```

4. Install:

   ```sh
   sudo tools/install-lines.sh --apps-dir /tmp/hbapps/Apps \
        --device /dev/disk/by-id/usb-Apple_iPod_<SERIAL>-0:0
   ```

   Optional flags: `--usb-serial <SERIAL>`, `--expected-size <bytes>`, `--slot auto|A|B`, `--backup-dir DIR`.

5. Restart the iPod.

**What the installer checks:** it must run as root, the device must be a block device with model `iPod` (and match
`--usb-serial` and `--expected-size` if you pass them), no partition of the disk may be mounted, and the maintenance
lock must be free. It detects the active slot, saves the files it will overwrite, writes the executable, icon and pack,
reads each file back and compares SHA-256, and restores the previous files on any failure. It never touches the
firmware, the loaders, `/Apps/Data` or `/iPod_Control`.

**What it does not do:** it does not build or merge packs. Slot A has not been tested on a device.

**Update** (replaces only `Lines.hbapp`, plus the icon and pack if they differ):

```sh
sudo tools/update-lines.sh --apps-dir DIR --device DEV
```

**Remove** (needs a pack built without Lines, or the `before-lines` backup the installer made):

```sh
sudo tools/remove-lines.sh --pack PACK --device DEV
```

Your ROMs and saves in `/Apps/Data/GBA` stay on the iPod.

## Adding ROMs

Lines plays ROMs that were cut into 32 KiB pages so the app can load them piece by piece. `tools/prepare_rom.py` does
that conversion on your PC:

```sh
python3 tools/prepare_rom.py MyGame.gba --name my_game --out GBA
# -> GBA/my_game/meta.bin, GBA/my_game/p000.bin, p001.bin, ...
```

The name uses `a-z 0-9 _ -` (up to 31 characters) and is what the menu shows. The script detects the save type
(none, SRAM, Flash 64K, Flash 128K, EEPROM) and verifies the pages against the ROM.

Copy the folder to the iPod (data partition unmounted, iPod in disk mode):

```sh
sudo tools/copy-rom.sh GBA/my_game --device /dev/disk/by-id/usb-Apple_iPod_<SERIAL>-0:0
# or rehearse on an image:  tools/copy-rom.sh GBA/my_game --image test.img
```

`copy-rom.sh` accepts the same `--usb-serial`, `--expected-size` and `--backup-dir` options as the installer, reads
every file back, and never touches `save.sav`, `state.bin` or `last.txt`. A page that exists with different content is
refused unless you pass `--force`. You can also copy the folder with any other tool that can write to the iPod's data
partition; it only has to end up at `/Apps/Data/GBA/<name>/`. Do not use the SDK's `--demo-data-force` option, which
wipes `/Apps/Data/GBA` including your saves. The folder layout is described in [`docs/rom-layout.md`](docs/rom-layout.md).

## Using the app

- **Opening Lines:** with two or more ROMs you get the menu (the last game is highlighted); with one ROM it starts
  straight away; reopening it without closing it continues the game.
- **Menu:** tap a row to play, drag to scroll, `ROT` switches portrait and landscape. A row shows the name, the ROM
  size, `SAVE` or `NEW`, and `LAST` for the game you played last.
- **In game (portrait):** the status bar on top has `ROMS`, `SAVE` and `ROT`; the panel below the picture has the
  D-pad, A, B, L, R, `SEL`, `START` and `EXIT`. Landscape shows the picture at 1.5x with the controls over it and
  `ROMS`, `SV` (save), `EXIT` in the margins.
- **`SAVE` / `SV`:** hold about half a second to write a save state now (the app also does it every minute).
- **`ROMS`:** hold about 0.6 seconds to save, free the memory and go back to the menu.
- **`EXIT`:** hold about 0.6 seconds to save a state and free all memory. The screen then reads *MEMORY RELEASED* and
  *YOU MAY LEAVE*; press Home. A tap, or opening Lines again, goes back to the last game from its state.
- **Idle:** ten minutes without touching the screen does the same as `EXIT`.

Leaving with Home without using `EXIT` also works: the system frees the app's block at the next launch and Lines
resumes the last game from its latest state.

### Saves

Each ROM folder holds its own `save.sav` (the cartridge's SRAM/Flash, flushed a couple of seconds after the game
changes it) and `state.bin` (a full save state). They never collide between games.

## Compatibility

This is a from-scratch interpreter running on a small ARM device, so be realistic.

- **Supported:** ARM and Thumb code, tiled, affine and bitmap background modes, sprites (including affine), windows
  and alpha blending, four timers, four DMA channels, interrupts, the usual SWI calls, SRAM, Flash 64K and Flash 128K
  saves, ROMs up to 32 MiB.
- **Not supported or limited:** no sound output (the audio hardware is not emulated and the app has no audio path);
  EEPROM games load but their saves are not written yet; no link cable, solar sensor, RTC or other cartridge
  peripherals; ROMs larger than 32 MiB are listed but refused. The software BIOS covers the common calls, so a game that
  depends on unusual BIOS behaviour may fail.
- **Speed:** the app targets full speed and drops drawn frames when it cannot keep up (the `SK` value in landscape
  shows the skip level, `FPS` the achieved rate). How fast a game runs depends on the game; heavy titles may run below
  full speed. No benchmark figures are published here because they have not been collected in a repeatable way.
- **Title-specific speedup:** for one known title the core can optionally replace a few hot routines with native code;
  it only activates when the ROM's header game code and the expected code match, and everything else is unaffected.
- **Not tested on:** slot A of the app pack. The author tested on one device only.

Game compatibility is not tracked per title. If something does not work, it is expected rather than surprising.

## How it works

```
 ROM folder (32 KiB pages)            iPod nano 7 (raw surface app)
 /Apps/Data/GBA/<name>/p000.bin ...        |
            |                              v
            |  page fault          +-------------------+
            +--------------------> | ROM page cache    |  LRU, sized from the free heap
                                   +---------+---------+
                                             |
                          +------------------v------------------+
                          | ARM7TDMI interpreter (ARM + Thumb)  |
                          |  bus, I/O, timers, DMA, IRQ, saves  |
                          |  software BIOS (SWI)                |
                          +------------------+------------------+
                                             |  240x160 frame (BGR555)
                                  scanline renderer (PPU)
                                             |
                           1.5x scale + rotate to the 240x432 framebuffer
```

- **Interpreter:** `src/cpu.c` decodes ARM and Thumb with dense jump tables. Busy-wait loops are detected and
  fast-forwarded to the next event, and halting waits sleep until an interrupt.
- **Memory and I/O:** `src/mem.c` is the bus, the I/O registers, timers, DMA, interrupts, SRAM/Flash save memory and the
  frame loop.
- **Video:** `src/ppu.c` renders lines lazily; when the device falls behind, whole frames are not drawn (render-skipping)
  while emulation continues at full rate.
- **BIOS:** `src/bios.c` implements the software interrupts in C. No BIOS image is needed or included.
- **ROM cache:** the ROM is addressed through a table of 32 KiB pages. A miss reads the page file into a slot; the
  replacement is least-recently-used on per-frame stamps. The number of slots (16 to 256) is computed at launch from the
  free heap, leaving about 1.5 MiB for the system. A list of the pages that were resident is kept in `hot.bin` and
  preloaded after a reload so the game does not stall.
- **Display:** the 240x160 picture is scaled 1.5x to 360x240 and rotated onto the 240x432 framebuffer in landscape; in
  portrait it is copied 1:1 with the controls below it.
- **Front end:** `src/lines.c` holds the menu, the touch mapping, the pacing and the memory-release cycle.

## FAQ

**Are ROMs or a BIOS included?**
No. Use your own dumps of cartridges you own. The BIOS is implemented in software, so no BIOS file is needed.

**Where do I get ROMs?**
Dump them from cartridges you own. This project does not provide or link to any.

**Does it have sound?**
No. There is no audio output.

**Why not just use the BIOS from a real console?**
The software BIOS keeps the project free of copyrighted data and covers the calls games commonly use.

**My game does not save.**
EEPROM saves are not written yet. SRAM and Flash games are. Make sure the data partition was unmounted cleanly.

**The app says the ROM is not found.**
The folder must be `/Apps/Data/GBA/<name>/` with `meta.bin` and `p000.bin`. Run the file through
`tools/prepare_rom.py` and copy the whole folder.

## Risks

- This writes the iPod's app pack and ROM data. The installer verifies every write and can roll back, but a mistake with
  the wrong device can still damage data. Check the device path, keep the data partition unmounted, and **back up the
  iPod before use.** Do not connect a modified iPod to iTunes or restore it with Apple software unless you know what
  you are doing.
- Do not remove power or the cable while a script is writing.
- The test suite runs on a PC with synthetic programs. It is not a substitute for testing on a device.
- No warranty (see `LICENSE`).

## Tests

```sh
tests/run_tests.sh --sdk /path/to/NanoApps   # core, prepare_rom, front end (needs gcc, python3, SDK headers)
tests/test_scripts.sh                        # installer, updater, remover, ROM copy on a FAT32 image (mtools, dosfstools)
```

All ROMs and programs used by the tests are generated by the tests. `tests/README.md` lists what is covered.

## Project layout

```
src/     the app: lines.c (menu, input, display, memory), cpu.c, mem.c, ppu.c, bios.c, gba.h, Makefile, Info.plist
tools/   prepare_rom.py, build.sh, check_arm.sh, install / update / remove / copy-rom scripts
tests/   core test (synthetic ARM/Thumb programs), front-end simulation, script tests
docs/    ROM folder layout
```

## Acknowledgments

- [olievans123/nano7-untethered](https://github.com/olievans123/nano7-untethered): the untethered firmware and loader
  that make homebrew apps possible on this device.
- [nfzerox/NanoApps](https://github.com/nfzerox/NanoApps): the app SDK Lines is built against.
- The public GBA hardware documentation (GBATEK) was used as a reference for hardware behaviour.

## License

MIT License, Copyright (c) 2026 tenspav (see `LICENSE`). Provenance review and third-party notes: `THIRD_PARTY.md`.
