/* ppu.c - scanline renderer: tiled/affine/bitmap backgrounds, sprites, windows, blending.
 * Fast path: when no window and no blending apply to a line, layers are painted
 * back-to-front; otherwise the general per-pixel compositor runs. */
#include "gba.h"

#define IO16(g, off) (*(u16 *)&(g)->io[(off)])
#define TRANSP 0x8000u            /* marker in layer buffers (colours use only 15 bits) */

static const u8 OBJ_W[3][4] = { {8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32} };
static const u8 OBJ_H[3][4] = { {8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64} };

static inline u16 pal16(GBA *g, u32 idx) { return *(u16 *)&g->pal[idx * 2] & 0x7FFF; }

/* ------------------------------------------------------------ text BGs --- */

static void render_text_bg(GBA *g, int bg, int y, u16 *line)
{
    u16 cnt = IO16(g, IO_BG0CNT + bg * 2);
    u32 hofs = IO16(g, IO_BG0HOFS + bg * 4) & 0x1FF, vofs = IO16(g, IO_BG0HOFS + bg * 4 + 2) & 0x1FF;
    u32 cbase = ((cnt >> 2) & 3) * 0x4000, sbase = ((cnt >> 8) & 31) * 0x800;
    int bpp8 = (cnt >> 7) & 1, size = (cnt >> 14) & 3;
    u32 wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    u32 sy = (y + vofs) & hmask;
    u32 ty = sy >> 3, py = sy & 7;
    const u16 *palw = (const u16 *)g->pal;
    const u8 *vram = g->vram;
    u32 sx = hofs & wmask;
    int x = 0;
    while (x < GBA_W) {
        u32 tx = sx >> 3;
        u32 blk = 0;
        if (size == 1) blk = (tx >> 5) & 1;
        else if (size == 2) blk = (ty >> 5) & 1;
        else if (size == 3) blk = (((ty >> 5) & 1) << 1) | ((tx >> 5) & 1);
        u32 ent = *(const u16 *)&vram[(sbase + blk * 0x800 + (((ty & 31) << 5) + (tx & 31)) * 2) & 0xFFFF];
        u32 tile = ent & 0x3FF, hf = (ent >> 10) & 1, vf = (ent >> 11) & 1, pb = ent >> 12;
        u32 ry = vf ? 7 - py : py;
        u32 start = sx & 7;
        u32 n = 8 - start;
        if (x + (int)n > GBA_W) n = (u32)(GBA_W - x);
        u16 *dst = line + x;
        if (bpp8) {
            u32 a = cbase + tile * 64 + ry * 8;
            if (a >= 0x10000) { for (u32 i = 0; i < n; i++) dst[i] = TRANSP; }
            else {
                const u8 *r = &vram[a];
                for (u32 i = 0; i < n; i++) {
                    u32 px = start + i, c = r[hf ? 7 - px : px];
                    dst[i] = c ? (u16)(palw[c] & 0x7FFF) : (u16)TRANSP;
                }
            }
        } else {
            u32 a = cbase + tile * 32 + ry * 4;
            if (a >= 0x10000) { for (u32 i = 0; i < n; i++) dst[i] = TRANSP; }
            else {
                u32 w = *(const u32 *)&vram[a];
                const u16 *pp = palw + pb * 16;
                for (u32 i = 0; i < n; i++) {
                    u32 px = start + i, c = (w >> ((hf ? 7 - px : px) * 4)) & 15;
                    dst[i] = c ? (u16)(pp[c] & 0x7FFF) : (u16)TRANSP;
                }
            }
        }
        x += (int)n;
        sx = (sx + n) & wmask;
    }
}

/* Same as render_text_bg, but paints the opaque pixels straight onto 'out'
 * (back-to-front composition, no intermediate buffer), a tile row at a time.
 * wm != 0: only where (wm[x] & bit). */
