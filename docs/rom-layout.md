# ROM folder layout

On the iPod, one folder under `/Apps/Data/GBA/` is one game: `/Apps/Data/GBA/<name>/`.

| File | Written by | Content |
|---|---|---|
| `meta.bin` | `tools/prepare_rom.py` | 8 bytes: u32 little-endian ROM size, u8 save type (0 none, 1 SRAM, 2 Flash 64K, 3 Flash 128K, 4 EEPROM), 3 zero bytes |
| `p000.bin`, `p001.bin` ... | `tools/prepare_rom.py` | the ROM in 32 KiB pages, the last one zero padded; `p1000.bin` and up for ROMs larger than 31 MB |
| `save.sav` | the app | cartridge save memory (SRAM or Flash), created on first save |
| `state.bin` | the app | full save state |
| `hot.bin` | the app | list of the ROM pages that were resident at the last state save (a preload hint) |

Other files:

- `/Apps/Data/GBA/last.txt`: folder name of the last game played.
- `/Apps/Data/GBA/keep.bin`: a small marker that tells the next launch whether memory was released or a game was running.

## Rules

- Names use `a-z 0-9 _ -`, at most 31 characters. The menu shows them in upper case.
- ROMs up to 32 MiB (the GBA address space). A larger ROM is listed but cannot be started.
- A folder without a valid `meta.bin` plus `p000.bin` is ignored by the menu.
- The cache is not limited by the ROM size: it holds 16 to 256 pages and the rest is read on demand.
- EEPROM games load but their saves are not written yet.

## Behaviour summary

- 2 or more ROMs: the menu opens with the last game highlighted. 1 ROM: starts directly. Reopening without closing
  continues the game.
- After `EXIT` or ten idle minutes, a tap (or the next launch) resumes the last game, not the menu. After `ROMS`,
  opening Lines shows the menu.
- Never use the SDK's `--demo-data-force` option: it wipes `/Apps/Data/GBA` including your saves.
