/*
 * Lines - GBA emulator front end for the iPod nano 7 (NanoApps raw surface app).
 *
 *  - ROM pages (32 KiB files p000.bin ...) are cached in RAM with a clock eviction
 *  - landscape output: 240x160 scaled 1.5x to 360x240 and rotated onto the 240x432 fb
 *  - multitouch controls, frame pacing with automatic render-skipping
 *  - the save (flash / SRAM) is flushed to disk a couple of seconds after it changes
 *  - memory release: the OS gives a raw surface app no exit callback (the resident only stops calling
 *    hb_raw_frame), so the emulator block + ROM page slots (~3 MB) would stay allocated after leaving.
 *    Holding EXIT (~0.6 s; landscape: right margin under SV, portrait: right of START) or 10 min without a
 *    touch saves the save file + a save state, frees every block and shows "MEMORY RELEASED / YOU MAY LEAVE";
 *    a tap (or the next launch) reloads from the save state. Leaving without it keeps the memory until the
 *    next launch of the app. Trace: GBAFREE (bytes, heap free after), GBAFRWHY (1 button / 2 idle, ms),
 *    GBARELOD (ms to reload from disk after a release, resumed).
 *
 *  - ROM selection: every sub-folder of /Apps/Data/GBA that holds meta.bin + p000.bin ... (made by
 *    prepare_rom.py) is a ROM. Its save.sav and
 *    state.bin live in the same folder, so saves never collide. /Apps/Data/GBA/last.txt = last ROM.
 *    Launch: >= 2 ROMs -> menu (last ROM highlighted); 1 ROM -> straight into it; after EXIT / 10 min idle
 *    the tap (or next launch) resumes the last ROM, not the menu. Holding ROMS (portrait: status bar,
 *    landscape: left margin) saves, frees every block (same path as EXIT) and returns to the menu.
 *    Trace: GBAROMS (count, last index+1), GBAPICK (index, size), GBAFRWHY reason 3 = switch to menu.
 *
 *  - UNTETHER (fork SDK) memory model: ONE hb_os_alloc block = [32 B header][GBA struct][ROM page cache][zero page],
 *    registered as the resident's aux block (HB_APP_AUX_BLOCK), which the resident frees at the next app launch.
 *    Nothing survives a relaunch any more (the resident frees the block before payload_entry), so the old
 *    keep.bin block reclaim is gone; keep.bin is only a marker now (released / was running -> resume the last ROM).
 *    The cache is sized at run time from hb_os_heap_free()/hb_os_heap_largest(): as many 32 KiB slots as the ROM
 *    needs (<= MAX_SLOTS), leaving >= HEAP_RESERVE free for the OS; the allocator's real granularity is measured
 *    (free before/after) and the request shrinks when it rounds up (power of two). Fallback: 64 / 32 / 16 slots.
 *    Replacement = LRU on per-frame stamps (rom_ref scanned once per emulated frame). hot.bin (per ROM) lists the
 *    resident pages by recency at every state save and is preloaded behind the LOADING screen after a reload;
 *    a ROM that fits whole in the cache is preloaded entirely.
 *    Trace: GBAHEAP (free, largest before), GBASLOTS (slots, free after), GBABLK (bytes, consumed), GBAHOT (pages).
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"
#include "hb_heap.h"
#include "gba.h"

#define GBA_ROOT   "/Apps/Data/GBA"
#define LAST_PATH  GBA_ROOT "/last.txt"
#define ROM_NAME   32                   /* folder name incl. NUL: at most 31 chars */
#define MAX_ROMS   32
#define ROM_MAX_BYTES 0x2000000u        /* 32 MiB = the GBA ROM address space (ROM_MAX_PAGES pages) */
#define MAX_SLOTS  256                  /* cache slots (32 KiB each) the book-keeping can hold: 8 MiB of ROM pages */
#define MIN_SLOTS  16                   /* below this the game thrashes: "OUT OF MEMORY" */
#define FALLBACK_SLOTS 64               /* what the pre-untether app always used: tried when the sized request fails */
#ifndef HEAP_RESERVE
#define HEAP_RESERVE (1536u * 1024u)    /* heap bytes that must stay free for the resident / OS after our block */
#endif
#define FRAME_US   16743u               /* 59.7275 Hz */

#define PORTRAIT (keep.k_landscape == 0)
#define LW (PORTRAIT ? 240 : 432)
#define LH (PORTRAIT ? 432 : 240)
#define PGY 20                          /* portrait: game image rows 20..179 */

#define BLACK 0x000000
#define FG    0xE8ECF4
#define DIM   0x7C869A
#define OKC   0x2FBF71
#define BAD   0xE63946
#define PBG   0x0E1118
#define BTN   0x2A3142
#define ACC   0xFF7A45

/* Book-keeping that must outlive a relaunch lives in .data (the resident reuses the loaded image of a re-opened app):
 * the screen orientation and the ROM being played. Every heap pointer is dead after a relaunch (the resident frees
 * the aux block before calling payload_entry), so hb_raw_init clears them: the cache/block fields below are only
 * valid inside one launch. A fresh image load (reboot / reinstall) brings magic back to 1. */
static struct {
    uint32_t magic;
    GBA *k_g;
    u8 *k_cache;                /* first ROM page slot (inside the same heap block as k_g) */
    u8 *k_zero;                 /* shared zero page for ROM addresses past the end of the ROM */
    int k_nslots, k_nused;
    uint32_t k_rom_size, k_save_type, k_npages, k_blk_bytes;
    int booted;
    int k_landscape;
    uint32_t released;          /* blocks freed by EXIT / idle / ROMS (reason): the next boot logs GBARELOD */
    char k_rom[ROM_NAME];       /* folder of the current ROM (resume after a relaunch keeps playing it) */
} keep = { 1u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0} };
static int slot_page[MAX_SLOTS];        /* ROM page held by each slot (.bss: re-zeroed every launch, like the pointers) */
static uint32_t slot_stamp[MAX_SLOTS];  /* LRU clock of the last emulated frame that read the slot's page */
static uint32_t lru_clock;

/* KEEP_PATH is a tiny marker file now (no block addresses): REL_MAGIC = memory was released by EXIT / idle / ROMS
 * (tag 2 = go to the menu), RUN_MAGIC = a game was running (it was left with Home: the resident has freed its block,
 * so the next launch reloads the last ROM from its save state instead of showing the menu). */
#define KEEP_PATH "/Apps/Data/GBA/keep.bin"
#define BLK_MAGIC 0x4C494E32u
#define REL_MAGIC 0x4C52454Cu
#define RUN_MAGIC 0x4C52554Eu
#define BLK_HDR   32
#ifndef IDLE_FREE_US
#define IDLE_FREE_US (10u * 60u * 1000000u) /* no touch this long while running -> save state + free */
#endif
typedef struct { uint32_t magic, tag, pad0, pad1; } keepfile_t;
static keepfile_t kf;

/* ---------------------------------------- portrait / landscape drawing --- */

static void m_pt(int lx, int ly, int *px, int *py)
{
    if (PORTRAIT) { *px = lx; *py = ly; }
    else          { *px = ly; *py = 431 - lx; }
}

static void to_logical(int tx, int ty, int *lx, int *ly)
{
    if (PORTRAIT) { *lx = tx; *ly = ty; }
    else          { *lx = 431 - ty; *ly = tx; }
}

static void frect(int x, int y, int w, int h, uint32_t c)
{
    int ax, ay, bx, by;
    if (w <= 0 || h <= 0) return;
    m_pt(x, y, &ax, &ay);
    m_pt(x + w - 1, y + h - 1, &bx, &by);
    int x0 = ax < bx ? ax : bx, y0 = ay < by ? ay : by;
    int x1 = ax < bx ? bx : ax, y1 = ay < by ? by : ay;
    hb_raw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, c);
}

static void rect_out(int x, int y, int w, int h, uint32_t c)
{
    frect(x, y, w, 1, c); frect(x, y + h - 1, w, 1, c);
    frect(x, y, 1, h, c); frect(x + w - 1, y, 1, h, c);
}

