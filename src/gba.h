/*
 * gba.h - a small, portable Game Boy Advance core (original implementation).
 * No libc dependency beyond what the platform layer provides, so the same
 * sources build natively on a PC (for testing) and freestanding for the iPod.
 */
#ifndef GBA_CORE_H
#define GBA_CORE_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uint64_t u64;
typedef int64_t  s64;

#define GBA_W 240
#define GBA_H 160

/* ROM is accessed through a page table so the platform can page it in on demand. */
#define ROM_PAGE_SHIFT 15
#define ROM_PAGE_SIZE  (1u << ROM_PAGE_SHIFT)
#define ROM_MAX_PAGES  1024            /* 32 MiB address space / 32 KiB */

/* CPSR bits */
#define F_N 0x80000000u
#define F_Z 0x40000000u
#define F_C 0x20000000u
#define F_V 0x10000000u
#define PSR_I 0x80u
#define PSR_F 0x40u
#define PSR_T 0x20u

enum { MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
       MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F };

/* I/O register offsets (relative to 0x04000000) */
#define IO_DISPCNT   0x000
#define IO_DISPSTAT  0x004
#define IO_VCOUNT    0x006
#define IO_BG0CNT    0x008
#define IO_BG0HOFS   0x010
#define IO_BG2PA     0x020
#define IO_BG2X      0x028
#define IO_BG3PA     0x030
#define IO_BG3X      0x038
#define IO_WIN0H     0x040
#define IO_WININ     0x048
#define IO_MOSAIC    0x04C
#define IO_BLDCNT    0x050
#define IO_BLDALPHA  0x052
#define IO_BLDY      0x054
#define IO_DMA0SAD   0x0B0
#define IO_TM0CNT_L  0x100
#define IO_KEYINPUT  0x130
#define IO_KEYCNT    0x132
#define IO_IE        0x200
#define IO_IF        0x202
#define IO_WAITCNT   0x204
#define IO_IME       0x208
#define IO_POSTFLG   0x300
#define IO_HALTCNT   0x301

enum { SAVE_NONE = 0, SAVE_SRAM, SAVE_FLASH64, SAVE_FLASH128, SAVE_EEPROM };

typedef struct GBA GBA;

typedef struct {
    u32 reload, counter;       /* counter valid at 'stamp' */
    u64 stamp;
    u16 ctl;
    u8  active;
} Timer;

typedef struct {
    u32 sad, dad, cnt;         /* latched working copies */
    u32 rsad, rdad;            /* raw register values */
    u16 rcnt, rctl;
    u8  active;
} Dma;

struct GBA {
    /* ---- CPU ---- */
    u32 r[16];                 /* r[15] = address of current instruction + 8 (ARM) / + 4 (Thumb) */
    u32 cpsr, spsr;
    u32 bank_sp[6], bank_lr[6], bank_spsr[6];
    u32 usr_r8_12[5], fiq_r8_12[5];
    u64 cycles;                /* total emulated CPU cycles */
    u64 next_event;
    int halted;
    int irq_line;              /* IE & IF & IME, cached */
    int branched;              /* set when an instruction wrote the PC */
    int skip_render;           /* platform sets this to skip drawing scanlines (frameskip) */
    int render_upto;           /* lazy renderer: lines [0, render_upto) of this frame are drawn */
    u64 halt_cycles;           /* statistics: cycles fast-forwarded while halted/idle */
    /* idle (busy-wait) loop detection */
    u32 store_seq, io_seq;     /* bumped on every store / I/O read */
    u32 idle_pc, idle_sig, idle_stores, idle_io;
    int idle, idle_n;
    int intrwait;              /* HLE IntrWait in progress (mask in intrwait_mask) */
    u32 intrwait_mask;
    /* m4a sound driver: skip the PCM mixing (no audio output) but keep channel state */
    int sound_hle;             /* platform enables it */
    u32 mix_pc, rev_pc;        /* ARM entry points found in IWRAM (0 = not found yet) */
    /* ---- memory ---- */
    u8  bios[0x4000];
    u8  ewram[0x40000];
    u8  iwram[0x8000];
    u8  pal[0x400];
    u8  vram[0x18000];
    u8  oam[0x400];
    u8  io[0x400];
    u8  sram[0x20000];         /* SRAM / Flash backing store (128 KiB max) */

