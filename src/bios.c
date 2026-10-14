/* bios.c - high-level emulation of the GBA BIOS software interrupts.
 * Original implementation based on the documented behaviour of each call. */
#include "gba.h"

#define IO16(g, off) (*(u16 *)&(g)->io[(off)])

static s16 sin_tab[256];
static int sin_ready;

/* 256-step sine table via the stable "magic circle" integer recurrence. */
static void sin_init(void)
{
    s32 x = 0, y = 32768 << 4;
    const s32 e = 1608;                   /* 2*sin(pi/256) in 16.16 */
    for (int i = 0; i < 256; i++) {
        s32 v = x >> 4;
        sin_tab[i] = (s16)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
        x += (s32)(((s64)e * y) >> 16);
        y -= (s32)(((s64)e * x) >> 16);
    }
    sin_ready = 1;
}

static s32 bsin(u32 a16) { return sin_tab[(a16 >> 8) & 255]; }
static s32 bcos(u32 a16) { return sin_tab[((a16 >> 8) + 64) & 255]; }

static u32 isqrt32(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

static s32 arctan_poly(s32 v)
{
    s32 a = -((v * v) >> 14), r = 0xA9;
    r = ((a * r) >> 14) + 0x390;
    r = ((a * r) >> 14) + 0x91C;
    r = ((a * r) >> 14) + 0xFB6;
    r = ((a * r) >> 14) + 0x16AA;
    r = ((a * r) >> 14) + 0x2081;
    r = ((a * r) >> 14) + 0x3651;
    r = ((a * r) >> 14) + 0xA2F9;
    return (v * r) >> 16;
}

/* ---------------------------------------------------------- decompression --- */
/* Output byte stream abstraction: WRAM variants write bytes; VRAM variants
 * can only write halfwords, so one byte is held back until its pair arrives. */

typedef struct { GBA *g; u32 dst, out; int vram, have; u32 pending; } Out;

static void out_put(Out *o, u32 b)
{
    if (!o->vram) { mem_write8(o->g, o->dst + o->out, b); o->out++; return; }
    if (!o->have) { o->pending = b; o->have = 1; o->out++; }
    else { mem_write16(o->g, o->dst + o->out - 1, o->pending | (b << 8)); o->have = 0; o->out++; }
}

static u32 out_get(Out *o, u32 idx)
{
    if (o->vram && o->have && idx == o->out - 1) return o->pending;
    return mem_read8(o->g, o->dst + idx);
}

static void out_flush(Out *o)
{
    if (o->vram && o->have) { mem_write16(o->g, o->dst + o->out - 1, o->pending); o->have = 0; }
}

static void lz77(GBA *g, u32 src, u32 dst, int vram)
{
    Out o = { g, dst, 0, vram, 0, 0 };
    u32 size = mem_read32(g, src) >> 8;
    src += 4;
    while (o.out < size) {
        u32 flags = mem_read8(g, src++);
        for (int i = 0; i < 8 && o.out < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 b0 = mem_read8(g, src++), b1 = mem_read8(g, src++);
                u32 len = (b0 >> 4) + 3, disp = (((b0 & 15) << 8) | b1) + 1;
                for (u32 k = 0; k < len && o.out < size; k++) out_put(&o, out_get(&o, o.out - disp));
            } else out_put(&o, mem_read8(g, src++));
        }
    }
    out_flush(&o);
}

static void rl(GBA *g, u32 src, u32 dst, int vram)
{
    Out o = { g, dst, 0, vram, 0, 0 };
    u32 size = mem_read32(g, src) >> 8;
    src += 4;
    while (o.out < size) {
        u32 f = mem_read8(g, src++);
        if (f & 0x80) {
            u32 len = (f & 0x7F) + 3, b = mem_read8(g, src++);
            for (u32 k = 0; k < len && o.out < size; k++) out_put(&o, b);
        } else {
            u32 len = (f & 0x7F) + 1;
            for (u32 k = 0; k < len && o.out < size; k++) out_put(&o, mem_read8(g, src++));
        }
    }
    out_flush(&o);
}