static const uint16_t FONT_AZ[26] = {
    0b010101111101101, 0b110101110101110, 0b011100100100011, 0b110101101101110,
    0b111100110100111, 0b111100110100100, 0b011100101101011, 0b101101111101101,
    0b111010010010111, 0b001001001101010, 0b101101110101101, 0b100100100100111,
    0b101111111101101, 0b110101101101101, 0b010101101101010, 0b110101110100100,
    0b010101101111011, 0b110101110101101, 0b011100010001110, 0b111010010010010,
    0b101101101101111, 0b101101101101010, 0b101101111111101, 0b101101010101101,
    0b101101010010010, 0b111001010100111,
};
static const uint16_t FONT_09[10] = {
    0b111101101101111, 0b010110010010111, 0b110001010100111, 0b110001110001110,
    0b101101111001001, 0b111100110001110, 0b011100111101111, 0b111001010010010,
    0b111101111101111, 0b111101111001110,
};

static uint16_t glyph_of(char c)
{
    if (c >= 'A' && c <= 'Z') return FONT_AZ[c - 'A'];
    if (c >= '0' && c <= '9') return FONT_09[c - '0'];
    if (c == '-') return 0b000000111000000;
    if (c == '/') return 0b001001010100100;
    if (c == '.') return 0b000000000000010;
    if (c == '_') return 0b000000000000111;
    return 0;
}

static void text(int x, int y, const char *s, int sc, uint32_t col)
{
    for (; *s; s++, x += 4 * sc) {
        uint16_t gl = glyph_of(*s);
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (gl & (1u << (14 - (r * 3 + c))))
                    frect(x + c * sc, y + r * sc, sc, sc, col);
    }
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void text_c(int cx, int y, const char *s, int sc, uint32_t col)
{
    text(cx - (slen(s) * 4 * sc - sc) / 2, y, s, sc, col);
}

static int utoa(uint32_t v, char *buf)
{
    char t[12];
    int n = 0, i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) buf[i++] = t[--n];
    buf[i] = 0;
    return i;
}

static void num(int x, int y, uint32_t v, int sc, uint32_t col)
{
    char b[12];
    utoa(v, b);
    text(x, y, b, sc, col);
}

/* ------------------------------------------------------------ state --- */

/* S_META (read meta.bin) -> S_ALLOC (size + allocate the block) -> S_CACHE (slots, preload) -> S_BOOT -> S_RUN */
enum { S_ALLOC, S_META, S_CACHE, S_BOOT, S_RESUME, S_RUN, S_ERROR, S_FREED, S_START, S_MENU };

/* ROM list / selection */
typedef struct { char dir[ROM_NAME]; uint32_t size; uint8_t stype, has_save, bad; } rom_t;
static rom_t roms[MAX_ROMS];
static int nroms, cur_idx = -1;
static int force_menu, rl_menu, fr_menu, err_armed;
static char last_name[ROM_NAME];
static char pbuf[ROM_NAME + 48];

static char *build_path(char *out, const char *dir, const char *leaf)
{
    int n = 0;
    const char *r = GBA_ROOT "/";
    while (*r) out[n++] = *r++;
    while (*dir) out[n++] = *dir++;
    while (*leaf) out[n++] = *leaf++;
    out[n] = 0;
    return out;
}
/* path of a file inside the current ROM's folder (single static buffer: use it at once) */
static const char *gp(const char *leaf) { return build_path(pbuf, keep.k_rom, leaf); }

/* memory release */
static int rel_reason, rl_from_rel, fr_wait_up, free_touch, free_prev, any_touch, menu_touch, menu_prev;
static uint32_t rel_freed, rel_heap1, free_since_us, last_touch_us, rl_t0;

#define KEEP_MAGIC 0x4B454550u

#define g        (keep.k_g)
#define cache_base (keep.k_cache)
#define nslots    (keep.k_nslots)
#define nused     (keep.k_nused)
#define rom_size  (keep.k_rom_size)
#define rom_npages (keep.k_npages)
#define SLOT_MEM(s) (cache_base + (uint32_t)(s) * ROM_PAGE_SIZE)
#define save_type (keep.k_save_type)

static int state, err_code;
static int pre_i, pre_n, pre_started;      /* preload progress (S_CACHE) */
static uint32_t page_loads, loads_last_sec;

static uint16_t xmap_[360];
static uint16_t yoff_[240];

static uint32_t next_due_us, last_second_us, frames_emulated, fps_shown, rendered_frames;
static uint32_t ema_us;                    /* smoothed emulation cost per frame (us) */
static int rskip = 1, rskip_cnt;
static uint32_t help_until_us;
static uint32_t save_dirty_since_us;
static int margins_dirty = 1;
static int saved_flash, save_prev;          /* "OK" shown on the save button for a moment */
static uint32_t saved_flash_until_us, save_since_us, last_state_us, menu_since_us;

/* automatic timing experiment: 0 adaptive, 1 never draw, 2 always draw */
static uint32_t boot_us;
static int exp_mode;
static uint64_t acc_skip_us, acc_rend_us, acc_blit_us;
static uint32_t n_skip, n_rend, n_blit;

/* --------------------------------------------------------- ROM cache --- */

static void page_path(char *out, uint32_t page)
{
    char leaf[16];
    int i = 0;
    leaf[i++] = '/'; leaf[i++] = 'p';
    if (page >= 1000) leaf[i++] = (char)('0' + (page / 1000) % 10);   /* 32 MiB ROMs: p1000 .. p1023 */
    leaf[i++] = (char)('0' + (page / 100) % 10);
    leaf[i++] = (char)('0' + (page / 10) % 10);
    leaf[i++] = (char)('0' + page % 10);
    leaf[i++] = '.'; leaf[i++] = 'b'; leaf[i++] = 'i'; leaf[i++] = 'n';
    leaf[i] = 0;
    build_path(out, keep.k_rom, leaf);
}

/* Replacement: LRU. lru_scan() stamps, once per emulated frame, every slot whose page was read (rom_ref, set by the
 * core on every ROM read). The victim is the slot with the oldest stamp among the slots NOT read in the current
 * frame. Measured on a real-game page trace: 64 slots -> 20% fewer disk reads than the old CLOCK. */
static void lru_scan(void)
{
    lru_clock++;
    for (int s = 0; s < nused; s++) {
        int pg = slot_page[s];
        if (g->rom_ref[pg]) { g->rom_ref[pg] = 0; slot_stamp[s] = lru_clock; }
    }
}

static int pick_victim(GBA *gg)
{
    int best = 0;
    uint32_t bs = 0xFFFFFFFFu;
    for (int s = 0; s < nused; s++) {
        uint32_t st = gg->rom_ref[slot_page[s]] ? 0xFFFFFFFEu : slot_stamp[s];   /* read in this frame: keep */
        if (st < bs) { bs = st; best = s; }
    }
    return best;
}

static const u8 *rom_fault(GBA *gg, u32 page)
{
    int s;
    if (page >= rom_npages) {                           /* past the end of the ROM: zeros, no slot, no disk access */
        gg->rom_tab[page] = keep.k_zero;
        return keep.k_zero;
    }
    if (nused < nslots) s = nused++;
    else {
        s = pick_victim(gg);
        int old = slot_page[s];
        gg->rom_tab[old] = 0;
        gg->rom_ref[old] = 0;
    }
    u8 *mem = SLOT_MEM(s);
    char path[96];
    page_path(path, page);
    uint32_t n = hb_fs_read(path, mem, ROM_PAGE_SIZE);
    for (uint32_t i = n; i < ROM_PAGE_SIZE; i++) mem[i] = 0;
    slot_page[s] = (int)page;
    slot_stamp[s] = lru_clock;
    gg->rom_tab[page] = mem;
    page_loads++;
    if ((page_loads & 15) == 1) hb_trace_log("GBAPAGE ", page, page_loads);
    return mem;
}

/* The core's gba_set_rom() / a state load clear the page table: point it at the slots again. */
static void cache_rebind(void)
{
    for (int i = 0; i < ROM_MAX_PAGES; i++) { g->rom_tab[i] = 0; g->rom_ref[i] = 0; }
    for (int s = 0; s < nused; s++) g->rom_tab[slot_page[s]] = SLOT_MEM(s);
}

