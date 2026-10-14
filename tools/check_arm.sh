#!/usr/bin/env bash
# Sanity check of the built ARM ELF (usage: check_arm.sh path/to/lines.reloc.elf).
#  - the emulator core and the front end must be present (gba_run_frame, cpu_run, ppu_render_line, bios_swi)
#  - no VFP/NEON instructions anywhere: the core is integer-only and the app must link against soft-float code
set -euo pipefail
E=${1:?usage: check_arm.sh ELF}
miss=0
for s in gba_run_frame cpu_run ppu_render_line bios_swi; do
  arm-none-eabi-nm "$E" | grep -qE " [Tt] $s\$" || { echo "missing symbol: $s"; miss=1; }
done
v=$(arm-none-eabi-objdump -d "$E" | grep -ciE "\bv(ld|st|add|sub|mul|mov|cvt|cmp|div|sqrt)[a-z0-9.]*\s" || true)
echo "core symbols missing: $miss; VFP/NEON instructions: $v"
if [ "$miss" = 0 ] && [ "$v" = 0 ]; then echo CHECK_ARM_OK; else echo CHECK_ARM_FAILED; exit 1; fi
