/* mem.c - bus, I/O registers, timers, DMA, interrupts, save memory, frame loop */
#include "gba.h"

#define IO16(g, off) (*(u16 *)&(g)->io[(off)])

/* ------------------------------------------------------------ interrupts --- */

void irq_update(GBA *g)
{
    u16 ie = IO16(g, IO_IE), iff = IO16(g, IO_IF);
    g->irq_line = (IO16(g, IO_IME) & 1) && (ie & iff & 0x3FFF);
    if (ie & iff & 0x3FFF) g->halted = 0;
}

void irq_raise(GBA *g, u16 mask)
{
    IO16(g, IO_IF) |= mask;
    irq_update(g);
}

/* ---------------------------------------------------------------- timers --- */

static const u8 TM_SHIFT[4] = { 0, 6, 8, 10 };

static void timer_overflow(GBA *g, int i, u32 count)
{
    if (!count) return;
    if (g->tm[i].ctl & 0x40) irq_raise(g, (u16)(1u << (3 + i)));
    if (i < 3 && (g->tm[i + 1].ctl & 0x84) == 0x84 && g->tm[i + 1].active) {
        Timer *n = &g->tm[i + 1];
        u32 c = n->counter + count;
        u32 ov = 0;
        while (c >= 0x10000) { c = c - 0x10000 + n->reload; ov++; if (!ov) break; }
        n->counter = c;
        timer_overflow(g, i + 1, ov);
    }
}

void timers_sync(GBA *g)
{
    for (int i = 0; i < 4; i++) {
        Timer *t = &g->tm[i];
        if (!t->active || (t->ctl & 4)) { t->stamp = g->cycles; continue; }
        int sh = TM_SHIFT[t->ctl & 3];
        u64 el = g->cycles - t->stamp;
        u64 ticks = el >> sh;
        if (!ticks) continue;
        t->stamp += ticks << sh;
        u64 c = t->counter + ticks;
        u32 ov = 0;
        if (c >= 0x10000) {
            u32 span = 0x10000 - t->reload;
            if (span == 0) span = 0x10000;
            c -= 0x10000;
            ov = 1 + (u32)(c / span);
            c = t->reload + (u32)(c % span);
        }
        t->counter = (u32)c;
        timer_overflow(g, i, ov);
    }
}

u64 timers_next_event(GBA *g)
{
    u64 best = g->cycles + 0x100000;
    for (int i = 0; i < 4; i++) {
        Timer *t = &g->tm[i];
        if (!t->active || (t->ctl & 4)) continue;
        int sh = TM_SHIFT[t->ctl & 3];
        u64 ticks = 0x10000 - t->counter;
        u64 at = t->stamp + (ticks << sh);
        if (at <= g->cycles) at = g->cycles + 1;
        if (at < best) best = at;
    }
    return best;
}

static void timer_write_ctl(GBA *g, int i, u16 v)
{
    Timer *t = &g->tm[i];
    timers_sync(g);
    int was = t->active;
    t->ctl = v;
    t->active = (v >> 7) & 1;
    if (t->active && !was) { t->counter = t->reload; t->stamp = g->cycles; }
}

/* ------------------------------------------------------------------- DMA --- */

static void dma_run(GBA *g, int n)
{
    Dma *d = &g->dma[n];
    u16 ctl = d->rctl;
    g->idle = 0; g->idle_pc = 0;
    int w32 = (ctl >> 10) & 1;
    int sinc = (ctl >> 7) & 3, dinc = (ctl >> 5) & 3;
    u32 cnt = d->cnt;
    u32 sad = d->sad, dad = d->dad;
    int sstep = (sinc == 1) ? -1 : (sinc == 2 ? 0 : 1);
    int dstep = (dinc == 1) ? -1 : (dinc == 2 ? 0 : 1);
    int size = w32 ? 4 : 2;

    for (u32 i = 0; i < cnt; i++) {
        if (w32) mem_write32(g, dad, mem_read32(g, sad & ~3u));
        else     mem_write16(g, dad, mem_read16(g, sad & ~1u));
        sad += (u32)(sstep * size);
        dad += (u32)(dstep * size);
    }
    g->cycles += 2 + (u64)cnt * ((sad >> 24) >= 8 ? 4 : 2);

    d->sad = sad;
    d->dad = (dinc == 3) ? d->rdad : dad;
    int timing = (ctl >> 12) & 3;
    if (ctl & 0x4000) irq_raise(g, (u16)(0x100u << n));
    if ((ctl & 0x200) && timing != 0) {
        u32 c = d->rcnt;
        d->cnt = c ? c : (n == 3 ? 0x10000u : 0x4000u);
        if (dinc == 3) d->dad = d->rdad;
    } else {
        d->active = 0;
        d->rctl &= (u16)~0x8000;
        IO16(g, IO_DMA0SAD + n * 12 + 10) = d->rctl;
    }
}