/* hot.bin: ROM pages resident at the last state save, most recently used first. Written with the state, preloaded
 * behind the LOADING screen after a reload, so the game does not stall on ~100 cold page reads (sim: worst frame
 * 26 pages = 68 ms -> 2-4 pages). Not written when the whole ROM fits the cache (it is preloaded entirely). */
#define HOT_MAGIC 0x31544F48u                       /* 'HOT1' */
static uint16_t hot_pg[MAX_SLOTS];
static int hot_n;

static void hot_write(void)
{
    static uint32_t hdr[3];
    static uint8_t idx[MAX_SLOTS];
    static uint16_t pg[MAX_SLOTS];
    int n = nused;
    if (rom_npages <= (uint32_t)nslots || n <= 0) return;
    for (int i = 0; i < n; i++) {                       /* insertion sort by stamp, newest first */
        int j = i;
        while (j > 0 && slot_stamp[idx[j - 1]] < slot_stamp[i]) { idx[j] = idx[j - 1]; j--; }
        idx[j] = (uint8_t)i;
    }
    for (int i = 0; i < n; i++) pg[i] = (uint16_t)slot_page[idx[i]];
    hdr[0] = HOT_MAGIC; hdr[1] = rom_size; hdr[2] = (uint32_t)n;
    void *const parts[2] = { hdr, pg };
    const uint32_t lens[2] = { sizeof hdr, (uint32_t)n * 2u };
    hb_fs_write_parts(gp("/hot.bin"), parts, lens, 2);
}

static void hot_read(void)
{
    static uint32_t buf[3 + MAX_SLOTS / 2];
    hot_n = 0;
    uint32_t n = hb_fs_read(gp("/hot.bin"), buf, sizeof buf);
    if (n < 12 || buf[0] != HOT_MAGIC || buf[1] != rom_size) return;
    uint32_t cnt = buf[2];
    if (cnt > MAX_SLOTS || n < 12 + cnt * 2u) return;
    const uint16_t *src = (const uint16_t *)(buf + 3);
    for (uint32_t i = 0; i < cnt; i++) if (src[i] < rom_npages) hot_pg[hot_n++] = src[i];
}

/* ------------------------------------------------------------ video --- */

static inline uint32_t rgb(uint16_t c)
{
    /* the compositor needs alpha = 0xFF (the SDK helpers do the same) */
    return 0xFF000000u | ((uint32_t)(c & 31) << 19) | ((uint32_t)((c >> 5) & 31) << 11) | ((uint32_t)((c >> 10) & 31) << 3);
}

static void blit_portrait(const uint16_t *fr)
{
    /* fb stride == GBA width: 1:1 copy into rows PGY..PGY+159 */
    uint32_t *fb = hb_raw_fb() + PGY * 240;
    for (int i = 0; i < GBA_W * GBA_H; i += 4) {
        fb[i] = rgb(fr[i]); fb[i + 1] = rgb(fr[i + 1]);
        fb[i + 2] = rgb(fr[i + 2]); fb[i + 3] = rgb(fr[i + 3]);
    }
}

static void blit(const uint16_t *fr)
{
    if (PORTRAIT) { blit_portrait(fr); return; }
    const int flip = 0;
    uint32_t *fb = hb_raw_fb();
    int prev_gx = -1;
    uint32_t *prev_row = 0;
    for (int lx = 0; lx < 360; lx++) {
        int gx = xmap_[lx];
        uint32_t *row = flip ? fb + (36 + lx) * 240 : fb + (431 - (36 + lx)) * 240;
        if (gx == prev_gx) {
            for (int i = 0; i < 240; i++) row[i] = prev_row[i];
        } else if (!flip) {
            for (int ly = 0; ly < 240; ly++) row[ly] = rgb(fr[yoff_[ly] + gx]);
        } else {
            for (int ly = 0; ly < 240; ly++) row[239 - ly] = rgb(fr[yoff_[ly] + gx]);
        }
        prev_gx = gx; prev_row = row;
    }
}

static void fcircle(int cx, int cy, int r, uint32_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
        frect(cx - w, cy + dy, 2 * w + 1, 1, c);
    }
}

/* portrait control panel below the game (drawn once per layout change) */
static void draw_controls_portrait(void)
{
    frect(0, 180, 240, 252, PBG);
    frect(4, 186, 76, 34, BTN);    text_c(42, 198, "L", 2, FG);
    frect(160, 186, 76, 34, BTN);  text_c(198, 198, "R", 2, FG);
    frect(66 - 18, 300 - 58, 36, 116, BTN);                 /* d-pad cross */
    frect(66 - 58, 300 - 18, 116, 36, BTN);
    fcircle(66, 300, 8, PBG);
    fcircle(200, 286, 30, ACC);    text_c(200, 279, "A", 3, BLACK);
    fcircle(146, 328, 27, 0xC4552D); text_c(146, 321, "B", 3, BLACK);
    frect(48, 394, 66, 28, BTN);   text_c(81, 403, "SEL", 2, DIM);
    frect(126, 394, 66, 28, BTN);  text_c(159, 403, "START", 2, DIM);
    frect(196, 394, 40, 28, BTN);  text_c(216, 403, "EXIT", 2, BAD);   /* hold: free memory */
}

static void draw_status_portrait(void)
{
    frect(0, 0, 240, PGY, BLACK);
    text(4, 5, "FPS", 2, DIM);
    num(34, 5, fps_shown, 2, fps_shown >= 55 ? OKC : (fps_shown >= 30 ? FG : BAD));
    frect(56, 1, 58, 18, BTN);                                         /* hold: ROM menu */
    text_c(85, 5, "ROMS", 2, ACC);
    frect(122, 1, 62, 18, saved_flash ? OKC : BTN);
    text_c(153, 5, saved_flash ? "OK" : "SAVE", 2, saved_flash ? BLACK : FG);
    frect(192, 1, 46, 18, BTN);
    text_c(215, 5, "ROT", 2, FG);
    margins_dirty = 0;
}

static void draw_margins(void)
{
    if (PORTRAIT) { draw_status_portrait(); return; }
    frect(0, 0, 36, LH, BLACK);
    frect(396, 0, 36, LH, BLACK);
    text(3, 4, "FPS", 2, DIM);
    num(3, 20, fps_shown, 3, fps_shown >= 55 ? OKC : (fps_shown >= 30 ? FG : BAD));
    text(398, 4, "PG", 2, DIM);
    num(398, 20, loads_last_sec, 3, FG);
    frect(398, 46, 32, 36, saved_flash ? OKC : BTN);
    text(402, 58, saved_flash ? "OK" : "SV", 2, saved_flash ? BLACK : FG);
    frect(398, 92, 32, 36, BTN);                        /* hold: free memory */
    text(399, 104, "EXIT", 2, BAD);
    text(3, 54, "SK", 2, DIM);
    num(3, 70, (uint32_t)rskip, 3, FG);
    frect(2, 100, 32, 36, BTN);                         /* hold: ROM menu */
    text(3, 112, "ROMS", 2, ACC);
    margins_dirty = 0;
}

static void draw_overlay(void)
{
    uint32_t c = 0x4A90D9;
    rect_out(36, 62, 124, 178, c);                      /* d-pad */
    rect_out(36, 0, 74, 40, c);                         /* L */
    rect_out(322, 0, 74, 40, c);                        /* R */
    rect_out(290, 90, 106, 150, c);                     /* A / B */
    rect_out(130, 206, 140, 34, c);                     /* select / start */
    text(44, 66, "DIR", 2, c);
    text(44, 4, "L", 2, c);
    text(386, 4, "R", 2, c);
    text(358, 108, "A", 3, c);
    text(296, 170, "B", 3, c);
    text(136, 214, "SEL", 2, c);
    text(206, 214, "START", 2, c);
    text(204, 12, "ROT", 2, c);
}

/* ------------------------------------------------------------ input --- */

static int rot_touch, save_touch;           /* finger on the ROT / SAVE button this frame */

