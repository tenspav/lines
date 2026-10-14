/* core_test.c - emulator core tests on the PC with tiny SYNTHETIC ARM / Thumb programs written here as machine
 * code words (no ROM images, no BIOS file). Each test builds a ROM page in memory, runs a few frames and checks
 * registers' effects in IWRAM / frame buffer / save RAM.
 *   gcc -O1 -I../src -o core_test core_test.c ../src/cpu.c ../src/mem.c ../src/ppu.c ../src/bios.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba.h"

static u8 page[ROM_PAGE_SIZE];
static u8 zero_page[ROM_PAGE_SIZE];
static GBA *g;
static int fails, checks;

static const u8 *fault(GBA *gg, u32 pg) { (void)gg; return pg == 0 ? page : zero_page; }

static void check(const char *what, int ok)
{
    checks++;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static void put32(u32 off, u32 v) { memcpy(page + off, &v, 4); }

/* load WORDS (n of them) at the ROM entry point, reset the core, run FRAMES frames */
static void run(const u32 *w, int n, int frames)
{
    memset(page, 0, sizeof page);
    for (int i = 0; i < n; i++) put32((u32)i * 4, w[i]);
    memset(g, 0, sizeof *g);
    gba_set_rom(g, ROM_PAGE_SIZE, fault, 0);
    gba_reset(g);
    for (int i = 0; i < frames; i++) gba_run_frame(g);
}

static u32 iw(u32 off) { u32 v; memcpy(&v, g->iwram + off, 4); return v; }

#define B_SELF 0xEAFFFFFEu