void dma_trigger(GBA *g, int timing)
{
    for (int i = 0; i < 4; i++) {
        Dma *d = &g->dma[i];
        if (d->active && (((d->rctl >> 12) & 3) == timing)) dma_run(g, i);
    }
}

static void dma_write(GBA *g, int n, u32 reg, u16 v)
{
    Dma *d = &g->dma[n];
    switch (reg) {
    case 0: d->rsad = (d->rsad & 0xFFFF0000u) | v; break;
    case 2: d->rsad = (d->rsad & 0x0000FFFFu) | ((u32)(v & (n == 0 ? 0x07FF : 0x0FFF)) << 16); break;
    case 4: d->rdad = (d->rdad & 0xFFFF0000u) | v; break;
    case 6: d->rdad = (d->rdad & 0x0000FFFFu) | ((u32)(v & (n == 3 ? 0x0FFF : 0x07FF)) << 16); break;
    case 8: d->rcnt = v; break;
    case 10: {
        int was = (d->rctl >> 15) & 1;
        d->rctl = v;
        IO16(g, IO_DMA0SAD + n * 12 + 10) = v;
        if ((v >> 15) & 1) {
            if (!was) {
                d->sad = d->rsad; d->dad = d->rdad;
                u32 c = d->rcnt;
                d->cnt = c ? c : (n == 3 ? 0x10000u : 0x4000u);
                d->active = 1;
                if (((v >> 12) & 3) == 0) dma_run(g, n);
            }
        } else d->active = 0;
        break;
    }
    }
}

/* ------------------------------------------------------------ I/O access --- */

static u32 io_read16(GBA *g, u32 off)
{
    off &= 0x3FE;
    switch (off) {
    case IO_DISPSTAT: {
        u16 v = IO16(g, IO_DISPSTAT) & 0xFFB8;
        if (g->vcount >= 160 && g->vcount < 227) v |= 1;
        if (g->line_cycle >= 960) v |= 2;
        if (g->vcount == (v >> 8)) v |= 4;
        return v;
    }
    case IO_VCOUNT: return (u32)g->vcount;
    case IO_KEYINPUT: return (u16)~g->keys & 0x3FF;
    case IO_TM0CNT_L: case IO_TM0CNT_L + 4: case IO_TM0CNT_L + 8: case IO_TM0CNT_L + 12:
        timers_sync(g);
        return g->tm[(off - IO_TM0CNT_L) >> 2].counter & 0xFFFF;
    case IO_TM0CNT_L + 2: case IO_TM0CNT_L + 6: case IO_TM0CNT_L + 10: case IO_TM0CNT_L + 14:
        return g->tm[(off - IO_TM0CNT_L - 2) >> 2].ctl;
    default:
        if (off >= IO_DMA0SAD && off < IO_DMA0SAD + 48) {
            u32 r = (off - IO_DMA0SAD) % 12;
            return r == 10 ? IO16(g, off) : 0;
        }
        return IO16(g, off);
    }
}