static uint16_t keys_portrait(int x, int y)
{
    uint16_t k = 0;
    if (y < PGY + 2) { if (x >= 186) rot_touch = 1; else if (x >= 118) save_touch = 1; else if (x >= 54) menu_touch = 1; }
    else if (y < 180) { }                                           /* game image */
    else if (y < 228) k |= x < 120 ? 512 : 256;                     /* L / R */
    else if (y < 386) {
        if (x < 128) {                                              /* d-pad */
            int dx = x - 66, dy = y - 300;
            if (dx > 14) k |= 16; else if (dx < -14) k |= 32;
            if (dy > 14) k |= 128; else if (dy < -14) k |= 64;
        } else {                                                    /* nearest of A / B */
            int ax = x - 200, ay = y - 286, bx = x - 146, by = y - 328;
            k |= (ax * ax + ay * ay <= bx * bx + by * by) ? 1 : 2;
        }
    } else if (x >= 194) free_touch = 1;                            /* EXIT */
    else k |= x < 120 ? 4 : 8;                                      /* select / start */
    return k;
}

static uint16_t read_keys(void)
{
    hb_touch_t f[HB_MAX_FINGERS];
    int n = hb_touch_poll_multi(f);
    uint16_t k = 0;
    if (n < 0) n = 0;
    if (n > HB_MAX_FINGERS) n = HB_MAX_FINGERS;
    rot_touch = save_touch = free_touch = menu_touch = 0;
    any_touch = n > 0;
    for (int i = 0; i < n; i++) {
        if (PORTRAIT) { k |= keys_portrait(f[i].x, f[i].y); continue; }
        int lx, ly;
        to_logical(f[i].x, f[i].y, &lx, &ly);
        if (ly < 40 && lx >= 176 && lx < 256) { rot_touch = 1; continue; }
        if (lx < 36 && ly >= 96 && ly < 140) { menu_touch = 1; continue; }
        if (lx >= 396 && ly >= 90 && ly < 132) { free_touch = 1; continue; }
        if (lx >= 396 && ly >= 44 && ly < 86) { save_touch = 1; continue; }
        if (lx < 160 && ly > 62) {                                  /* d-pad */
            int dx = lx - 98, dy = ly - 150;
            if (dx > 16) k |= 16; else if (dx < -16) k |= 32;
            if (dy > 16) k |= 128; else if (dy < -16) k |= 64;
        } else if (ly < 40 && lx < 110) k |= 512;                   /* L */
        else if (ly < 40 && lx >= 322) k |= 256;                    /* R */
        else if (lx >= 290 && ly >= 90) {                           /* A / B */
            int ax = lx - 372, ay = ly - 148, bx = lx - 316, by = ly - 178;
            if (ax * ax + ay * ay < 55 * 55) k |= 1;
            if (bx * bx + by * by < 48 * 48) k |= 2;
        } else if (ly >= 206 && lx >= 200 && lx < 270) k |= 8;      /* start */
        else if (ly >= 206 && lx >= 130 && lx < 200) k |= 4;        /* select */
    }
    return k;
}

/* ------------------------------------------------------------- save --- */

static uint32_t save_bytes(void)
{
    if (save_type == SAVE_FLASH128) return 0x20000;
    if (save_type == SAVE_FLASH64) return 0x10000;
    if (save_type == SAVE_SRAM) return 0x8000;
    return 0;
}

static void save_flush(void)
{
    uint32_t n = save_bytes();
    if (n) hb_fs_write(gp("/save.sav"), g->sram, n);
    g->flash_dirty = 0;
    save_dirty_since_us = 0;
}

/* ------------------------------------------------------ save state --- */

/* The whole emulator struct goes to disk (~630 KB) behind a 16-byte header;
 * host pointers (ROM page table, callbacks) are re-applied after a load. It is
 * written every minute and when SAVE is held, and loaded on a fresh start, so
 * a reboot / reinstall / closed app continues exactly where it stopped. */
#define STATE_LEAF  "/state.bin"
#define STATE_PATH  gp(STATE_LEAF)
#define STATE_MAGIC 0x54534247u              /* 'GBST' */
#define STATE_VER   1u

static void boot_fresh(void)
{
    gba_set_rom(g, rom_size, rom_fault, 0);
    cache_rebind();                                     /* keep the pages already preloaded */
    gba_reset(g);
    gba_set_save(g, (int)save_type);
    uint32_t n = save_bytes();
    if (n) hb_fs_read(gp("/save.sav"), g->sram, n);
}

static void state_header(uint32_t *h)
{
    h[0] = STATE_MAGIC; h[1] = STATE_VER; h[2] = (uint32_t)sizeof(GBA); h[3] = rom_size;
}

__attribute__((noinline)) static int state_save(void)
{
    static uint32_t hdr[4];
    state_header(hdr);
    void *const parts[2] = { hdr, g };
    const uint32_t lens[2] = { sizeof hdr, (uint32_t)sizeof(GBA) };
    uint32_t t0 = hb_time_uptime_us();
    int ok = hb_fs_write_parts(STATE_PATH, parts, lens, 2);
    hb_trace_log("GBASTSAV", (uint32_t)ok, hb_time_uptime_us() - t0);
    if (ok) hot_write();
    return ok;
}

/* Reads straight into the emulator block: the 16-byte header lands in the
 * spare bytes in front of g (the block header has 32, only 8 are used). */
__attribute__((noinline)) static int state_load(void)
{
    static uint32_t want[4], got[4];
    state_header(want);
    if (hb_fs_read(STATE_PATH, got, sizeof got) != sizeof got) return 0;
    for (int i = 0; i < 4; i++) if (got[i] != want[i]) { hb_trace_log("GBASTBAD", got[1], got[2]); return 0; }
    uint8_t *dst = (uint8_t *)g - 16;
    uint32_t n = hb_fs_read(STATE_PATH, dst, 16 + (uint32_t)sizeof(GBA));
    if (n != 16 + sizeof(GBA)) { boot_fresh(); return 0; }
    gba_set_rom(g, rom_size, rom_fault, 0);             /* the saved struct holds stale host pointers */
    cache_rebind();                                     /* the preloaded slots stay valid */
    hb_trace_log("GBASTLOD", n, 0);
    return 1;
}

/* ------------------------------------------------------- wake lock --- */

/* Same trick as the SDK's hb_wake_lock (LVGL-only): send the OS power/idle
 * singleton the touch-activity event so the panel never dims / turns off. */
#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define HB_KEVENT_TOUCHACTIVITY 4

static uint32_t last_poke_us;
static int poked;

static void wake_poke(uint32_t now)
{
    if (poked && now - last_poke_us < 8000000u) return;
#ifdef LINES_HOST
    (void)now; return;
#endif
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();
    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, HB_KEVENT_TOUCHACTIVITY);
    last_poke_us = now;
    poked = 1;
}

/* ------------------------------------------------------------- main --- */

static int layout_dirty = 1, rot_prev;
static uint32_t rot_since_us;

static void draw_layout(void)
{
    hb_raw_fill(PORTRAIT ? PBG : BLACK);
    if (PORTRAIT) draw_controls_portrait();
    margins_dirty = 1;
    layout_dirty = 0;
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    hb_trace_reset();
    hb_trace_log("GBAINIT ", 0, 0);
    for (int i = 0; i < 360; i++) xmap_[i] = (uint16_t)(i * 2 / 3);
    for (int i = 0; i < 240; i++) yoff_[i] = (uint16_t)((i * 2 / 3) * 240);
    hb_raw_fill(BLACK);
    state = S_START;
    rl_t0 = last_touch_us = hb_time_uptime_us();
    /* Every heap block of the previous launch is gone (the resident frees the aux block before payload_entry):
     * drop the dead pointers. .data survives a relaunch, so keep.k_landscape / keep.k_rom are still valid. */
    if (keep.k_g) hb_trace_log("GBASTALE", (uint32_t)(uintptr_t)keep.k_g, (uint32_t)keep.k_nslots);
    keep.k_g = 0; keep.k_cache = 0; keep.k_zero = 0;
    keep.k_nslots = keep.k_nused = 0;
    keep.booted = 0;
    keep.magic = KEEP_MAGIC;
}

