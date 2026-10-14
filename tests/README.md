# Tests

Everything here runs on a Linux PC, without an iPod and without root. Nothing is downloaded. There are no ROM
images and no BIOS file in this repository: every test builds its own synthetic ROMs and programs.

```sh
tests/run_tests.sh --sdk /path/to/NanoApps   # core + prepare_rom + front end
tests/test_scripts.sh                        # install / update / remove / copy-rom on a FAT32 image
```

Needs: `gcc`, `python3` (standard library only); for `test_scripts.sh` also `mtools` and `dosfstools`. The front-end
suite needs the SDK headers (`sdk/*.h` of a NanoApps checkout); without `--sdk` it is skipped.

## What is covered

| File | What it does |
|---|---|
| `core_test.c` | The emulator core (`src/cpu.c`, `mem.c`, `ppu.c`, `bios.c`) running tiny ARM and Thumb programs written as machine-code words: ALU, flags, conditional execution, ARM to Thumb switch, SWI Div, LZ77 decompression, a mode 3 bitmap frame, SRAM writes, IRQ plus VBlankIntrWait, save-type detection |
| `mkrom.py` | Writes small synthetic GBA ROM images (valid header byte, optional save-library tag) |
| `run_ui_tests.sh` + `lines_host.c` | The real `src/lines.c` front end with the core stubbed: ROM scan and sorting, menu taps and scrolling, per-ROM saves and states, an existing `save.sav` is kept, ROMS hold, EXIT and idle release and resume, relaunch cases, leak checks over repeated ROM switching, single ROM / no ROM / vanished ROM |
| `test_scripts.sh` | The shell tools against a FAT32 image: install, rollback after an injected failure, update (idempotent, with rollback), remove, slot detection, refusals, ROM copy |
| `run_tests.sh` | Runs the core test, checks `prepare_rom.py` on synthetic ROMs, then the front end |

## Limits

These tests exercise the emulator on small hand-written programs. They are not a game compatibility suite and they do
not replace testing on a device.