void io_write16(GBA *g, u32 off, u16 v)
{
    off &= 0x3FE;
    if (off >= IO_TM0CNT_L && off < IO_TM0CNT_L + 16) {
        int i = (off - IO_TM0CNT_L) >> 2;
        if ((off & 2) == 0) g->tm[i].reload = v;
        else timer_write_ctl(g, i, v);
        return;
    }
    if (off >= IO_DMA0SAD && off < IO_DMA0SAD + 48) {
        dma_write(g, (off - IO_DMA0SAD) / 12, (off - IO_DMA0SAD) % 12, v);
        return;
    }
    switch (off) {
    case IO_DISPSTAT:
        IO16(g, off) = (u16)((IO16(g, off) & 7) | (v & 0xFFF8));
        return;
    case IO_VCOUNT: case IO_KEYINPUT: return;
    case IO_IF:
        IO16(g, IO_IF) &= (u16)~v;
        irq_update(g);
        return;
    case IO_IE: case IO_IME:
        IO16(g, off) = v;
        irq_update(g);
        return;
    case IO_BG2X: case IO_BG2X + 2: case IO_BG2X + 4: case IO_BG2X + 6:
    case IO_BG3X: case IO_BG3X + 2: case IO_BG3X + 4: case IO_BG3X + 6:
        IO16(g, off) = v;
        {   /* writing a reference point reloads the internal copy */
            u32 b = off & ~7u; u32 lo = IO16(g, b), hi = IO16(g, b + 2) & 0x0FFF;
            s32 x = (s32)((hi << 16) | lo); x = (x << 4) >> 4;
            u32 yl = IO16(g, b + 4), yh = IO16(g, b + 6) & 0x0FFF;
            s32 y = (s32)((yh << 16) | yl); y = (y << 4) >> 4;
            if (b == IO_BG2X) { g->bg2x = x; g->bg2y = y; } else { g->bg3x = x; g->bg3y = y; }
        }
        return;
    default:
        IO16(g, off) = v;
        return;
    }
}

static void io_write8(GBA *g, u32 off, u32 v)
{
    off &= 0x3FF;
    if (off == IO_HALTCNT) { g->halted = 1; irq_update(g); return; }
    if (off == IO_POSTFLG) { g->io[off] = (u8)v; return; }
    u32 base = off & ~1u;
    u16 cur = (u16)io_read16(g, base);
    if (off & 1) cur = (u16)((cur & 0x00FF) | (v << 8));
    else         cur = (u16)((cur & 0xFF00) | (v & 0xFF));
    /* IF is write-1-to-clear: a byte write must only touch its own byte */
    if (base == IO_IF) cur = (u16)((off & 1) ? ((v & 0xFF) << 8) : (v & 0xFF));
    io_write16(g, base, cur);
}

/* ---------------------------------------------------------------- flash --- */

static u32 save_read8(GBA *g, u32 a)
{
    a &= 0xFFFF;
    if (g->save_type == SAVE_SRAM) return g->sram[a & 0x7FFF];
    if (g->save_type == SAVE_FLASH64 || g->save_type == SAVE_FLASH128) {
        if (g->flash_id_mode) {
            if (a == 0) return g->save_type == SAVE_FLASH128 ? 0xC2 : 0x32;
            if (a == 1) return g->save_type == SAVE_FLASH128 ? 0x09 : 0x1B;
        }
        return g->sram[((u32)g->flash_bank << 16) | a];
    }
    return 0xFF;
}