/* keep.bin marker (no pointers): was memory released (EXIT / idle / ROMS) or was a game running (left with Home)? */
__attribute__((noinline)) static void marker_read(void)
{
    kf.magic = 0;
    if (hb_fs_read(KEEP_PATH, &kf, sizeof kf) < 8) return;
    if (kf.magic == REL_MAGIC) { rl_from_rel = 1; rl_menu = kf.tag == 2; }      /* released: resume the last ROM / menu */
    else if (kf.magic == RUN_MAGIC) rl_from_rel = 1;                            /* left running: reload the last ROM */
}

__attribute__((noinline)) static void marker_write(uint32_t magic, uint32_t tag)
{
    kf.magic = magic; kf.tag = tag; kf.pad0 = kf.pad1 = 0;
    hb_fs_write(KEEP_PATH, &kf, sizeof kf);
}

/* Size and allocate the one heap block [hdr][GBA][cache slots][zero page]. `want` = slots the ROM could use.
 * 1) planned from hb_os_heap_free()/largest(), keeping HEAP_RESERVE free; 2) the real consumption is measured
 * (free before/after) because the OS allocator may round the request up to a power of two: if the reserve got
 * eaten the block is returned and the request shrinks to what the rounded size gives; 3) fallback 64 / 32 / 16. */
#define GBA_PAD   (((uint32_t)sizeof(GBA) + 31u) & ~31u)
#define BLK_FIXED (BLK_HDR + GBA_PAD + ROM_PAGE_SIZE)          /* + the shared zero page */
/* Does the OS allocator round requests up (power-of-two classes)? Probed with a 1.25 MiB + 4 KiB request, measured as
 * the drop of hb_os_heap_free(), and freed at once. The probe stays below the reserve: when there is no room for it
 * the worst case (rounding) is assumed. Never probe with the full-size request: a power-of-two class could drain the
 * heap for an instant and an OS allocation in that window would panic. */
static int alloc_rounds_up(uint32_t f0)
{
    const uint32_t R = 1310720u + 4096u;
    if (f0 < HEAP_RESERVE + 2u * 1024u * 1024u + 256u * 1024u) return 1;
    uint32_t b = hb_os_heap_free();
    void *p = hb_os_alloc(R);
    if (!p) return 1;
    uint32_t a = hb_os_heap_free();
    hb_os_free(p);
    uint32_t c = b > a ? b - a : R;
    hb_trace_log("GBAPROBE", R, c);
    return c > R + R / 16u;
}

__attribute__((noinline)) static void *alloc_block(int want, int *slots_out)
{
    uint32_t f0 = hb_os_heap_free(), lg0 = hb_os_heap_largest();
    int need = want < MIN_SLOTS ? want : MIN_SLOTS;
    uint32_t cap = f0 > HEAP_RESERVE ? f0 - HEAP_RESERVE : 0;
    int slots;
    uint32_t last_used = 0;
    hb_trace_log("GBAHEAP ", f0, lg0);
    if (cap > lg0) cap = lg0;
    if (alloc_rounds_up(f0)) {                                  /* power-of-two classes: use the biggest class that fits */
        uint32_t cls = 0x80000000u;
        while (cls > cap) cls >>= 1;
        cap = cls;
    }
    slots = cap > BLK_FIXED ? (int)((cap - BLK_FIXED) / ROM_PAGE_SIZE) : 0;
    if (slots > want) slots = want;
    for (int attempt = 0; attempt < 6 && slots >= need && slots > 0; attempt++) {
        uint32_t bytes = BLK_FIXED + (uint32_t)slots * ROM_PAGE_SIZE;
        uint32_t fb = hb_os_heap_free();
        void *p = hb_os_alloc(bytes);
        if (!p) { slots -= slots / 4 + 1; continue; }
        uint32_t fa = hb_os_heap_free(), used = fb > fa ? fb - fa : bytes;
        hb_trace_log("GBABLK  ", bytes, used);
        if (fa >= HEAP_RESERVE) { *slots_out = slots; keep.k_blk_bytes = bytes; return p; }
        hb_os_free(p);
        if (used > bytes + 4096u || used == last_used) {       /* rounded up (power of two): fit the half-size class */
            uint32_t half = used / 2;
            slots = half > BLK_FIXED ? (int)((half - BLK_FIXED) / ROM_PAGE_SIZE) : 0;
        } else {
            slots -= (int)((HEAP_RESERVE - fa + ROM_PAGE_SIZE - 1) / ROM_PAGE_SIZE);
        }
        last_used = used;
        if (slots > want) slots = want;
    }
    static const int fb_slots[3] = { FALLBACK_SLOTS, 32, MIN_SLOTS };
    for (int i = 0; i < 3; i++) {
        int n = fb_slots[i] < want ? fb_slots[i] : want;
        uint32_t bytes = BLK_FIXED + (uint32_t)n * ROM_PAGE_SIZE;
        if (hb_os_heap_largest() < bytes || hb_os_heap_free() < bytes + 512u * 1024u) continue;
        void *p = hb_os_alloc(bytes);
        if (p) { *slots_out = n; keep.k_blk_bytes = bytes; hb_trace_log("GBAFALLB", (uint32_t)n, bytes); return p; }
    }
    return 0;
}

/* "MEMORY RELEASED" screen (nothing of the app is allocated any more) */
static void freed_screen(void)
{
    char b[40];
    int i;
    hb_raw_fill(BLACK);
    text_c(LW / 2, 50, "MEMORY RELEASED", 3, OKC);
    text_c(LW / 2, 82, "YOU MAY LEAVE", 3, FG);
    if (fr_menu) {                                            /* idle in the ROM menu: no game was loaded */
        text_c(LW / 2, 122, "NO GAME IN MEMORY", 2, FG);
        text_c(LW / 2, 162, "10 MIN IDLE", 2, DIM);
        text_c(LW / 2, 196, "TAP TO RETURN TO MENU", 2, ACC);
        return;
    }
    i = 0; { const char *s = "FREED "; while (*s) b[i++] = *s++; }
    i += utoa((rel_freed + 1023u) / 1024u, b + i);
    b[i++] = ' '; b[i++] = 'K'; b[i++] = 'B'; b[i] = 0;
    text_c(LW / 2, 122, b, 2, FG);
    i = 0; { const char *s = "FREE "; while (*s) b[i++] = *s++; }
    i += utoa((rel_heap1 + 1023u) / 1024u, b + i);
    b[i++] = ' '; b[i++] = 'K'; b[i++] = 'B'; b[i] = 0;
    text_c(LW / 2, 140, b, 2, DIM);
    text_c(LW / 2, 162, rel_reason == 2 ? "10 MIN IDLE - GAME SAVED" : "GAME SAVED", 2, DIM);
    text_c(LW / 2, 196, "TAP TO RETURN", 2, ACC);
}

/* Free the one heap block (emulator + ROM page cache) and mark keep.bin "nothing held" (tag 2: next launch = menu).
 * The aux pointer is cleared first so the resident never frees it a second time. */
__attribute__((noinline)) static void free_blocks(uint32_t tag)
{
    if (g) {
        uint32_t *blk = (uint32_t *)((u8 *)g - BLK_HDR);
        blk[0] = 0;
        HB_APP_AUX_BLOCK = 0;
        hb_os_free(blk);
    }
    g = 0; cache_base = 0; keep.k_zero = 0;
    nslots = nused = 0;
    keep.booted = 0;
    marker_write(REL_MAGIC, tag);
}

/* Save everything, then free every heap block of the app (why: 1 EXIT, 2 idle, 3 ROMS -> menu). The next boot
 * (tap here, or the next launch) loads the save state from disk. Aborted if the save state cannot be written. */