static void draw_text_bg(GBA *g, int bg, int y, u16 *out, const u8 *wm, u8 bit)
{
    u16 cnt = IO16(g, IO_BG0CNT + bg * 2);
    u32 hofs = IO16(g, IO_BG0HOFS + bg * 4) & 0x1FF, vofs = IO16(g, IO_BG0HOFS + bg * 4 + 2) & 0x1FF;
    u32 cbase = ((cnt >> 2) & 3) * 0x4000, sbase = ((cnt >> 8) & 31) * 0x800;
    int bpp8 = (cnt >> 7) & 1, size = (cnt >> 14) & 3;
    u32 wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    u32 sy = (y + vofs) & hmask;
    u32 ty = sy >> 3, py = sy & 7;
    const u16 *palw = (const u16 *)g->pal;
    const u8 *vram = g->vram;
    u32 rowbase = sbase + ((size & 2) ? ((ty >> 5) & 1) * ((size == 3) ? 0x1000u : 0x800u) : 0) + (ty & 31) * 64;
    u32 sx = hofs & wmask;
    int x = 0;
    while (x < GBA_W) {
        u32 tx = sx >> 3;
        u32 ent = *(const u16 *)&vram[(rowbase + ((size & 1) ? ((tx >> 5) & 1) * 0x800 : 0) + (tx & 31) * 2) & 0xFFFF];
        u32 start = sx & 7, n = 8 - start;
        if (x + (int)n > GBA_W) n = (u32)(GBA_W - x);
        u32 ry = (ent & 0x800) ? 7 - py : py;
        u16 *dst = out + x - (int)start;                     /* dst[i] for tile pixel i */
        const u8 *wmt = wm ? wm + x - (int)start : 0;
        if (bpp8) {
            u32 a = cbase + (ent & 0x3FF) * 64 + ry * 8;
            if (a < 0x10000) {
                u32 lo = *(const u32 *)&vram[a], hi = *(const u32 *)&vram[a + 4];
                if (lo | hi) {
                    if (ent & 0x400) { u32 t = __builtin_bswap32(lo); lo = __builtin_bswap32(hi); hi = t; }
                    for (u32 i = start; i < start + n; i++) {
                        u32 c = (i < 4 ? lo >> (i * 8) : hi >> ((i - 4) * 8)) & 255;
                        if (c && (!wmt || (wmt[i] & bit))) dst[i] = palw[c] & 0x7FFF;
                    }
                }
            }
        } else {
            u32 a = cbase + (ent & 0x3FF) * 32 + ry * 4;
            if (a < 0x10000) {
                u32 w = *(const u32 *)&vram[a];
                if (w) {
                    if (ent & 0x400) {                         /* h-flip: reverse the 8 nibbles */
                        w = ((w >> 4) & 0x0F0F0F0Fu) | ((w & 0x0F0F0F0Fu) << 4);
                        w = __builtin_bswap32(w);
                    }
                    const u16 *pp = palw + (ent >> 12) * 16;
                    if (n == 8 && !wmt) {
                        u32 c;
                        c = w & 15;         if (c) dst[0] = pp[c] & 0x7FFF;
                        c = (w >> 4) & 15;  if (c) dst[1] = pp[c] & 0x7FFF;
                        c = (w >> 8) & 15;  if (c) dst[2] = pp[c] & 0x7FFF;
                        c = (w >> 12) & 15; if (c) dst[3] = pp[c] & 0x7FFF;
                        c = (w >> 16) & 15; if (c) dst[4] = pp[c] & 0x7FFF;
                        c = (w >> 20) & 15; if (c) dst[5] = pp[c] & 0x7FFF;
                        c = (w >> 24) & 15; if (c) dst[6] = pp[c] & 0x7FFF;
                        c = w >> 28;        if (c) dst[7] = pp[c] & 0x7FFF;
                    } else {
                        for (u32 i = start; i < start + n; i++) {
                            u32 c = (w >> (i * 4)) & 15;
                            if (c && (!wmt || (wmt[i] & bit))) dst[i] = pp[c] & 0x7FFF;
                        }
                    }
                }
            }
        }
        x += (int)n;
        sx = (sx + n) & wmask;
    }
}

/* ----------------------------------------------------------- affine BGs --- */

/* out != 0: paint opaque pixels onto out (wm/bit as in draw_text_bg);
 * out == 0: write every pixel (TRANSP where empty) to line. */