static void save_write8(GBA *g, u32 a, u32 v)
{
    a &= 0xFFFF; v &= 0xFF;
    if (g->save_type == SAVE_SRAM) { g->sram[a & 0x7FFF] = (u8)v; g->flash_dirty = 1; return; }
    if (g->save_type != SAVE_FLASH64 && g->save_type != SAVE_FLASH128) return;

    switch (g->flash_state) {
    case 0: if (a == 0x5555 && v == 0xAA) g->flash_state = 1; break;
    case 1: g->flash_state = (a == 0x2AAA && v == 0x55) ? 2 : 0; break;
    case 2:
        if (a == 0x5555) {
            switch (v) {
            case 0x90: g->flash_id_mode = 1; g->flash_state = 0; break;
            case 0xF0: g->flash_id_mode = 0; g->flash_state = 0; break;
            case 0x80: g->flash_erase = 1; g->flash_state = 0; break;
            case 0xA0: g->flash_state = 3; break;
            case 0xB0: g->flash_state = 4; break;
            case 0x10:
                if (g->flash_erase) {
                    for (u32 i = 0; i < 0x20000; i++) g->sram[i] = 0xFF;
                    g->flash_dirty = 1;
                }
                g->flash_erase = 0; g->flash_state = 0; break;
            default: g->flash_state = 0; break;
            }
        } else if (v == 0x30 && g->flash_erase) {
            u32 base = ((u32)g->flash_bank << 16) | (a & 0xF000);
            for (u32 i = 0; i < 0x1000; i++) g->sram[base + i] = 0xFF;
            g->flash_dirty = 1;
            g->flash_erase = 0; g->flash_state = 0;
        } else g->flash_state = 0;
        break;
    case 3:
        g->sram[((u32)g->flash_bank << 16) | a] = (u8)v;
        g->flash_dirty = 1;
        g->flash_state = 0;
        break;
    case 4:
        if (a == 0) g->flash_bank = (int)(v & 1);
        g->flash_state = 0;
        break;
    }
}

void gba_set_save(GBA *g, int type)
{
    g->save_type = type;
    for (u32 i = 0; i < sizeof g->sram; i++) g->sram[i] = 0xFF;
}

void gba_detect_save(GBA *g, const u8 *rom, u32 len)
{
    g->save_type = SAVE_NONE;
    for (u32 i = 0; i < sizeof g->sram; i++) g->sram[i] = 0xFF;
    for (u32 i = 0; i + 8 < len; i += 4) {
        const u8 *p = rom + i;
        if (p[0] == 'F' && p[1] == 'L' && p[2] == 'A' && p[3] == 'S' && p[4] == 'H') {
            if (p[5] == '1' && p[6] == 'M') g->save_type = SAVE_FLASH128;
            else g->save_type = SAVE_FLASH64;
            return;
        }
        if (p[0] == 'S' && p[1] == 'R' && p[2] == 'A' && p[3] == 'M') { g->save_type = SAVE_SRAM; return; }
        if (p[0] == 'E' && p[1] == 'E' && p[2] == 'P' && p[3] == 'R' && p[4] == 'O' && p[5] == 'M') {
            g->save_type = SAVE_EEPROM; return;
        }
    }
}

/* ------------------------------------------------------------------ bus --- */

u32 mem_read8(GBA *g, u32 a)
{
    switch (a >> 24) {
    case 0x00: return a < 0x4000 ? g->bios[a] : 0;
    case 0x02: return g->ewram[a & 0x3FFFF];
    case 0x03: return g->iwram[a & 0x7FFF];
    case 0x04: { g->io_seq++; u32 v = io_read16(g, a & 0x3FE); return (a & 1) ? (v >> 8) & 0xFF : v & 0xFF; }
    case 0x05: return g->pal[a & 0x3FF];
    case 0x06: { u32 o = a & 0x1FFFF; if (o >= 0x18000) o -= 0x8000; return g->vram[o]; }
    case 0x07: return g->oam[a & 0x3FF];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        u32 o = a & 0x1FFFFFF;
        if (o >= g->rom_size) return (o >> 1) >> ((a & 1) * 8) & 0xFF;
        return rd_rom8(g, o);
    }
    case 0x0E: case 0x0F: return save_read8(g, a);
    default: return 0;
    }
}