int main(void)
{
    g = calloc(1, sizeof *g);
    if (!g) return 2;

    /* 1. ARM data processing, stores, flags and conditional execution */
    {
        const u32 p[] = {
            0xE3A00403,   /* mov r0,#0x03000000        */
            0xE3A01005,   /* mov r1,#5                 */
            0xE2812007,   /* add r2,r1,#7              */
            0xE5802000,   /* str r2,[r0]               */
            0xE3A0100A,   /* mov r1,#10                */
            0xE251100A,   /* subs r1,r1,#10 (Z set)    */
            0x03A02001,   /* moveq r2,#1               */
            0x13A02002,   /* movne r2,#2               */
            0xE5802004,   /* str r2,[r0,#4]            */
            B_SELF };
        run(p, 10, 1);
        check("ARM: add + str to IWRAM (5 + 7 = 12)", iw(0) == 12);
        check("ARM: subs sets Z, moveq taken, movne skipped", iw(4) == 1);
    }

    /* 2. ARM -> Thumb switch, Thumb ALU / shift / store */
    {
        const u32 p[] = {
            0xE3A00403,                 /* mov r0,#0x03000000                         */
            0xE28F1001,                 /* add r1,pc,#1   (address of the Thumb code + 1) */
            0xE12FFF11,                 /* bx r1                                      */
            0x31062107,                 /* movs r1,#7 ; adds r1,#6                    */
            0x22036041,                 /* str r1,[r0,#4] ; movs r2,#3                */
            0x60820092,                 /* lsls r2,r2,#2 ; str r2,[r0,#8]            */
            0x0000E7FE };               /* b .  (Thumb)                               */
        run(p, 7, 1);
        check("Thumb: movs/adds/str (7 + 6 = 13)", iw(4) == 13);
        check("Thumb: lsls (3 << 2 = 12)", iw(8) == 12);
        check("CPU is in Thumb state", (g->cpsr & PSR_T) != 0);
    }

    /* 3. SWI Div through the software BIOS */
    {
        const u32 p[] = {
            0xE3A04403,   /* mov r4,#0x03000000 */
            0xE3A00064,   /* mov r0,#100        */
            0xE3A01007,   /* mov r1,#7          */
            0xEF060000,   /* swi 6 (Div)        */
            0xE5840000,   /* str r0,[r4]        */
            0xE5841004,   /* str r1,[r4,#4]     */
            0xE5843008,   /* str r3,[r4,#8]     */
            B_SELF };
        run(p, 8, 1);
        check("SWI Div: 100 / 7 = 14", iw(0) == 14);
        check("SWI Div: remainder 2", iw(4) == 2);
        check("SWI Div: abs quotient 14", iw(8) == 14);
    }

    /* 4. SWI LZ77 decompression: literals, then a back reference */
    {
        u32 p[0x80 / 4] = {
            0xE3A04403,   /* mov r4,#0x03000000          */
            0xE3A00302,   /* mov r0,#0x08000000          */
            0xE2800C01,   /* add r0,r0,#0x100            */
            0xE2841C01,   /* add r1,r4,#0x100            */
            0xEF110000,   /* swi 0x11 (LZ77 to WRAM)     */
            0xE3A00302,   /* mov r0,#0x08000000          */
            0xE2800F48,   /* add r0,r0,#0x120            */
            0xE2841C02,   /* add r1,r4,#0x200            */
            0xEF110000,   /* swi 0x11                    */
            B_SELF };
        memset(page, 0, sizeof page);
        run(p, 10, 0);
        static const u8 s1[] = { 0x10, 0x08, 0x00, 0x00, 0x00, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H' };
        static const u8 s2[] = { 0x10, 0x07, 0x00, 0x00, 0x10, 'A', 'B', 'C', 0x10, 0x02 };
        memcpy(page + 0x100, s1, sizeof s1);
        memcpy(page + 0x120, s2, sizeof s2);
        gba_run_frame(g);
        check("LZ77: 8 literals", memcmp(g->iwram + 0x100, "ABCDEFGH", 8) == 0);
        check("LZ77: back reference expands to ABCABCA", memcmp(g->iwram + 0x200, "ABCABCA", 7) == 0);
    }

    /* 5. Mode 3 bitmap: two pixels written to VRAM appear in the frame buffer */
    {
        const u32 p[] = {
            0xE3A00404,   /* mov r0,#0x04000000 (I/O)    */
            0xE3A01B01,   /* mov r1,#0x400               */
            0xE2811003,   /* add r1,r1,#3  (mode 3, BG2) */
            0xE1C010B0,   /* strh r1,[r0]  DISPCNT       */
            0xE3A02406,   /* mov r2,#0x06000000 (VRAM)   */
            0xE3A0301F,   /* mov r3,#0x1F  (red)         */
            0xE1C230B0,   /* strh r3,[r2]                */
            0xE3A05FF8,   /* mov r5,#0x3E0 (green)       */
            0xE1C250B2,   /* strh r5,[r2,#2]             */
            B_SELF };
        run(p, 10, 3);
        check("PPU mode 3: pixel (0,0) is red (0x001F)", g->frame[0] == 0x001F);
        check("PPU mode 3: pixel (1,0) is green (0x03E0)", g->frame[1] == 0x03E0);
        check("PPU mode 3: pixel (2,0) is black", g->frame[2] == 0);
    }

    /* 6. SRAM byte write lands in the save memory */
    {
        const u32 p[] = {
            0xE3A0040E,   /* mov r0,#0x0E000000 */
            0xE3A0105A,   /* mov r1,#0x5A       */
            0xE5C01000,   /* strb r1,[r0]       */
            B_SELF };
        memset(page, 0, sizeof page);
        for (int i = 0; i < 4; i++) put32((u32)i * 4, p[i]);
        memset(g, 0, sizeof *g);
        gba_set_rom(g, ROM_PAGE_SIZE, fault, 0);
        gba_reset(g);
        gba_set_save(g, SAVE_SRAM);
        gba_run_frame(g);
        check("SRAM: strb to 0x0E000000 reaches the save memory", g->sram[0] == 0x5A);
    }

    /* 7. VBlankIntrWait + IRQ: the program counts vertical blanks */
    {
        enum { H = 24 };    /* word index of the IRQ handler (mov pc,lr) */
        u32 p[40];
        int n = 0;
        p[n++] = 0xE3A04403;                      /* mov r4,#0x03000000            */
        p[n++] = 0xE3A02302;                      /* mov r2,#0x08000000            */
        p[n++] = 0xE2822000 | (H * 4);            /* add r2,r2,#H*4                */
        p[n++] = 0xE2843C7F;                      /* add r3,r4,#0x7F00             */
        p[n++] = 0xE28330FC;                      /* add r3,r3,#0xFC               */
        p[n++] = 0xE5832000;                      /* str r2,[r3]  (IRQ vector)     */
        p[n++] = 0xE3A00404;                      /* mov r0,#0x04000000            */
        p[n++] = 0xE3A01008;                      /* mov r1,#8                     */
        p[n++] = 0xE1C010B4;                      /* strh r1,[r0,#4]  DISPSTAT     */
        p[n++] = 0xE3A01001;                      /* mov r1,#1                     */
        p[n++] = 0xE2802C02;                      /* add r2,r0,#0x200              */
        p[n++] = 0xE1C210B0;                      /* strh r1,[r2]   IE             */
        p[n++] = 0xE1C210B8;                      /* strh r1,[r2,#8] IME           */
        int loop = n;
        p[n++] = 0xEF050000;                      /* swi 5 (VBlankIntrWait)        */
        p[n++] = 0xE5941000;                      /* ldr r1,[r4]                   */
        p[n++] = 0xE2811001;                      /* add r1,r1,#1                  */
        p[n++] = 0xE5841000;                      /* str r1,[r4]                   */
        p[n] = 0xEA000000u | ((u32)(loop - (n + 2)) & 0xFFFFFF); n++;
        while (n < H) p[n++] = B_SELF;
        /* user IRQ handler (called with r0 = 0x04000000): acknowledge IF and OR it into the BIOS flags word */
        p[H + 0] = 0xE2802C02;                    /* add r2,r0,#0x200              */
        p[H + 1] = 0xE1D210B2;                    /* ldrh r1,[r2,#2]   IF          */
        p[H + 2] = 0xE1C210B2;                    /* strh r1,[r2,#2]   acknowledge */
        p[H + 3] = 0xE3A03403;                    /* mov r3,#0x03000000            */
        p[H + 4] = 0xE2833C7F;                    /* add r3,r3,#0x7F00             */
        p[H + 5] = 0xE1D30FB8;                    /* ldrh r0,[r3,#0xF8]            */
        p[H + 6] = 0xE1800001;                    /* orr r0,r0,r1                  */
        p[H + 7] = 0xE1C30FB8;                    /* strh r0,[r3,#0xF8]            */
        p[H + 8] = 0xE1A0F00E;                    /* mov pc,lr                     */
        run(p, H + 9, 20);
        u32 v = iw(0);
        printf("       vblank counter after 20 frames: %u\n", v);
        check("IRQ + VBlankIntrWait: about one wake-up per frame", v >= 17 && v <= 20);
    }

    /* 8. save-type detection from library tags in a synthetic ROM head */
    {
        u8 rom[0x800];
        static const struct { const char *tag; int want; } t[] = {
            { "SRAM_V113", SAVE_SRAM }, { "FLASH_V126", SAVE_FLASH64 },
            { "FLASH1M_V103", SAVE_FLASH128 }, { "EEPROM_V124", SAVE_EEPROM } };
        int ok = 1;
        for (unsigned i = 0; i < 4; i++) {
            memset(rom, 0, sizeof rom);
            memcpy(rom + 0x200, t[i].tag, strlen(t[i].tag));
            memset(g, 0, sizeof *g);
            gba_reset(g);
            gba_detect_save(g, rom, sizeof rom);
            if (g->save_type != t[i].want) { ok = 0; printf("       %s -> %d, want %d\n", t[i].tag, g->save_type, t[i].want); }
        }
        check("save detection: SRAM / Flash64 / Flash128 / EEPROM tags", ok);
    }

    printf("SUMMARY: %d checks, %d failed\n", checks, fails);
    free(g);
    return fails ? 1 : 0;
}