static void huff(GBA *g, u32 src, u32 dst)
{
    u32 hdr = mem_read32(g, src), size = hdr >> 8, dbits = hdr & 15;
    u32 tree = src + 4, root = tree + 1;
    u32 p = tree + (mem_read8(g, tree) + 1) * 2;
    u32 node = root, word = 0, wbits = 0, out = 0;
    if (dbits == 0) return;
    while (out < size) {
        u32 w = mem_read32(g, p);
        p += 4;
        for (int i = 31; i >= 0 && out < size; i--) {
            u32 bit = (w >> i) & 1, nd = mem_read8(g, node);
            u32 nxt = (node & ~1u) + (((nd & 0x3F) + 1) * 2) + bit;
            if ((nd >> (7 - bit)) & 1) {
                word |= (mem_read8(g, nxt) & ((1u << dbits) - 1)) << wbits;
                wbits += dbits;
                if (wbits >= 32) { mem_write32(g, dst + out, word); out += 4; word = 0; wbits = 0; }
                node = root;
            } else node = nxt;
        }
    }
}

/* ------------------------------------------------------------------ SWI --- */

int bios_swi(GBA *g, u32 num)
{
    u32 *r = g->r;
    if (!sin_ready) sin_init();
    switch (num) {
    case 0x00:                                           /* SoftReset */
        for (u32 i = 0x7E00; i < 0x8000; i++) g->iwram[i] = 0;
        set_cpsr(g, MODE_SYS);
        g->r[13] = 0x03007F00; g->r[15] = 0x08000000 + 8;
        return 0;
    case 0x01: {                                         /* RegisterRamReset */
        u32 f = r[0];
        if (f & 1) for (u32 i = 0; i < sizeof g->ewram; i++) g->ewram[i] = 0;
        if (f & 2) for (u32 i = 0; i < 0x7E00; i++) g->iwram[i] = 0;
        if (f & 4) for (u32 i = 0; i < sizeof g->pal; i++) g->pal[i] = 0;
        if (f & 8) for (u32 i = 0; i < sizeof g->vram; i++) g->vram[i] = 0;
        if (f & 16) for (u32 i = 0; i < sizeof g->oam; i++) g->oam[i] = 0;
        return 0;
    }
    case 0x02: case 0x03: g->halted = 1; irq_update(g); return 0;
    case 0x04: case 0x05: {                              /* IntrWait / VBlankIntrWait */
        u32 clear = (num == 5) ? 1 : r[0], mask = (num == 5) ? 1 : r[1];
        u16 *flags = (u16 *)&g->iwram[0x7FF8];
        if (!g->intrwait) {
            if (clear) *flags &= (u16)~mask;
            g->intrwait = 1;
            IO16(g, IO_IME) = 1;
            irq_update(g);
        }
        if (*flags & mask) { *flags &= (u16)~mask; g->intrwait = 0; return 0; }
        g->halted = 1;
        return 1;
    }
    case 0x06: case 0x07: {                              /* Div / DivArm */
        s32 n = (s32)(num == 6 ? r[0] : r[1]), d = (s32)(num == 6 ? r[1] : r[0]);
        if (d == 0) { r[0] = n < 0 ? (u32)-1 : 1; r[1] = (u32)n; r[3] = 1; return 0; }
        int ov = (n == (s32)0x80000000 && d == -1);
        s32 q = ov ? (s32)0x80000000 : n / d, m = ov ? 0 : n % d;
        r[0] = (u32)q; r[1] = (u32)m; r[3] = (u32)(q < 0 ? -q : q);
        return 0;
    }
    case 0x08: r[0] = isqrt32(r[0]); return 0;
    case 0x09: r[0] = (u32)arctan_poly((s32)r[0]) & 0xFFFF; return 0;
    case 0x0A: {                                         /* ArcTan2 (approximation) */
        s32 x = (s32)r[0], y = (s32)r[1];
        u32 res;
        if (y == 0) res = (x < 0) ? 0x8000 : 0;
        else if (x == 0) res = (y < 0) ? 0xC000 : 0x4000;
        else {
            s32 ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a;
            if (ax >= ay) a = arctan_poly((s32)(((s64)ay << 14) / ax));
            else a = 0x4000 - arctan_poly((s32)(((s64)ax << 14) / ay));
            if (x < 0 && y >= 0) a = 0x8000 - a;
            else if (x < 0 && y < 0) a = 0x8000 + a;
            else if (x >= 0 && y < 0) a = 0x10000 - a;
            res = (u32)a & 0xFFFF;
        }
        r[0] = res;
        return 0;
    }
    case 0x0B: {                                         /* CpuSet */
        u32 src = r[0], dst = r[1], ctl = r[2], n = ctl & 0x1FFFFF;
        int fill = (ctl >> 24) & 1, w32 = (ctl >> 26) & 1;
        if (w32) {
            src &= ~3u; dst &= ~3u;
            u32 v = mem_read32(g, src);
            for (u32 i = 0; i < n; i++) { if (!fill) v = mem_read32(g, src + i * 4); mem_write32(g, dst + i * 4, v); }
        } else {
            src &= ~1u; dst &= ~1u;
            u32 v = mem_read16(g, src);
            for (u32 i = 0; i < n; i++) { if (!fill) v = mem_read16(g, src + i * 2); mem_write16(g, dst + i * 2, v); }
        }
        return 0;
    }
    case 0x0C: {                                         /* CpuFastSet */
        u32 src = r[0] & ~3u, dst = r[1] & ~3u, ctl = r[2], n = (ctl & 0x1FFFFF);
        n = (n + 7) & ~7u;
        int fill = (ctl >> 24) & 1;
        u32 v = mem_read32(g, src);
        for (u32 i = 0; i < n; i++) { if (!fill) v = mem_read32(g, src + i * 4); mem_write32(g, dst + i * 4, v); }
        return 0;
    }
    case 0x0D: r[0] = 0xBAAE187F; return 0;             /* GetBiosChecksum */
    case 0x0E: {                                         /* BgAffineSet */
        u32 src = r[0], dst = r[1], n = r[2];
        for (u32 i = 0; i < n; i++, src += 20, dst += 16) {
            s32 ox = (s32)mem_read32(g, src), oy = (s32)mem_read32(g, src + 4);
            s32 cx = (s16)mem_read16(g, src + 8), cy = (s16)mem_read16(g, src + 10);
            s32 sx = (s16)mem_read16(g, src + 12), sy = (s16)mem_read16(g, src + 14);
            u32 th = mem_read16(g, src + 16);
            s32 c = bcos(th), s = bsin(th);
            s32 a = (sx * c) >> 14, b = -((sx * s) >> 14), cc = (sy * s) >> 14, d = (sy * c) >> 14;
            mem_write16(g, dst, (u32)a); mem_write16(g, dst + 2, (u32)b);
            mem_write16(g, dst + 4, (u32)cc); mem_write16(g, dst + 6, (u32)d);
            mem_write32(g, dst + 8, (u32)(ox - (a * cx + b * cy)));
            mem_write32(g, dst + 12, (u32)(oy - (cc * cx + d * cy)));
        }
        return 0;
    }
    case 0x0F: {                                         /* ObjAffineSet */
        u32 src = r[0], dst = r[1], n = r[2], step = r[3];
        for (u32 i = 0; i < n; i++, src += 8, dst += step * 4) {
            s32 sx = (s16)mem_read16(g, src), sy = (s16)mem_read16(g, src + 2);
            u32 th = mem_read16(g, src + 4);
            s32 c = bcos(th), s = bsin(th);
            mem_write16(g, dst, (u32)((sx * c) >> 14));
            mem_write16(g, dst + step, (u32)(-((sx * s) >> 14)));
            mem_write16(g, dst + step * 2, (u32)((sy * s) >> 14));
            mem_write16(g, dst + step * 3, (u32)((sy * c) >> 14));
        }
        return 0;
    }
    case 0x11: lz77(g, r[0], r[1], 0); return 0;
    case 0x12: lz77(g, r[0], r[1], 1); return 0;
    case 0x13: huff(g, r[0], r[1]); return 0;
    case 0x14: rl(g, r[0], r[1], 0); return 0;
    case 0x15: rl(g, r[0], r[1], 1); return 0;
    case 0x1F: r[0] = mem_read32(g, r[0] + 4); return 0;   /* MidiKey2Freq (rough) */
    default: return 0;                                   /* sound driver, multiboot, ...: no-ops */
    }
}