u32 mem_read16(GBA *g, u32 a)
{
    a &= ~1u;
    switch (a >> 24) {
    case 0x02: return *(u16 *)&g->ewram[a & 0x3FFFE];
    case 0x03: return *(u16 *)&g->iwram[a & 0x7FFE];
    case 0x04: g->io_seq++; return io_read16(g, a & 0x3FE);
    case 0x05: return *(u16 *)&g->pal[a & 0x3FE];
    case 0x06: { u32 o = a & 0x1FFFE; if (o >= 0x18000) o -= 0x8000; return *(u16 *)&g->vram[o]; }
    case 0x07: return *(u16 *)&g->oam[a & 0x3FE];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        u32 o = a & 0x1FFFFFE;
        if (o >= g->rom_size) return (o >> 1) & 0xFFFF;
        u32 pg = o >> ROM_PAGE_SHIFT;
        const u8 *p = g->rom_tab[pg];
        if (!p) p = g->rom_fault(g, pg);
        g->rom_ref[pg] = 1;
        return *(const u16 *)(p + (o & (ROM_PAGE_SIZE - 1)));
    }
    case 0x0E: case 0x0F: { u32 v = save_read8(g, a); return v | (v << 8); }
    default: return a < 0x4000 && (a >> 24) == 0 ? *(u16 *)&g->bios[a] : 0;
    }
}

u32 mem_read32(GBA *g, u32 a)
{
    a &= ~3u;
    switch (a >> 24) {
    case 0x02: return *(u32 *)&g->ewram[a & 0x3FFFC];
    case 0x03: return *(u32 *)&g->iwram[a & 0x7FFC];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        u32 o = a & 0x1FFFFFC;
        if (o >= g->rom_size) return ((o >> 1) & 0xFFFF) | ((((o + 2) >> 1) & 0xFFFF) << 16);
        u32 pg = o >> ROM_PAGE_SHIFT;
        const u8 *p = g->rom_tab[pg];
        if (!p) p = g->rom_fault(g, pg);
        g->rom_ref[pg] = 1;
        return *(const u32 *)(p + (o & (ROM_PAGE_SIZE - 1)));
    }
    case 0x00: return a < 0x4000 ? *(u32 *)&g->bios[a] : 0;
    case 0x0E: case 0x0F: return save_read8(g, a) * 0x01010101u;
    default: return mem_read16(g, a) | (mem_read16(g, a + 2) << 16);
    }
}

void mem_write8(GBA *g, u32 a, u32 v)
{
    g->store_seq++;
    switch (a >> 24) {
    case 0x02: g->ewram[a & 0x3FFFF] = (u8)v; break;
    case 0x03: g->iwram[a & 0x7FFF] = (u8)v; break;
    case 0x04: if ((a & 0x3FF) < 0x60) ppu_before_write(g); io_write8(g, a, v); break;
    case 0x05: ppu_before_write(g); *(u16 *)&g->pal[a & 0x3FE] = (u16)((v & 0xFF) * 0x0101); break;
    case 0x06: {
        ppu_before_write(g);
        u32 o = a & 0x1FFFF; if (o >= 0x18000) o -= 0x8000;
        u32 lim = ((IO16(g, IO_DISPCNT) & 7) >= 3) ? 0x14000 : 0x10000;
        if (o < lim) *(u16 *)&g->vram[o & ~1u] = (u16)((v & 0xFF) * 0x0101);
        break;
    }
    case 0x0E: case 0x0F: save_write8(g, a, v); break;
    default: break;
    }
}

#ifdef GBA_DEBUG
#include <stdio.h>
extern int gba_dbg_on;
#endif

void mem_write16(GBA *g, u32 a, u32 v)
{
    u32 orig = a;
    g->store_seq++;
    a &= ~1u;
#ifdef GBA_DEBUG
    if (gba_dbg_on && (a == 0x0300310C || a == 0x03007FF8))
        printf("WRITE16 %08X <- %04X  (pc=%08X cpsr=%08X)\n", a, v & 0xFFFF, g->r[15] - ((g->cpsr & PSR_T) ? 4 : 8), g->cpsr);
#endif
    switch (a >> 24) {
    case 0x02: *(u16 *)&g->ewram[a & 0x3FFFE] = (u16)v; break;
    case 0x03: *(u16 *)&g->iwram[a & 0x7FFE] = (u16)v; break;
    case 0x04: if ((a & 0x3FE) < 0x60) ppu_before_write(g); io_write16(g, a & 0x3FE, (u16)v); break;
    case 0x05: ppu_before_write(g); *(u16 *)&g->pal[a & 0x3FE] = (u16)v; break;
    case 0x06: { ppu_before_write(g); u32 o = a & 0x1FFFE; if (o >= 0x18000) o -= 0x8000; *(u16 *)&g->vram[o] = (u16)v; break; }
    case 0x07: ppu_before_write(g); *(u16 *)&g->oam[a & 0x3FE] = (u16)v; break;
    case 0x0E: case 0x0F: save_write8(g, orig, v >> ((orig & 1) * 8)); break;
    default: break;
    }
}