__attribute__((noinline)) static void release_memory(int why)
{
    uint32_t t0 = hb_time_uptime_us(), h0 = hb_os_heap_free();
    if (save_bytes()) save_flush();                           /* in-game save (flash / SRAM) */
    if (!state_save()) { hb_trace_log("GBAFREE ", 0, 0xFFFFFFFFu); return; }
    free_blocks(why == 3 ? 2u : 0u);
    keep.released = (uint32_t)why;
    rel_reason = why;
    rel_heap1 = hb_os_heap_free();
    rel_freed = rel_heap1 > h0 ? rel_heap1 - h0 : 0;
    /* GBAFREE: a = bytes freed, b = heap free now; GBAFRWHY: a = reason (1 EXIT, 2 idle), b = ms */
    hb_trace_log("GBAFREE ", rel_freed, rel_heap1);
    hb_trace_log("GBAFRWHY", (uint32_t)why, (hb_time_uptime_us() - t0) / 1000u);
    if (why == 3) { force_menu = 1; state = S_START; return; }   /* ROMS: straight to the menu */
    fr_menu = 0;
    fr_wait_up = 1;
    state = S_FREED;
    freed_screen();
}

static void error_screen(void)
{
    hb_raw_fill(BLACK);
    text_c(LW / 2, 60, "ERROR", 4, BAD);
    if (err_code == 1) { text_c(LW / 2, 110, "OUT OF MEMORY", 2, FG); }
    else if (err_code == 2) { text_c(LW / 2, 110, "ROM NOT FOUND", 2, FG); text_c(LW / 2, 130, "COPY THE FILES TO GBA", 2, DIM); }
    else { text_c(LW / 2, 110, "FAILED", 2, FG); }
    text_c(LW / 2, 180, "TAP FOR MENU", 2, ACC);
}

/* ------------------------------------------------------- ROM list + menu --- */

/* upper-case, printable name for a folder name ("my_game" -> "MY GAME") */
static void disp_name(char *out, const char *dir, int maxc)
{
    int n = 0;
    for (; dir[n] && n < maxc; n++) {
        char c = dir[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '_') c = ' ';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ' ')) c = ' ';
        out[n] = c;
    }
    out[n] = 0;
}

static int scmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static void scpy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* Every sub-folder of GBA_ROOT with a valid meta.bin + p000.bin is a ROM (sorted by folder name). */
__attribute__((noinline)) static void scan_roms(void)
{
    static char names[MAX_ROMS][ROM_NAME];
    int nn = 0;
    hb_dir_t d;
    char fn[96], p[96];
    bool is_dir = false;
    nroms = 0;
    if (!hb_fs_dir_open(&d, GBA_ROOT, false)) return;
    while (nn < MAX_ROMS && hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        const char *nm = fn;
        if (!is_dir) continue;
        for (const char *q = fn; *q; q++) if (*q == '/') nm = q + 1;
        int len = slen(nm);
        if (len == 0 || len >= ROM_NAME || nm[0] == '.') continue;
        scpy(names[nn++], nm, ROM_NAME);
    }
    hb_fs_dir_close(&d);
    for (int i = 0; i < nn; i++) {
        uint8_t meta[8];
        if (hb_fs_read(build_path(p, names[i], "/meta.bin"), meta, 8) < 8) continue;
        uint32_t size = (uint32_t)meta[0] | ((uint32_t)meta[1] << 8) | ((uint32_t)meta[2] << 16) | ((uint32_t)meta[3] << 24);
        if (size == 0 || hb_fs_size(build_path(p, names[i], "/p000.bin")) == 0) continue;
        int k = nroms++;                                       /* insertion sort */
        while (k > 0 && scmp(roms[k - 1].dir, names[i]) > 0) { roms[k] = roms[k - 1]; k--; }
        scpy(roms[k].dir, names[i], ROM_NAME);
        roms[k].size = size;
        roms[k].stype = meta[4];
        roms[k].bad = size > ROM_MAX_BYTES;
        roms[k].has_save = hb_fs_exists(build_path(p, names[i], "/save.sav")) || hb_fs_exists(build_path(p, names[i], STATE_LEAF));
    }
}

static int find_rom(const char *dir)
{
    if (!dir[0]) return -1;
    for (int i = 0; i < nroms; i++) if (!scmp(roms[i].dir, dir)) return i;
    return -1;
}

static void read_last(void)
{
    char b[ROM_NAME + 4];
    uint32_t n = hb_fs_read(LAST_PATH, b, ROM_NAME - 1);
    int i = 0;
    while (i < (int)n && (uint8_t)b[i] > ' ') { last_name[i] = b[i]; i++; }
    last_name[i] = 0;
}

static void write_last(void)
{
    if (scmp(last_name, keep.k_rom) == 0) return;
    hb_fs_write(LAST_PATH, keep.k_rom, (uint32_t)slen(keep.k_rom));
    scpy(last_name, keep.k_rom, ROM_NAME);
}

#define MN_TOP 34
#define MN_ROW 54
#define MN_BOT 26
static int mn_scroll, mn_wait_up, mn_drag, mn_down, mn_sx, mn_sy, mn_sscroll, mn_dirty;

static int mn_listh(void) { return LH - MN_TOP - MN_BOT; }
static int mn_max(void) { int m = nroms * MN_ROW - mn_listh(); return m > 0 ? m : 0; }
static int mn_clamp(int s) { int m = mn_max(); return s < 0 ? 0 : (s > m ? m : s); }

static void menu_enter(void)
{
    mn_scroll = 0;
    if (cur_idx >= 0) mn_scroll = mn_clamp(cur_idx * MN_ROW - (mn_listh() - MN_ROW) / 2);
    mn_wait_up = 1; mn_down = mn_drag = 0;
    mn_dirty = 1;
    last_touch_us = hb_time_uptime_us();
    state = S_MENU;
    hb_trace_log("GBAROMS ", (uint32_t)nroms, (uint32_t)(cur_idx + 1));
}

static void menu_draw(void)
{
    char b[40];
    hb_raw_fill(PBG);
    if (nroms == 0) {
        text_c(LW / 2, 100, "NO ROMS FOUND", 3, FG);
        text_c(LW / 2, 140, "RUN PREPARE ROM AND", 2, DIM);
        text_c(LW / 2, 158, "COPY TO GBA", 2, DIM);
    }
    for (int i = 0; i < nroms; i++) {
        int y = MN_TOP + i * MN_ROW - mn_scroll;
        if (y + MN_ROW <= MN_TOP || y >= LH - MN_BOT) continue;
        const rom_t *r = &roms[i];
        int hl = (i == cur_idx), x = 14, k;
        frect(6, y, LW - 12, MN_ROW - 4, BTN);
        if (hl) { rect_out(6, y, LW - 12, MN_ROW - 4, ACC); rect_out(7, y + 1, LW - 14, MN_ROW - 6, ACC); }
        disp_name(b, r->dir, (LW - 28) / 8);
        text(x, y + 8, b, 2, r->bad ? DIM : FG);
        if (r->bad) { text(x, y + 28, "OVER 32 MB", 2, BAD); continue; }
        k = 0;
        if (r->size >= 1048576u) { k += utoa((r->size + 524288u) >> 20, b); b[k++] = ' '; b[k++] = 'M'; b[k++] = 'B'; b[k] = 0; }
        else { k += utoa((r->size + 512u) >> 10, b); b[k++] = ' '; b[k++] = 'K'; b[k++] = 'B'; b[k] = 0; }
        text(x, y + 28, b, 2, DIM);
        text(x + 8 * 7, y + 28, r->has_save ? "SAVE" : "NEW", 2, r->has_save ? OKC : DIM);
        if (hl) text(LW - 6 - 4 * 8 - 6, y + 28, "LAST", 2, ACC);
    }
    if (mn_max() > 0) {                                        /* scroll bar */
        int lh = mn_listh(), th = lh * lh / (nroms * MN_ROW);
        if (th < 12) th = 12;
        frect(LW - 4, MN_TOP + (lh - th) * mn_scroll / mn_max(), 3, th, DIM);
    }
    frect(0, 0, LW, MN_TOP, PBG);                              /* header / footer cover the rows' overflow */
    frect(0, LH - MN_BOT, LW, MN_BOT, PBG);
    text(8, 8, "ROMS", 3, FG);
    num(8 + 4 * 12 + 8, 8, (uint32_t)nroms, 3, ACC);
    frect(LW - 52, 5, 46, 24, BTN);
    text_c(LW - 29, 12, "ROT", 2, FG);
    frect(0, MN_TOP - 2, LW, 1, BTN);
    text_c(LW / 2, LH - 19, nroms ? "TAP TO PLAY" : "NO ROMS", 2, DIM);
}