    /* ROM through pages (platform fills / faults them in) */
    const u8 *rom_tab[ROM_MAX_PAGES];
    u8 rom_ref[ROM_MAX_PAGES]; /* set on every ROM read; used by the platform's page cache */
    u32 rom_size;
    const u8 *(*rom_fault)(GBA *g, u32 page);
    void *user;

    /* ---- video ---- */
    int vcount;
    u32 line_cycle;
    u16 frame[GBA_W * GBA_H];  /* BGR555 */
    u16 line_buf[GBA_W];
    s32 bg2x, bg2y, bg3x, bg3y; /* internal affine reference points */
    int frame_ready;

    /* ---- peripherals ---- */
    Timer tm[4];
    Dma dma[4];
    u16 keys;                  /* 1 = pressed, bits as KEYINPUT inverted */
    int save_type;
    int flash_state, flash_bank, flash_id_mode, flash_erase;
    int flash_dirty;
    u8  eeprom_dummy;
    u32 open_bus;
};

/* ---- API ---- */
void gba_reset(GBA *g);                       /* boots straight into the ROM, BIOS skipped */
void gba_set_rom(GBA *g, u32 size, const u8 *(*fault)(GBA *, u32 page), void *user);
void gba_run_frame(GBA *g);
void gba_detect_save(GBA *g, const u8 *rom_head, u32 head_len);

/* ---- internals shared between modules ---- */
u32  mem_read32(GBA *g, u32 a);
u32  mem_read16(GBA *g, u32 a);
u32  mem_read8 (GBA *g, u32 a);
void mem_write32(GBA *g, u32 a, u32 v);
void mem_write16(GBA *g, u32 a, u32 v);
void mem_write8 (GBA *g, u32 a, u32 v);
void io_write16(GBA *g, u32 off, u16 v);
void irq_raise(GBA *g, u16 mask);
void irq_update(GBA *g);
void timers_sync(GBA *g);
u64  timers_next_event(GBA *g);
void dma_trigger(GBA *g, int timing);         /* 1 = VBlank, 2 = HBlank */
void cpu_run(GBA *g, u64 until);
void cpu_take_irq(GBA *g);
int  bios_swi(GBA *g, u32 num);               /* returns 1 if SWI should be re-executed (HLE wait) */
void ppu_render_line(GBA *g, int y);
void ppu_sync(GBA *g);                        /* draw every line that is due, before a display write */

/* called before any write that could change what a scanline looks like */
static inline void ppu_before_write(GBA *g)
{
    if (!g->skip_render && g->render_upto < 160 && g->vcount < 160) ppu_sync(g);
}
void set_cpsr(GBA *g, u32 v);
void sound_hle_scan(GBA *g);                  /* once per frame: locate / re-validate the mixer */
u32  gba_hle_status(GBA *g);                  /* bit0 sound mixer HLE, bit1 title-specific sprite HLE */
extern u32 gba_icount;                        /* instructions executed (statistics) */

static inline u8 rd_rom8(GBA *g, u32 off)
{
    u32 pg = (off >> ROM_PAGE_SHIFT) & (ROM_MAX_PAGES - 1);
    const u8 *p = g->rom_tab[pg];
    if (!p) p = g->rom_fault(g, pg);
    g->rom_ref[pg] = 1;
    return p[off & (ROM_PAGE_SIZE - 1)];
}

void gba_set_save(GBA *g, int type);          /* choose the save type and start with erased memory */

#endif