static void affine_bg(GBA *g, int bg, u16 *line, u16 *out, const u8 *wm, u8 bit, s32 *refx, s32 *refy)
{
    u16 cnt = IO16(g, IO_BG0CNT + bg * 2);
    u32 pbase = (bg == 2) ? IO_BG2PA : IO_BG3PA;
    s32 pa = (s16)IO16(g, pbase), pc = (s16)IO16(g, pbase + 4);
    u32 cbase = ((cnt >> 2) & 3) * 0x4000, sbase = ((cnt >> 8) & 31) * 0x800;
    int wrap = (cnt >> 13) & 1, size = (cnt >> 14) & 3;
    s32 dim = 128 << size;
    u32 tshift = 4 + (u32)size;                 /* map is dim/8 tiles wide */
    const u8 *vram = g->vram;
    const u16 *palw = (const u16 *)g->pal;
    s32 ax = *refx, ay = *refy;                 /* incremental: (ref + pa*x) without multiplies */
    u32 lastkey = 0xFFFFFFFFu, tbase = 0;
    for (int x = 0; x < GBA_W; x++, ax += pa, ay += pc) {
        s32 tx = ax >> 8, ty = ay >> 8;
        if (wrap) { tx &= dim - 1; ty &= dim - 1; }
        else if ((u32)tx >= (u32)dim || (u32)ty >= (u32)dim) { if (!out) line[x] = TRANSP; continue; }
        u32 key = ((u32)(ty >> 3) << tshift) + (u32)(tx >> 3);
        if (key != lastkey) { lastkey = key; tbase = cbase + (u32)vram[(sbase + key) & 0xFFFF] * 64; }
        u32 a = tbase + (u32)(ty & 7) * 8 + (u32)(tx & 7);
        u32 c = a < 0x10000 ? vram[a] : 0;
        if (out) { if (c && (!wm || (wm[x] & bit))) out[x] = palw[c] & 0x7FFF; }
        else line[x] = c ? (u16)(palw[c] & 0x7FFF) : (u16)TRANSP;
    }
}

static void render_affine_bg(GBA *g, int bg, int y, u16 *line, s32 *refx, s32 *refy)
{
    (void)y;
    affine_bg(g, bg, line, 0, 0, 0, refx, refy);
}

static void render_bitmap_bg(GBA *g, int mode, int y, u16 *line, s32 *refx, s32 *refy)
{
    u16 dc = IO16(g, IO_DISPCNT);
    u32 frame = (dc & 0x10) ? 0xA000 : 0;
    s32 pa = (s16)IO16(g, IO_BG2PA), pc = (s16)IO16(g, IO_BG2PA + 4);
    int w = mode == 5 ? 160 : 240, h = mode == 5 ? 128 : 160;
    s32 ax = *refx, ay = *refy;
    (void)y;
    for (int x = 0; x < GBA_W; x++, ax += pa, ay += pc) {
        s32 tx = ax >> 8, ty = ay >> 8;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) { line[x] = TRANSP; continue; }
        if (mode == 3) line[x] = *(u16 *)&g->vram[((u32)ty * 240 + (u32)tx) * 2] & 0x7FFF;
        else if (mode == 4) {
            u32 c = g->vram[frame + (u32)ty * 240 + (u32)tx];
            line[x] = c ? pal16(g, c) : TRANSP;
        } else line[x] = *(u16 *)&g->vram[frame + ((u32)ty * 160 + (u32)tx) * 2] & 0x7FFF;
    }
}

/* --------------------------------------------------------------- sprites --- */