void mem_write32(GBA *g, u32 a, u32 v)
{
    u32 orig = a;
    g->store_seq++;
    a &= ~3u;
    switch (a >> 24) {
    case 0x02: *(u32 *)&g->ewram[a & 0x3FFFC] = v; break;
    case 0x03: *(u32 *)&g->iwram[a & 0x7FFC] = v; break;
    case 0x0E: case 0x0F: save_write8(g, orig, v >> ((orig & 3) * 8)); break;
    default: mem_write16(g, a, v & 0xFFFF); mem_write16(g, a + 2, v >> 16); break;
    }
}

/* ------------------------------------------------------------ boot / loop --- */

static const u32 IRQ_STUB[] = {
    0xEA000042,            /* 0x18: b 0x128                                */
};
static const u32 IRQ_HANDLER[] = {
    0xE92D500F,            /* stmfd sp!, {r0-r3, r12, lr}                  */
    0xE3A00301,            /* mov   r0, #0x04000000                        */
    0xE28FE000,            /* add   lr, pc, #0                             */
    0xE510F004,            /* ldr   pc, [r0, #-4]                          */
    0xE8BD500F,            /* ldmfd sp!, {r0-r3, r12, lr}                  */
    0xE25EF004,            /* subs  pc, lr, #4                             */
};

void gba_set_rom(GBA *g, u32 size, const u8 *(*fault)(GBA *, u32), void *user)
{
    g->rom_size = size;
    g->rom_fault = fault;
    g->user = user;
    for (int i = 0; i < ROM_MAX_PAGES; i++) { g->rom_tab[i] = 0; g->rom_ref[i] = 0; }
}

void gba_reset(GBA *g)
{
    u8 *p = (u8 *)g;
    (void)p;
    for (u32 i = 0; i < sizeof g->ewram; i++) g->ewram[i] = 0;
    for (u32 i = 0; i < sizeof g->iwram; i++) g->iwram[i] = 0;
    for (u32 i = 0; i < sizeof g->pal; i++) g->pal[i] = 0;
    for (u32 i = 0; i < sizeof g->vram; i++) g->vram[i] = 0;
    for (u32 i = 0; i < sizeof g->oam; i++) g->oam[i] = 0;
    for (u32 i = 0; i < sizeof g->io; i++) g->io[i] = 0;
    for (u32 i = 0; i < sizeof g->bios; i++) g->bios[i] = 0;
    for (int i = 0; i < 4; i++) { g->tm[i].reload = 0; g->tm[i].counter = 0; g->tm[i].ctl = 0; g->tm[i].active = 0; g->tm[i].stamp = 0; }
    for (int i = 0; i < 4; i++) { g->dma[i].active = 0; g->dma[i].rctl = 0; g->dma[i].rsad = 0; g->dma[i].rdad = 0; g->dma[i].rcnt = 0; }

    *(u32 *)&g->bios[0x18] = IRQ_STUB[0];
    for (int i = 0; i < 6; i++) *(u32 *)&g->bios[0x128 + i * 4] = IRQ_HANDLER[i];
    /* SWI vector: handled in HLE; leave a harmless 'movs pc, lr' */
    *(u32 *)&g->bios[0x08] = 0xE1B0F00E;

    for (int i = 0; i < 16; i++) g->r[i] = 0;
    for (int i = 0; i < 6; i++) { g->bank_sp[i] = 0; g->bank_lr[i] = 0; g->bank_spsr[i] = 0; }
    g->bank_sp[0] = 0x03007F00;   /* usr/sys */
    g->bank_sp[2] = 0x03007FA0;   /* irq */
    g->bank_sp[3] = 0x03007FE0;   /* svc */
    g->cpsr = MODE_SYS;
    g->r[13] = 0x03007F00;
    g->r[15] = 0x08000000 + 8;
    g->spsr = 0;
    g->cycles = 0; g->next_event = 0;
    g->halted = 0; g->irq_line = 0; g->intrwait = 0;
    g->branched = 0; g->halt_cycles = 0;
    g->idle = 0; g->idle_n = 0; g->idle_pc = 0; g->idle_sig = 0; g->idle_stores = 0; g->idle_io = 0;
    g->store_seq = 0; g->io_seq = 0;
    g->vcount = 0; g->line_cycle = 0;
    g->keys = 0; g->frame_ready = 0;
    g->flash_state = 0; g->flash_bank = 0; g->flash_id_mode = 0; g->flash_erase = 0; g->flash_dirty = 0;
    IO16(g, IO_KEYINPUT) = 0x03FF;
    IO16(g, IO_BG2PA) = 0x100; IO16(g, IO_BG2PA + 6) = 0x100;
    IO16(g, IO_BG3PA) = 0x100; IO16(g, IO_BG3PA + 6) = 0x100;
    g->bg2x = g->bg2y = g->bg3x = g->bg3y = 0;
}