static void start_rom(int idx)
{
    scpy(keep.k_rom, roms[idx].dir, ROM_NAME);
    cur_idx = idx;
    hb_trace_log("GBAPICK ", (uint32_t)idx, roms[idx].size);
    rl_t0 = hb_time_uptime_us();
    force_menu = 0;
    state = S_META;
}

static void menu_tap(int x, int y)
{
    if (y < MN_TOP) {
        if (x >= LW - 52 && y < 30) {                           /* ROT: switch portrait / landscape */
            keep.k_landscape = !keep.k_landscape;
            mn_scroll = mn_clamp(mn_scroll);
            mn_dirty = 1;
        }
        return;
    }
    if (y >= LH - MN_BOT) return;
    int rel = y - MN_TOP + mn_scroll, idx = rel / MN_ROW;
    if (idx < 0 || idx >= nroms || rel % MN_ROW >= MN_ROW - 4 || roms[idx].bad) return;
    start_rom(idx);
}

static void menu_frame(const hb_spoint_t *t, uint32_t now)
{
    int down = t && t->down, lx = 0, ly = 0;
    if (down) to_logical(t->x, t->y, &lx, &ly);
    if (mn_wait_up) {                                          /* the finger that held ROMS is still down */
        if (!down) mn_wait_up = 0;
        last_touch_us = now;
    } else if (down) {
        last_touch_us = now;
        if (!mn_down) { mn_down = 1; mn_drag = 0; mn_sx = lx; mn_sy = ly; mn_sscroll = mn_scroll; }
        else {
            int dy = ly - mn_sy;
            if (!mn_drag && (dy > 10 || dy < -10)) mn_drag = 1;
            if (mn_drag) {
                int ns = mn_clamp(mn_sscroll - dy);
                if (ns != mn_scroll) { mn_scroll = ns; mn_dirty = 1; }
            }
        }
    } else if (mn_down) {
        mn_down = 0;
        if (!mn_drag) menu_tap(mn_sx, mn_sy);
    }
    if (state != S_MENU) return;
    if (mn_dirty) { menu_draw(); mn_dirty = 0; }
    if (now - last_touch_us > IDLE_FREE_US) {                  /* 10 min in the menu: nothing is allocated, let the screen sleep */
        rel_reason = 2; rel_freed = 0; rel_heap1 = hb_os_heap_free();
        fr_menu = 1; force_menu = 1; fr_wait_up = 1;
        hb_trace_log("GBAFRWHY", 2, 0xFFFFFFFEu);
        state = S_FREED;
        freed_screen();
    }
}