/* returns bit0 = any sprite pixel drawn, bit1 = any semi-transparent pixel */
static int render_sprites(GBA *g, int y, u16 *objc, u8 *objp, u8 *objsemi, u8 *objwin)
{
    u16 dc = IO16(g, IO_DISPCNT);
    int one_d = (dc >> 6) & 1, flags = 0;
    u32 obase = 0x10000;
    int mode = dc & 7;
    for (int x = 0; x < GBA_W; x++) { objc[x] = TRANSP; objp[x] = 7; objsemi[x] = 0; objwin[x] = 0; }

    for (int i = 127; i >= 0; i--) {
        u16 a0 = *(u16 *)&g->oam[i * 8];
        int affine = (a0 >> 8) & 1, dbl = (a0 >> 9) & 1;
        if (!affine && dbl) continue;                       /* disabled */
        u16 a1 = *(u16 *)&g->oam[i * 8 + 2];
        int shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int w = OBJ_W[shape][size], h = OBJ_H[shape][size];
        int bw = (affine && dbl) ? w * 2 : w, bh = (affine && dbl) ? h * 2 : h;
        int oy = a0 & 255;
        int ry = (y - oy) & 255;
        if (ry >= bh) continue;
        u16 a2 = *(u16 *)&g->oam[i * 8 + 4];
        int omode = (a0 >> 10) & 3, ox = a1 & 511;
        if (ox >= 256) ox -= 512;
        int bpp8 = (a0 >> 13) & 1, prio = (a2 >> 10) & 3, pbank = a2 >> 12;
        u32 tile = a2 & 0x3FF;
        int tw = w >> 3;
        u32 tstep = one_d ? (u32)(bpp8 ? tw * 2 : tw) : 32;
        if (mode >= 3 && tile < 512) continue;

        s32 pa = 0, pb = 0, pc = 0, pd = 0;
        if (affine) {
            u32 gidx = ((a1 >> 9) & 31) * 32;
            pa = (s16)*(u16 *)&g->oam[gidx + 6]; pb = (s16)*(u16 *)&g->oam[gidx + 14];
            pc = (s16)*(u16 *)&g->oam[gidx + 22]; pd = (s16)*(u16 *)&g->oam[gidx + 30];
        }
        for (int sx = 0; sx < bw; sx++) {
            int x = ox + sx;
            if (x < 0 || x >= GBA_W) continue;
            int tx, ty;
            if (affine) {
                int cx = sx - bw / 2, cy = ry - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - sx : sx;
                ty = (a1 & 0x2000) ? h - 1 - ry : ry;
            }
            u32 t = tile + (u32)(ty >> 3) * tstep + (u32)(tx >> 3) * (bpp8 ? 2u : 1u);
            u32 addr = obase + (t & 0x3FF) * 32, c;
            if (bpp8) { addr += (u32)(ty & 7) * 8 + (u32)(tx & 7); c = g->vram[addr % 0x18000]; }
            else {
                addr += (u32)(ty & 7) * 4 + (u32)((tx & 7) >> 1);
                c = g->vram[addr % 0x18000]; c = (tx & 1) ? c >> 4 : c & 15;
            }
            if (!c) continue;
            if (omode == 2) { objwin[x] = 1; continue; }
            if (prio <= objp[x]) {
                objc[x] = pal16(g, 256 + (bpp8 ? c : pbank * 16 + c));
                objp[x] = (u8)prio;
                objsemi[x] = (omode == 1);
                flags |= 1;
                if (omode == 1) flags |= 2;
            }
        }
    }
    return flags;
}

/* ------------------------------------------------------------ composition --- */

static inline u16 blend_alpha(u16 a, u16 b, u32 eva, u32 evb)
{
    u32 r = ((a & 31) * eva + (b & 31) * evb) >> 4, gg = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    u32 bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (gg > 31) gg = 31;
    if (bl > 31) bl = 31;
    return (u16)(r | (gg << 5) | (bl << 10));
}

static inline u16 blend_bright(u16 c, u32 evy, int up)
{
    u32 r = c & 31, gg = (c >> 5) & 31, b = (c >> 10) & 31;
    if (up) { r += ((31 - r) * evy) >> 4; gg += ((31 - gg) * evy) >> 4; b += ((31 - b) * evy) >> 4; }
    else    { r -= (r * evy) >> 4; gg -= (gg * evy) >> 4; b -= (b * evy) >> 4; }
    return (u16)(r | (gg << 5) | (b << 10));
}

/* Lazy rendering: lines are drawn in batches. Line y is due once its HBlank has
 * started; any display-affecting write first draws everything that is due, so the
 * output is identical to drawing each line at its HBlank. */
void ppu_sync(GBA *g)
{
    if (g->skip_render) return;
    int target = g->vcount < 160 ? g->vcount + (g->line_cycle >= 960 ? 1 : 0) : 160;
    if (target > 160) target = 160;
    while (g->render_upto < target) { ppu_render_line(g, g->render_upto); g->render_upto++; }
}

static void fill_span(u8 *wm, int l, int r, u8 m)
{
    for (int x = 0; x < GBA_W; x++) {
        int hit = (l <= r) ? (x >= l && x < r) : (x >= l || x < r);
        if (r > 240) hit = x >= l;
        if (hit) wm[x] = m;
    }
}