static void line_start(GBA *g)
{
    g->line_cycle = 0;
    u16 ds = IO16(g, IO_DISPSTAT);
    if ((ds & 0x20) && g->vcount == (int)(ds >> 8)) irq_raise(g, 4);
    if (g->vcount == 0) {
        g->render_upto = 0;
        g->bg2x = (s32)((((u32)IO16(g, IO_BG2X + 2) & 0x0FFF) << 16) | IO16(g, IO_BG2X)); g->bg2x = (g->bg2x << 4) >> 4;
        g->bg2y = (s32)((((u32)IO16(g, IO_BG2X + 6) & 0x0FFF) << 16) | IO16(g, IO_BG2X + 4)); g->bg2y = (g->bg2y << 4) >> 4;
        g->bg3x = (s32)((((u32)IO16(g, IO_BG3X + 2) & 0x0FFF) << 16) | IO16(g, IO_BG3X)); g->bg3x = (g->bg3x << 4) >> 4;
        g->bg3y = (s32)((((u32)IO16(g, IO_BG3X + 6) & 0x0FFF) << 16) | IO16(g, IO_BG3X + 4)); g->bg3y = (g->bg3y << 4) >> 4;
    }
}

void gba_run_frame(GBA *g)
{
    sound_hle_scan(g);
    g->frame_ready = 0;
    while (!g->frame_ready) {
        u64 line_base = g->cycles - g->line_cycle;
        u64 phase_end = line_base + (g->line_cycle < 960 ? 960 : 1232);
        u64 until = phase_end, tn = timers_next_event(g);
        if (tn < until) until = tn;

        cpu_run(g, until);
        timers_sync(g);
        g->line_cycle = (u32)(g->cycles - line_base);

        if (g->line_cycle >= 1232) {
            g->line_cycle -= 1232;
            g->vcount++;
            if (g->vcount == 160) {
                ppu_sync(g);                              /* draw whatever is still pending */
                irq_raise(g, 0);
                if (IO16(g, IO_DISPSTAT) & 8) irq_raise(g, 1);
                dma_trigger(g, 1);
                g->frame_ready = 1;
            }
            if (g->vcount >= 228) g->vcount = 0;
            line_start(g);
        } else if (g->line_cycle >= 960 && !(g->io[0x1FF] & 1)) {
            g->io[0x1FF] |= 1;                       /* hblank entered this line */
            if (g->vcount < 160) dma_trigger(g, 2);      /* the line itself is drawn lazily */
            if (IO16(g, IO_DISPSTAT) & 0x10) irq_raise(g, 2);
        }
        if (g->line_cycle < 960) g->io[0x1FF] &= (u8)~1;
    }
}