void hb_raw_frame(const hb_spoint_t *t)
{
    uint32_t now = hb_time_uptime_us();
    if (state == S_FREED) {                                   /* nothing allocated; the screen may sleep */
        int down = t && t->down;
        if (!down) fr_wait_up = 0;
        else if (!fr_wait_up) {                               /* a new tap: boot again from the save state */
            rl_t0 = now;
            last_touch_us = now;
            state = force_menu ? S_START : S_META;            /* idle in the menu: back to it; else the last ROM */
        }
        return;
    }
    wake_poke(now);

    if (state == S_START) {                                   /* decide: menu or straight into a ROM */
        char nm[ROM_NAME];
        marker_read();
        scan_roms();
        read_last();
        cur_idx = find_rom(last_name);
        if (cur_idx < 0) cur_idx = find_rom(keep.k_rom);
        int menu = force_menu || rl_menu || keep.released == 3;
        if (!menu) {
            if (cur_idx >= 0 && (rl_from_rel || keep.released)) { }       /* after EXIT / idle: resume the last ROM */
            else if (nroms == 1) cur_idx = 0;                              /* single ROM: no menu */
            else menu = 1;
        }
        if (cur_idx >= 0 && roms[cur_idx].bad) menu = 1;
        if (!menu && cur_idx < 0) menu = 1;
        force_menu = 0;
        hb_trace_log("GBAROMS ", (uint32_t)nroms, (uint32_t)(cur_idx + 1) | (menu ? 0x100u : 0u));
        if (menu) { hb_raw_fill(PBG); menu_enter(); return; }
        scpy(nm, roms[cur_idx].dir, ROM_NAME);
        scpy(keep.k_rom, nm, ROM_NAME);
        state = S_META;
        return;
    }
    if (state == S_MENU) { menu_frame(t, now); return; }

    if (state == S_META) {                                    /* ROM size / save type, then size the block */
        uint8_t meta[8];
        char nm[40];
        hb_raw_fill(BLACK);
        text_c(LW / 2, 90, "LOADING", 3, FG);
        disp_name(nm, keep.k_rom, LW / 8 - 2);
        text_c(LW / 2, 124, nm, 2, DIM);
        if (hb_fs_read(gp("/meta.bin"), meta, 8) < 8) { err_code = 2; state = S_ERROR; return; }
        rom_size = (uint32_t)meta[0] | ((uint32_t)meta[1] << 8) | ((uint32_t)meta[2] << 16) | ((uint32_t)meta[3] << 24);
        save_type = (uint32_t)meta[4];
        rom_npages = (rom_size + ROM_PAGE_SIZE - 1) / ROM_PAGE_SIZE;
        if (rom_npages > ROM_MAX_PAGES) rom_npages = ROM_MAX_PAGES;
        hb_trace_log("GBAMETA ", rom_size, save_type);
        state = S_ALLOC;
        return;
    }
    if (state == S_ALLOC) {
        int want = rom_npages > MAX_SLOTS ? MAX_SLOTS : (int)rom_npages, n = 0;
        if (want < 1) want = 1;
        uint32_t *blk = (uint32_t *)alloc_block(want, &n);
        hb_trace_log("GBAALLOC", (uint32_t)(uintptr_t)blk, (uint32_t)sizeof(GBA));
        if (!blk || n < 1) { err_code = 1; state = S_ERROR; return; }
        HB_APP_AUX_BLOCK = blk;                               /* the resident frees it when the app is unloaded */
        blk[0] = BLK_MAGIC;
        blk[1] = (now ^ 0x9E3779B9u) | 1u;
        g = (GBA *)((u8 *)blk + BLK_HDR);
        cache_base = (u8 *)g + GBA_PAD;
        keep.k_zero = cache_base + (uint32_t)n * ROM_PAGE_SIZE;
        nslots = n;
        nused = 0;
        lru_clock = 0;
        {   /* OS heap memory is not zeroed: clear the whole emulator state (+ the shared zero page) */
            uint32_t *w = (uint32_t *)g;
            for (uint32_t i = 0; i < sizeof(GBA) / 4; i++) w[i] = 0;
            w = (uint32_t *)keep.k_zero;
            for (uint32_t i = 0; i < ROM_PAGE_SIZE / 4; i++) w[i] = 0;
        }
        for (int i = 0; i < MAX_SLOTS; i++) { slot_page[i] = 0; slot_stamp[i] = 0; }
        hb_trace_log("GBASLOTS", (uint32_t)nslots, hb_os_heap_free());
        pre_i = pre_n = pre_started = 0;
        state = S_CACHE;
        return;
    }
    if (state == S_CACHE) {
        /* Preload behind the LOADING screen. A ROM that fits the cache whole is read entirely (no faults later);
         * otherwise the pages listed in hot.bin (resident at the last save), oldest first so LRU order survives. */
        if (!pre_started) {
            if (rom_npages <= (uint32_t)nslots) pre_n = (int)rom_npages;
            else {
                hot_read();
                pre_n = hot_n < nslots ? hot_n : nslots;
            }
            pre_i = 0; pre_started = 1;
            hb_trace_log("GBAHOT  ", (uint32_t)pre_n, rom_npages <= (uint32_t)nslots);
        }
        uint32_t t0 = hb_time_uptime_us();
        while (pre_i < pre_n && hb_time_uptime_us() - t0 < 18000u) {
            uint32_t page = rom_npages <= (uint32_t)nslots ? (uint32_t)pre_i : hot_pg[pre_n - 1 - pre_i];
            lru_clock++;
            if (!g->rom_tab[page]) rom_fault(g, page);
            pre_i++;
        }
        if (pre_n > 0) {                                      /* progress bar */
            int bw = LW - 80, fw = bw * pre_i / pre_n;
            frect(40, 170, bw, 10, BTN);
            frect(40, 170, fw, 10, ACC);
        }
        if (pre_i < pre_n) return;
        state = S_BOOT;
        return;
    }
    if (state == S_BOOT) {
        boot_fresh();
        int resumed = state_load();                           /* continue from the last save state */
        g->sound_hle = 1;
        g->skip_render = 0;
        next_due_us = now;
        boot_us = now - 30000000u;                            /* timing experiment off */
        last_state_us = hb_time_uptime_us();
        if (resumed) { saved_flash = 1; saved_flash_until_us = last_state_us + 1500000u; }
        last_second_us = now;
        help_until_us = now + 8000000u;
        ema_us = 16000;
        hb_raw_fill(BLACK);
        layout_dirty = 1;
        state = S_RUN;
        keep.booted = 1;
        hb_trace_log("GBABOOT ", 0, 0);
        write_last();                                         /* remember the ROM for the next launch */
        marker_write(RUN_MAGIC, 0);                           /* left with Home -> the next launch reloads this ROM */
        if (rl_from_rel || keep.released) {                   /* GBARELOD: ms to reload after a release, resumed */
            hb_trace_log("GBARELOD", (hb_time_uptime_us() - rl_t0) / 1000u, (uint32_t)resumed);
            rl_from_rel = 0;
            keep.released = 0;
        }
        last_touch_us = hb_time_uptime_us();
        /* header check: game code at 0xAC and the first word of the title */
        hb_trace_log("GBAROM  ", mem_read32(g, 0x080000AC), mem_read32(g, 0x080000A0));
        hb_trace_log("GBAROM0 ", mem_read32(g, 0x08000000), mem_read32(g, 0x08000004));
        return;
    }
    if (state == S_ERROR) {
        if (!(t && t->down)) err_armed = 1;
        else if (err_armed) {                                 /* tap: back to the ROM menu (nothing stays allocated) */
            err_armed = 0;
            if (keep.k_g || nslots) free_blocks(2u);
            keep.released = 3;
            force_menu = 1;
            state = S_START;
            return;
        }
        error_screen();
        return;
    }

    /* ---- running ---- */
    if ((int32_t)(now - next_due_us) < 0) return;           /* ahead of schedule */

    int due = 0;
    while ((int32_t)(now - next_due_us) >= 0 && due < 3) { due++; next_due_us += FRAME_US; }
    if ((int32_t)(now - next_due_us) > 100000) next_due_us = now;   /* far behind: resync */

    g->keys = read_keys();
    /* ROT must be held ~0.6 s (stray touches near the corner must not rotate) */
    if (!rot_touch) rot_prev = 0;
    else if (!rot_prev) { rot_prev = 1; rot_since_us = now; }
    else if (rot_prev == 1 && now - rot_since_us > 600000u) {
        rot_prev = 2;                                       /* fired; wait for release */
        keep.k_landscape = !keep.k_landscape;
        layout_dirty = 1;
        help_until_us = now + 4000000u;
    }
    /* EXIT must be held ~0.6 s too; no touch for IDLE_FREE_US: same release (after a save state) */
    if (any_touch) last_touch_us = now;
    if (!free_touch) free_prev = 0;
    else if (!free_prev) { free_prev = 1; free_since_us = now; }
    else if (free_prev == 1 && now - free_since_us > 600000u) { free_prev = 2; release_memory(1); if (state != S_RUN) return; }
    /* ROMS held ~0.6 s: save + free everything (as EXIT), then back to the ROM menu */
    if (!menu_touch) menu_prev = 0;
    else if (!menu_prev) { menu_prev = 1; menu_since_us = now; }
    else if (menu_prev == 1 && now - menu_since_us > 600000u) { menu_prev = 2; release_memory(3); if (state != S_RUN) return; }
    if (now - last_touch_us > IDLE_FREE_US) { last_touch_us = now; release_memory(2); if (state != S_RUN) return; }

    uint32_t sec = (now - boot_us) / 1000000u;
    exp_mode = sec < 6 ? 0 : (sec < 14 ? 1 : (sec < 22 ? 2 : 0));

    uint32_t t0 = hb_time_uptime_us();
    int rendered = 0;
    for (int i = 0; i < due; i++) {
        int last = (i == due - 1);
        rskip_cnt++;
        int want = last && (rskip_cnt >= rskip);
        if (exp_mode == 1) want = 0;
        else if (exp_mode == 2) want = last;
        g->skip_render = !want;
        if (want) rskip_cnt = 0;
        uint32_t f0 = hb_time_uptime_us();
        gba_run_frame(g);
        lru_scan();                                       /* stamp the ROM pages this frame read (LRU) */
        uint32_t fd = hb_time_uptime_us() - f0;
        if (want) { acc_rend_us += fd; n_rend++; } else { acc_skip_us += fd; n_skip++; }
        frames_emulated++;
        if (want) rendered = 1;
    }
    uint32_t spent = hb_time_uptime_us() - t0;
    uint32_t per = spent / (uint32_t)due;
    ema_us = (ema_us * 7 + per) / 8;
    if (exp_mode == 0) rskip = ema_us <= 13000 ? 1 : (ema_us <= 28000 ? 2 : (ema_us <= 45000 ? 3 : 4));

    if (rendered) {
        if (layout_dirty) draw_layout();
        uint32_t b0 = hb_time_uptime_us();
        blit(g->frame);
        acc_blit_us += hb_time_uptime_us() - b0; n_blit++;
        rendered_frames++;
        if (margins_dirty) draw_margins();
        if (!PORTRAIT && (int32_t)(help_until_us - now) > 0) draw_overlay();
    }

    if (g->flash_dirty) {
        if (!save_dirty_since_us) save_dirty_since_us = now | 1;
        else if (now - save_dirty_since_us > 2500000u) save_flush();
    }

    /* save state: SAVE held ~0.5 s, or automatically every minute */
    {
        int manual = 0;
        if (!save_touch) save_prev = 0;
        else if (!save_prev) { save_prev = 1; save_since_us = now; }
        else if (save_prev == 1 && now - save_since_us > 500000u) { save_prev = 2; manual = 1; }
        if (manual || now - last_state_us > 60000000u) {
            state_save();
            last_state_us = hb_time_uptime_us();
            next_due_us = last_state_us;                       /* don't rush to catch up after the write */
            if (manual) { saved_flash = 1; saved_flash_until_us = last_state_us + 1500000u; margins_dirty = 1; }
        }
        if (saved_flash && (int32_t)(now - saved_flash_until_us) > 0) { saved_flash = 0; margins_dirty = 1; }
    }

    if (now - last_second_us >= 1000000u) {
        fps_shown = frames_emulated;
        loads_last_sec = page_loads;
        hb_trace_log("GBASEC  ", fps_shown | ((uint32_t)exp_mode << 16), page_loads);
        hb_trace_log("GBAEMU  ", n_skip ? (uint32_t)(acc_skip_us / n_skip) : 0, n_skip);
        hb_trace_log("GBAREND ", n_rend ? (uint32_t)(acc_rend_us / n_rend) : 0, n_rend);
        hb_trace_log("GBABLIT ", n_blit ? (uint32_t)(acc_blit_us / n_blit) : 0, n_blit);
        hb_trace_log("GBAINSN ", gba_icount, gba_hle_status(g));   /* instructions/s, HLE flags */
        gba_icount = 0;
        {
            uint32_t sum = 0;
            for (int i = 0; i < GBA_W * GBA_H; i += 97) sum += g->frame[i];
            hb_trace_log("GBAPC   ", g->r[15], g->cpsr);
            hb_trace_log("GBAFRM  ", sum, *(uint16_t *)&g->io[0] | ((uint32_t)g->vcount << 16));
        }
        acc_skip_us = acc_rend_us = acc_blit_us = 0; n_skip = n_rend = n_blit = 0;
        frames_emulated = 0; page_loads = 0;
        last_second_us = now;
        margins_dirty = 1;
    }
}