void ppu_render_line(GBA *g, int y)
{
    u16 dc = IO16(g, IO_DISPCNT);
    u16 *out = &g->frame[y * GBA_W];
    if (dc & 0x80) { for (int x = 0; x < GBA_W; x++) out[x] = 0x7FFF; return; }

    int mode = dc & 7;
    static u16 bgl[4][GBA_W];
    static u16 objc[GBA_W];
    static u8 objp[GBA_W], objsemi[GBA_W], objwin[GBA_W];
    u32 bgprio[4];
    int bgon[4] = { 0, 0, 0, 0 };
    int bgkind[4] = { 0, 0, 0, 0 };          /* 1 text, 2 affine, 3 bitmap */

    for (int b = 0; b < 4; b++) bgprio[b] = IO16(g, IO_BG0CNT + b * 2) & 3;
    switch (mode) {
    case 0: for (int b = 0; b < 4; b++) bgkind[b] = 1; break;
    case 1: bgkind[0] = bgkind[1] = 1; bgkind[2] = 2; break;
    case 2: bgkind[2] = bgkind[3] = 2; break;
    case 3: case 4: case 5: bgkind[2] = 3; break;
    default: break;
    }
    for (int b = 0; b < 4; b++) if (bgkind[b] && (dc & (0x100 << b))) bgon[b] = 1;

    int objon = (dc & 0x1000) != 0, objflags = 0;
    if (objon || (dc & 0x8000)) objflags = render_sprites(g, y, objc, objp, objsemi, objwin);
    else for (int x = 0; x < GBA_W; x++) { objc[x] = TRANSP; objp[x] = 7; objsemi[x] = 0; objwin[x] = 0; }
    if (!objon) objflags = 0;

    /* affine reference points used by this line; advanced for the next one */
    s32 r2x = g->bg2x, r2y = g->bg2y, r3x = g->bg3x, r3y = g->bg3y;
    g->bg2x += (s16)IO16(g, IO_BG2PA + 2); g->bg2y += (s16)IO16(g, IO_BG2PA + 6);
    g->bg3x += (s16)IO16(g, IO_BG3PA + 2); g->bg3y += (s16)IO16(g, IO_BG3PA + 6);

    u16 bldcnt = IO16(g, IO_BLDCNT), bldalpha = IO16(g, IO_BLDALPHA);
    int effect = (bldcnt >> 6) & 3;
    u16 backdrop = pal16(g, 0);
    int anywin = (dc & 0xE000) != 0;

    /* per-pixel window mask (bits 0-3 BGs, 4 OBJ, 5 effects), built from spans */
    static u8 wm[GBA_W];
    if (anywin) {
        u16 w0h = IO16(g, IO_WIN0H), w1h = IO16(g, IO_WIN0H + 2), w0v = IO16(g, IO_WIN0H + 4), w1v = IO16(g, IO_WIN0H + 6);
        u16 winin = IO16(g, IO_WININ), winout = IO16(g, IO_WININ + 2);
        int win0 = (dc & 0x2000) != 0, win1 = (dc & 0x4000) != 0, winobj = (dc & 0x8000) != 0;
        int in0v = 0, in1v = 0;
        if (win0) { int t = w0v >> 8, b = w0v & 255; in0v = (t <= b) ? (y >= t && y < b) : (y >= t || y < b); }
        if (win1) { int t = w1v >> 8, b = w1v & 255; in1v = (t <= b) ? (y >= t && y < b) : (y >= t || y < b); }
        u8 outm = (u8)(winout & 0x3F);
        for (int x = 0; x < GBA_W; x++) wm[x] = outm;
        if (winobj) { u8 om = (u8)((winout >> 8) & 0x3F); for (int x = 0; x < GBA_W; x++) if (objwin[x]) wm[x] = om; }
        if (win1 && in1v) fill_span(wm, w1h >> 8, w1h & 255, (u8)((winin >> 8) & 0x3F));
        if (win0 && in0v) fill_span(wm, w0h >> 8, w0h & 255, (u8)(winin & 0x3F));
        /* the same mask across the whole line (the usual case): fold it into the
         * layer enables and drop the per-pixel window tests */
        u8 m = wm[0];
        int x = 1;
        while (x < GBA_W && wm[x] == m) x++;
        if (x == GBA_W) {
            for (int b = 0; b < 4; b++) if (!(m & (1u << b))) bgon[b] = 0;
            if (!(m & 0x10)) { objflags = 0; for (int i = 0; i < GBA_W; i++) objc[i] = TRANSP; }
            if (!(m & 0x20)) { effect = 0; objflags &= ~2; }
            anywin = 0;
        }
    }

    /* ---- fast path: no blending on this line (windows allowed) ----
     * layers are painted straight onto the output, back to front */
    if (!(effect != 0 && (bldcnt & 0x3F) != 0) && !(objflags & 2)) {
        { u32 bd2 = (u32)backdrop | ((u32)backdrop << 16); u32 *o32 = (u32 *)out; for (int x = 0; x < GBA_W / 2; x++) o32[x] = bd2; }
        const u8 *wmp = anywin ? wm : 0;
        for (int p = 3; p >= 0; p--) {
            for (int b = 3; b >= 0; b--) {
                if (!bgon[b] || bgprio[b] != (u32)p) continue;
                u8 bit = (u8)(1u << b);
                if (bgkind[b] == 1) { draw_text_bg(g, b, y, out, wmp, bit); continue; }
                if (bgkind[b] == 2) {
                    if (b == 2) affine_bg(g, 2, 0, out, wmp, bit, &r2x, &r2y);
                    else affine_bg(g, 3, 0, out, wmp, bit, &r3x, &r3y);
                    continue;
                }
                render_bitmap_bg(g, mode, y, bgl[b], &r2x, &r2y);
                const u16 *s = bgl[b];
                if (!anywin) { for (int x = 0; x < GBA_W; x++) { u16 c = s[x]; if (c != TRANSP) out[x] = c; } }
                else { for (int x = 0; x < GBA_W; x++) { u16 c = s[x]; if (c != TRANSP && (wm[x] & bit)) out[x] = c; } }
            }
            if (objflags & 1) {
                if (!anywin) { for (int x = 0; x < GBA_W; x++) if (objc[x] != TRANSP && objp[x] == (u8)p) out[x] = objc[x]; }
                else { for (int x = 0; x < GBA_W; x++) if (objc[x] != TRANSP && objp[x] == (u8)p && (wm[x] & 0x10)) out[x] = objc[x]; }
            }
        }
        return;
    }

    /* ---- general path ---- */
    for (int b = 0; b < 4; b++) {
        if (!bgon[b]) continue;
        if (bgkind[b] == 1) render_text_bg(g, b, y, bgl[b]);
        else if (bgkind[b] == 2) render_affine_bg(g, b, y, bgl[b], b == 2 ? &r2x : &r3x, b == 2 ? &r2y : &r3y);
        else render_bitmap_bg(g, mode, y, bgl[b], &r2x, &r2y);
    }
    u32 eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = IO16(g, IO_BLDY) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;

    /* enabled BGs in compositing order: priority, then index */
    int order[4], nb = 0;
    for (u32 p = 0; p < 4; p++) for (int b = 0; b < 4; b++) if (bgon[b] && bgprio[b] == p) order[nb++] = b;

    for (int x = 0; x < GBA_W; x++) {
        u32 mask = anywin ? wm[x] : 0x3F;
        u16 c0 = backdrop, c1 = backdrop;
        int l0 = 5, l1 = 5, n = 0;
        int semi = 0;
        int objpend = (mask & 0x10) && objc[x] != TRANSP;   /* sprite sits above BGs of equal priority */
        u32 op = objp[x];
        for (int k = 0; k < nb && n < 2; k++) {
            int b = order[k];
            if (objpend && op <= bgprio[b]) {
                objpend = 0;
                if (n == 0) { c0 = objc[x]; l0 = 4; semi = objsemi[x]; } else { c1 = objc[x]; l1 = 4; }
                if (++n >= 2) break;
            }
            if (!(mask & (1u << b)) || bgl[b][x] == TRANSP) continue;
            if (n == 0) { c0 = bgl[b][x]; l0 = b; } else { c1 = bgl[b][x]; l1 = b; }
            n++;
        }
        if (objpend && n < 2 && op < 4) {
            if (n == 0) { c0 = objc[x]; l0 = 4; semi = objsemi[x]; } else { c1 = objc[x]; l1 = 4; }
            n++;
        }
        u16 c = c0;
        if (mask & 0x20) {
            int t1 = (bldcnt >> l0) & 1, t2 = (bldcnt >> (8 + l1)) & 1;
            if (semi && t2) c = blend_alpha(c0, c1, eva, evb);
            else if (effect == 1 && t1 && t2) c = blend_alpha(c0, c1, eva, evb);
            else if (effect == 2 && t1) c = blend_bright(c0, evy, 1);
            else if (effect == 3 && t1) c = blend_bright(c0, evy, 0);
        }
        out[x] = c;
    }
}
