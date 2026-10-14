# Lines app Makefile for the NanoApps SDK build system.
# Usage: copy src/ to <NanoApps>/apps/lines and run make there (tools/build.sh does exactly that).
APP_NAME    := lines
SRCS        := lines.c mem.c cpu.c bios.c ppu.c
RAW_SURFACE := 1
EXTRA_CFLAGS += -O2 -fjump-tables -mword-relocations -fno-tree-loop-distribute-patterns
include ../../sdk/hb_app.mk
