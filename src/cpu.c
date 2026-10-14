/* cpu.c - ARM7TDMI interpreter (ARM + Thumb) */
#include "gba.h"

#ifdef GBA_HLECHECK
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifdef GBA_DEBUG
#include <stdio.h>
int gba_dbg_on, gba_trace_n;
#endif

/* r[15] always holds (address of the executing instruction) + 8 in ARM state
 * and + 4 in Thumb state, which is exactly what instructions observe. */

static u32 cond_mask[16];
static int cond_ready;

static void cond_init(void)
{
    for (int c = 0; c < 16; c++) {
        u32 m = 0;
        for (int f = 0; f < 16; f++) {
            int n = (f >> 3) & 1, z = (f >> 2) & 1, cc = (f >> 1) & 1, v = f & 1, ok = 0;
            switch (c) {
            case 0: ok = z; break;           case 1: ok = !z; break;
            case 2: ok = cc; break;          case 3: ok = !cc; break;
            case 4: ok = n; break;           case 5: ok = !n; break;
            case 6: ok = v; break;           case 7: ok = !v; break;
            case 8: ok = cc && !z; break;    case 9: ok = !cc || z; break;
            case 10: ok = n == v; break;     case 11: ok = n != v; break;
            case 12: ok = !z && n == v; break; case 13: ok = z || n != v; break;
            case 14: ok = 1; break;          default: ok = 0; break;
            }
            if (ok) m |= 1u << f;
        }
        cond_mask[c] = m;
    }
    cond_ready = 1;
}

static const u8 W16[16] = { 1, 1, 3, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 5, 5 };
static const u8 W32[16] = { 1, 1, 6, 1, 1, 2, 2, 1, 6, 6, 6, 6, 6, 6, 5, 5 };

static int mode_idx(u32 m)
{
    switch (m & 0x1F) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0;
    }
}

void set_cpsr(GBA *g, u32 v)
{
    int om = mode_idx(g->cpsr), nm = mode_idx(v);
    if (om != nm) {
        g->bank_sp[om] = g->r[13]; g->bank_lr[om] = g->r[14]; g->bank_spsr[om] = g->spsr;
        if (om == 1) for (int i = 0; i < 5; i++) { g->fiq_r8_12[i] = g->r[8 + i]; g->r[8 + i] = g->usr_r8_12[i]; }
        if (nm == 1) for (int i = 0; i < 5; i++) { g->usr_r8_12[i] = g->r[8 + i]; g->r[8 + i] = g->fiq_r8_12[i]; }
        g->r[13] = g->bank_sp[nm]; g->r[14] = g->bank_lr[nm]; g->spsr = g->bank_spsr[nm];
    }
    g->cpsr = v;
}

/* user-bank register pointer, for LDM/STM with the S bit */
static u32 *ureg(GBA *g, int i)
{
    int m = mode_idx(g->cpsr);
    if (m == 0) return &g->r[i];
    if (m == 1 && i >= 8 && i <= 12) return &g->usr_r8_12[i - 8];
    if (i == 13) return &g->bank_sp[0];
    if (i == 14) return &g->bank_lr[0];
    return &g->r[i];
}

static void exception(GBA *g, u32 mode, u32 vec, u32 lr_val)
{
    u32 old = g->cpsr;
    set_cpsr(g, (old & ~0x3Fu) | mode | PSR_I);
    g->spsr = old;
    g->r[14] = lr_val;
    g->r[15] = vec + 8;
}

void cpu_take_irq(GBA *g)
{
    u32 lr = (g->cpsr & PSR_T) ? g->r[15] : g->r[15] - 4;
    g->halted = 0;
    g->idle = 0; g->idle_n = 0; g->idle_pc = 0;
#ifdef GBA_DEBUG
    if (gba_dbg_on && !gba_trace_n) { gba_trace_n = 260; printf("--- IRQ taken: lr=%08X cpsr=%08X IE=%04X IF=%04X\n", lr, g->cpsr, *(u16 *)&g->io[IO_IE], *(u16 *)&g->io[IO_IF]); }
#endif
    exception(g, MODE_IRQ, 0x18, lr);
}

static inline void branch_arm(GBA *g, u32 a)   { g->r[15] = (a & ~3u) + 8; g->branched = 1; }
static inline void branch_thumb(GBA *g, u32 a) { g->r[15] = (a & ~1u) + 4; g->branched = 1; }

static inline void branch_state(GBA *g, u32 a)
{
    if (g->cpsr & PSR_T) branch_thumb(g, a); else branch_arm(g, a);
}

static inline void bx(GBA *g, u32 a)
{
    if (a & 1) { g->cpsr |= PSR_T; branch_thumb(g, a); }
    else       { g->cpsr &= ~PSR_T; branch_arm(g, a); }
}

static inline void set_nz(GBA *g, u32 v)
{
    g->cpsr = (g->cpsr & 0x3FFFFFFFu) | (v & 0x80000000u) | (v == 0 ? F_Z : 0);
}

static inline u32 ror32(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* Busy-wait detection: a tiny backward loop that repeats with identical register
 * state, no stores and no I/O reads cannot make progress by itself, so the CPU
 * can be fast-forwarded to the next event (an interrupt will end the wait). */
static inline void idle_branch(GBA *g, u32 pc, u32 target)
{
    if (pc - target > 32) return;
    u32 sig = g->cpsr;
    for (int i = 0; i < 13; i++) sig = sig * 31u + g->r[i];
    if (g->idle_pc == pc && g->idle_sig == sig && g->idle_stores == g->store_seq && g->idle_io == g->io_seq) {
        if (++g->idle_n >= 8) { g->idle = 1; g->idle_n = 0; }
    } else {
        g->idle_pc = pc; g->idle_sig = sig; g->idle_stores = g->store_seq; g->idle_io = g->io_seq; g->idle_n = 0;
    }
}

/* ----------------------------------------------------------------- ALU --- */

static u32 alu(GBA *g, u32 opc, u32 a, u32 b, u32 *carry_in_out, int s)
{
    u32 res = 0, c = (g->cpsr & F_C) ? 1 : 0;
    u32 cf = *carry_in_out, vf = (g->cpsr & F_V) ? 1 : 0;
    int arith = 0;
    switch (opc) {
    case 0: case 8:  res = a & b; break;
    case 1: case 9:  res = a ^ b; break;
    case 12: res = a | b; break;
    case 13: res = b; break;
    case 14: res = a & ~b; break;
    case 15: res = ~b; break;
    case 2: case 10: res = a - b; cf = a >= b; vf = ((a ^ b) & (a ^ res)) >> 31; arith = 1; break;
    case 3: res = b - a; cf = b >= a; vf = ((b ^ a) & (b ^ res)) >> 31; arith = 1; break;
    case 4: case 11: res = a + b; cf = res < a; vf = ((a ^ res) & (b ^ res)) >> 31; arith = 1; break;
    case 5: { u64 t = (u64)a + b + c; res = (u32)t; cf = (u32)(t >> 32); vf = ((a ^ res) & (b ^ res)) >> 31; arith = 1; break; }
    case 6: { u64 t = (u64)a - b - (1 - c); res = (u32)t; cf = (u64)a >= (u64)b + (1 - c); vf = ((a ^ b) & (a ^ res)) >> 31; arith = 1; break; }
    case 7: { u64 t = (u64)b - a - (1 - c); res = (u32)t; cf = (u64)b >= (u64)a + (1 - c); vf = ((b ^ a) & (b ^ res)) >> 31; arith = 1; break; }
    }
    (void)arith;
    if (s) {
        g->cpsr = (g->cpsr & 0x0FFFFFFFu) | (res & 0x80000000u) | (res == 0 ? F_Z : 0)
                | (cf ? F_C : 0) | (vf ? F_V : 0);
    }
    return res;
}

/* barrel shifter (immediate or register amount) */
static u32 shifter(GBA *g, u32 v, u32 type, u32 amt, int by_reg, u32 *carry)
{
    u32 c = *carry;
    (void)g;
    switch (type) {
    case 0: /* LSL */
        if (amt == 0) break;
        if (amt < 32) { c = (v >> (32 - amt)) & 1; v <<= amt; }
        else if (amt == 32) { c = v & 1; v = 0; }
        else { c = 0; v = 0; }
        break;
    case 1: /* LSR */
        if (amt == 0) { if (by_reg) break; amt = 32; }
        if (amt < 32) { c = (v >> (amt - 1)) & 1; v >>= amt; }
        else if (amt == 32) { c = v >> 31; v = 0; }
        else { c = 0; v = 0; }
        break;
    case 2: /* ASR */
        if (amt == 0) { if (by_reg) break; amt = 32; }
        if (amt < 32) { c = (v >> (amt - 1)) & 1; v = (u32)((s32)v >> amt); }
        else { c = v >> 31; v = (u32)((s32)v >> 31); }
        break;
    default: /* ROR / RRX */
        if (amt == 0) {
            if (by_reg) break;
            c = v & 1; v = (v >> 1) | ((u32)(g->cpsr & F_C ? 1 : 0) << 31);
        } else {
            amt &= 31;
            if (amt == 0) { c = v >> 31; }
            else { c = (v >> (amt - 1)) & 1; v = ror32(v, amt); }
        }
        break;
    }
    *carry = c;
    return v;
}

/* ------------------------------------------------------------ ARM state --- */

static void arm_dp(GBA *g, u32 op)
{
    u32 opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    u32 carry = (g->cpsr & F_C) ? 1 : 0, b;
    int regshift = !(op & (1u << 25)) && (op & 0x10);
    if (op & (1u << 25)) {
        u32 rot = ((op >> 8) & 15) * 2;
        b = ror32(op & 0xFF, rot);
        if (rot) carry = b >> 31;
    } else {
        u32 v = g->r[op & 15];
        if (regshift) {
            if ((op & 15) == 15) v += 4;
            b = shifter(g, v, (op >> 5) & 3, g->r[(op >> 8) & 15] & 0xFF, 1, &carry);
            g->cycles += 1;
        } else {
            b = shifter(g, v, (op >> 5) & 3, (op >> 7) & 31, 0, &carry);
        }
    }
    u32 a = g->r[rn];
    if (rn == 15 && regshift) a += 4;

    int test = (opc >= 8 && opc <= 11);
    if (test && !s) return;                                 /* handled by PSR transfer path */
    int to_pc = (rd == 15 && !test);
    u32 res = alu(g, opc, a, b, &carry, s && !to_pc);
    if (test) {
        if (rd == 15) set_cpsr(g, g->spsr);     /* TSTP/TEQP/CMPP/CMNP form: restore CPSR from SPSR */
        return;
    }
    if (to_pc) {
        if (s) {
            u32 sp = g->spsr;
            set_cpsr(g, sp);
        }
        branch_state(g, res);
        g->cycles += 2;
    } else g->r[rd] = res;
}

static void arm_psr(GBA *g, u32 op)
{
    int sp = (op >> 22) & 1;
    if (!(op & (1u << 21))) {                                /* MRS */
        g->r[(op >> 12) & 15] = sp ? g->spsr : g->cpsr;
        return;
    }
    u32 val = (op & (1u << 25)) ? ror32(op & 0xFF, ((op >> 8) & 15) * 2) : g->r[op & 15];
    u32 mask = 0;
    if (op & (1u << 19)) mask |= 0xFF000000u;
    if (op & (1u << 18)) mask |= 0x00FF0000u;
    if (op & (1u << 17)) mask |= 0x0000FF00u;
    if (op & (1u << 16)) mask |= 0x000000FFu;
    if (sp) { g->spsr = (g->spsr & ~mask) | (val & mask); return; }
    if ((g->cpsr & 0x1F) == MODE_USR) mask &= 0xFF000000u;
    mask &= ~PSR_T;
    set_cpsr(g, (g->cpsr & ~mask) | (val & mask));
    irq_update(g);
}

static void arm_mul(GBA *g, u32 op)
{
    u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15, s = (op >> 20) & 1;
    if (!(op & (1u << 23))) {
        u32 res = g->r[rm] * g->r[rs];
        if (op & (1u << 21)) res += g->r[rn];
        g->r[rd] = res;
        if (s) set_nz(g, res);
        g->cycles += 2;
    } else {
        u64 res;
        if (op & (1u << 22)) res = (u64)(s64)((s64)(s32)g->r[rm] * (s64)(s32)g->r[rs]);
        else                 res = (u64)g->r[rm] * (u64)g->r[rs];
        if (op & (1u << 21)) res += ((u64)g->r[rd] << 32) | g->r[rn];
        g->r[rn] = (u32)res; g->r[rd] = (u32)(res >> 32);
        if (s) g->cpsr = (g->cpsr & 0x3FFFFFFFu) | (u32)((res >> 32) & 0x80000000u) | (res == 0 ? F_Z : 0);
        g->cycles += 3;
    }
}

static void arm_swp(GBA *g, u32 op)
{
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    u32 addr = g->r[rn];
    if (op & (1u << 22)) {
        u32 t = mem_read8(g, addr); mem_write8(g, addr, g->r[rm]); g->r[rd] = t;
    } else {
        u32 t = ror32(mem_read32(g, addr), (addr & 3) * 8); mem_write32(g, addr, g->r[rm]); g->r[rd] = t;
    }
    g->cycles += 4;
}

static void arm_hword(GBA *g, u32 op)
{
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    u32 off = (op & (1u << 22)) ? (((op >> 4) & 0xF0) | (op & 15)) : g->r[op & 15];
    u32 base = g->r[rn], addr = base;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, wb = (op >> 21) & 1, ld = (op >> 20) & 1;
    u32 sh = (op >> 5) & 3;
    u32 nb = up ? base + off : base - off;
    if (pre) addr = nb;
    if (ld) {
        u32 v;
        if (sh == 1) v = ror32(mem_read16(g, addr), (addr & 1) * 8);
        else if (sh == 2) v = (u32)(s32)(s8)mem_read8(g, addr);
        else v = (addr & 1) ? (u32)(s32)(s8)mem_read8(g, addr) : (u32)(s32)(s16)mem_read16(g, addr);
        if (!pre || wb) g->r[rn] = nb;
        if (rd == 15) { branch_arm(g, v); g->cycles += 2; } else g->r[rd] = v;
        g->cycles += W16[addr >> 24 & 15] + 1;
    } else {
        u32 v = g->r[rd]; if (rd == 15) v += 4;
        mem_write16(g, addr, v);
        if (!pre || wb) g->r[rn] = nb;
        g->cycles += W16[addr >> 24 & 15];
    }
}

static void arm_sdt(GBA *g, u32 op)
{
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    u32 off;
    if (op & (1u << 25)) {
        u32 carry = (g->cpsr & F_C) ? 1 : 0;
        off = shifter(g, g->r[op & 15], (op >> 5) & 3, (op >> 7) & 31, 0, &carry);
    } else off = op & 0xFFF;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1, wb = (op >> 21) & 1, ld = (op >> 20) & 1;
    u32 base = g->r[rn], addr = base, nb = up ? base + off : base - off;
    if (pre) addr = nb;
    if (ld) {
        u32 v = byte ? mem_read8(g, addr) : ror32(mem_read32(g, addr), (addr & 3) * 8);
        if (!pre || wb) g->r[rn] = nb;
        if (rd == 15) { branch_arm(g, v); g->cycles += 2; } else g->r[rd] = v;
        g->cycles += (byte ? W16 : W32)[addr >> 24 & 15] + 1;
    } else {
        u32 v = g->r[rd]; if (rd == 15) v += 4;
        if (byte) mem_write8(g, addr, v); else mem_write32(g, addr, v);
        if (!pre || wb) g->r[rn] = nb;
        g->cycles += (byte ? W16 : W32)[addr >> 24 & 15];
    }
}

static void arm_block(GBA *g, u32 op)
{
    u32 rn = (op >> 16) & 15, list = op & 0xFFFF;
    int pre = (op >> 24) & 1, up = (op >> 23) & 1, sbit = (op >> 22) & 1, wb = (op >> 21) & 1, ld = (op >> 20) & 1;
    u32 base = g->r[rn];
    int n = 0;
    for (int i = 0; i < 16; i++) if (list & (1u << i)) n++;
    u32 empty = 0;
    if (n == 0) { list = 0x8000; n = 16; empty = 1; }
    u32 total = empty ? 0x40 : (u32)n * 4;
    u32 start, end;
    if (up) { start = pre ? base + 4 : base; end = base + total; }
    else    { start = pre ? base - total : base - total + 4; end = base - total; }
    int user = sbit && !(ld && (list & 0x8000));
    int first = 1;
    u32 addr = start;
    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (ld) {
            u32 v = mem_read32(g, addr);
            if (user) *ureg(g, i) = v; else if (i == 15) { branch_arm(g, v); } else g->r[i] = v;
        } else {
            u32 v = user ? *ureg(g, i) : g->r[i];
            if (i == 15) v += 4;
            if (i == (int)rn && !first && wb) v = end;
            mem_write32(g, addr, v);
        }
        first = 0;
        addr += 4;
    }
    if (ld && sbit && (list & 0x8000)) { set_cpsr(g, g->spsr); branch_state(g, g->r[15] - 8); }
    if (wb && !(ld && (list & (1u << rn)))) g->r[rn] = (u32)(up ? base + total : base - total);
    g->cycles += (u64)n * (W32[start >> 24 & 15]) + 1;
}

static void exec_arm(GBA *g, u32 op)
{
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x0FFFFFF0u) == 0x012FFF10u) { bx(g, g->r[op & 15]); g->cycles += 2; return; }
        if ((op & 0x90) == 0x90) {
            if ((op & 0x60) == 0) {
                if (((op >> 23) & 3) == 2 && (op & 0x0FB00FF0u) == 0x01000090u) { arm_swp(g, op); return; }
                arm_mul(g, op); return;
            }
            arm_hword(g, op); return;
        }
        {
            u32 opc = (op >> 21) & 15;
            if (opc >= 8 && opc <= 11 && !(op & (1u << 20))) { arm_psr(g, op); return; }
        }
        arm_dp(g, op); return;
    case 1: {
        u32 opc = (op >> 21) & 15;
        if (opc >= 8 && opc <= 11 && !(op & (1u << 20))) { arm_psr(g, op); return; }
        arm_dp(g, op); return;
    }
    case 2: arm_sdt(g, op); return;
    case 3:
        if (op & 0x10) return;                              /* undefined */
        arm_sdt(g, op); return;
    case 4: arm_block(g, op); return;
    case 5: {
        s32 off = ((s32)(op << 8)) >> 6;
        if (op & (1u << 24)) g->r[14] = g->r[15] - 4;
        else if (off < 0) idle_branch(g, g->r[15] - 8, g->r[15] + (u32)off);
        branch_arm(g, g->r[15] + (u32)off);
        g->cycles += 2;
        return;
    }
    case 7:
        if (op & (1u << 24)) {
            if (bios_swi(g, (op >> 16) & 0xFF)) g->branched = 1;   /* HLE wait: re-run the SWI */
            g->cycles += 3;
        }
        return;
    default: return;
    }
}

static inline u32 fetch32(GBA *g, u32 pc);

/* ---------------------------------------------- m4a sound mixer HLE --- *
 * The m4a driver's SoundMainRAM (ARM code copied to IWRAM) mixes every PCM
 * channel sample by sample: about half of all instructions in some titles. With no
 * audio output we skip the mixing but advance each channel exactly like the
 * real loop does (position, fraction, end of sample / loop), so the sequencer
 * and anything waiting on channels behaves the same.
 *
 * Per-channel entry (ARM, after the Thumb setup): r2 = samples left, r3 = sample
 * pointer, r4 = channel, r8 = output samples this frame, r12 = pitch scale,
 * [sp+12] = loop start pointer, [sp+16] = loop length (0 = one-shot). */
static int ts_checked, ts_hle;     /* title-specific sprite HLE: per ROM, outside the GBA struct (save-state layout) */
static void ts_detect(GBA *g);
u32 gba_icount;                    /* statistics: instructions executed */

/* bit0 = sound mixer HLE active, bit1 = title-specific sprite HLE active */
u32 gba_hle_status(GBA *g) { return (g->mix_pc ? 1u : 0u) | (ts_hle ? 2u : 0u); }

static const u32 MIX_SIG[6] = { 0xE58D8000, 0xE594901C, 0xE5D4A00A, 0xE5D4B00B, 0xE5D40001, 0xE3100030 };
static const u32 REV_SIG[4] = { 0xE3540002, 0x02807E35, 0x10857008, 0xE1A04008 };
#define MIX_EXIT 0x1F4          /* "add r0, pc, #1 / bx r0" back to Thumb */
#define REV_EXIT 0x04C

static int sig_at(GBA *g, u32 off, const u32 *sig, int n)
{
    for (int i = 0; i < n; i++)
        if (*(const u32 *)&g->iwram[(off + 4u * i) & 0x7FFC] != sig[i]) return 0;
    return 1;
}

void sound_hle_scan(GBA *g)
{
    if (!g->sound_hle) { g->mix_pc = g->rev_pc = 0; return; }
    if (!ts_checked) ts_detect(g);
    if (g->mix_pc && !sig_at(g, g->mix_pc & 0x7FFF, MIX_SIG, 6)) g->mix_pc = 0;
    if (g->rev_pc && !sig_at(g, g->rev_pc & 0x7FFF, REV_SIG, 4)) g->rev_pc = 0;
    if (g->mix_pc && g->rev_pc) return;
    for (u32 off = 0; off + 0x200 < 0x8000; off += 4) {
        if (!g->mix_pc && sig_at(g, off, MIX_SIG, 6) &&
            *(const u32 *)&g->iwram[off + MIX_EXIT] == 0xE28F0001) g->mix_pc = 0x03000000 + off;
        if (!g->rev_pc && sig_at(g, off, REV_SIG, 4)) g->rev_pc = 0x03000000 + off;
    }
}

static void hle_mix(GBA *g, u32 pc)
{
    u32 ch = g->r[4], n = g->r[8], ptr = g->r[3];
    s32 left = (s32)g->r[2];
    u32 type = mem_read8(g, ch + 1);
    if (type & 0x30) { g->r[15] = pc + 8; return; }        /* reverse / compressed: not handled, run it */
    u32 fw = mem_read32(g, ch + 28), adv;
    if (type & 8) { adv = n; g->cycles += 10u * n; }       /* fixed frequency: one source sample per output */
    else {
        u32 step = g->r[12] * mem_read32(g, ch + 32);
        u64 tot = (u64)fw + (u64)step * n;
        adv = (u32)(tot >> 23);
        fw = (u32)tot & 0x7FFFFF;
        g->cycles += 17u * n;
    }
    left -= (s32)adv;
    if (left > 0) ptr += adv;
    else {
        u32 sp = g->r[13];
        u32 loop_len = mem_read32(g, sp + 16), loop_start = mem_read32(g, sp + 12);
        if (!loop_len) {                                     /* one-shot sample ended: channel off */
            mem_write8(g, ch, 0);
            g->r[15] = pc + MIX_EXIT + 8;
            return;
        }
        while (left <= 0) left += (s32)loop_len;
        ptr = loop_start + (loop_len - (u32)left);
    }
    mem_write32(g, ch + 28, fw);
    mem_write32(g, ch + 24, (u32)left);
    mem_write32(g, ch + 40, ptr);
    g->r[15] = pc + MIX_EXIT + 8;
}

/* ------------------------------------- Title-specific sprite speedup (HLE), selected by the ROM game code --- *
 * BuildOamBuffer calls three pure routines every frame that take ~1/3 of the
 * game's CPU time: UpdateOamCoords, BuildSpritePriorities and SortSprites
 * (insertion sort of 64 sprites). They are replaced by native code that does
 * exactly what the compiled Thumb does (same truncations), checked against the
 * real code with GBA_HLECHECK on the host. */
#define TS_UPDATE_OAM  0x08006BF4u
#define TS_BUILD_PRIO  0x08006CB8u
#define TS_SORT        0x08006CF8u
#define TS_SPRITES     0x2063Cu          /* EWRAM offsets */
#define TS_PRIOS       0x21780u
#define TS_ORDER       0x21800u
#define TS_OFS_X       0x21BC8u
#define TS_OFS_Y       0x21BCAu
#define SPR(i)         (TS_SPRITES + (u32)(i) * 0x44u)

static inline u16 ew16(const u8 *ew, u32 o) { return *(const u16 *)&ew[o & 0x3FFFE]; }
static inline u32 ew32(const u8 *ew, u32 o) { return *(const u32 *)&ew[o & 0x3FFFC]; }

static void ts_update_oam(u8 *ew)
{
    for (u32 i = 0; i < 64; i++) {
        u32 s = SPR(i), fl = ew[s + 0x3E];
        if ((fl & 5) != 1) continue;                       /* inUse && !invisible */
        u32 x = (u32)((s32)(s16)ew16(ew, s + 0x20) + (s16)ew16(ew, s + 0x24) + (s8)ew[s + 0x28]);
        u32 y = (u32)ew16(ew, s + 0x26) + ew16(ew, s + 0x22) + ew[s + 0x29];
        if (fl & 2) { x += (u32)(s16)ew16(ew, TS_OFS_X); y += ew[TS_OFS_Y]; }
        *(u16 *)&ew[s + 2] = (u16)((ew16(ew, s + 2) & 0xFE00) | (x & 0x1FF));
        ew[s] = (u8)y;
    }
}

static void ts_build_prio(u8 *ew)
{
    for (u32 i = 0; i < 64; i++) {
        u32 s = SPR(i);
        *(u16 *)&ew[TS_PRIOS + i * 2] = (u16)(ew[s + 0x43] | (((ew[s + 5] >> 2) & 3) << 8));
    }
}

static inline s32 ts_sort_y(const u8 *ew, u32 idx)
{
    u32 s = SPR(idx), r = ew[s];
    if (r > 159) r = (r - 256) & 0xFFFF;
    if ((ew32(ew, s) & 0xC0000300u) == 0xC0000300u) {      /* affine double + size 3 */
        u32 shape = ew[s + 1] >> 6;
        if ((shape == 0 || shape == 2) && (s16)r > 128) r = (r - 256) & 0xFFFF;
    }
    return (s16)r;
}

static void ts_sort(u8 *ew)
{
    u8 *ord = &ew[TS_ORDER];
    const u16 *pr = (const u16 *)&ew[TS_PRIOS];
    for (u32 i = 1; i < 64; i++) {
        u32 j = i;
        while (j > 0) {
            u32 a = ord[j - 1], b = ord[j];
            u32 pa = pr[a], pb = pr[b];
            if (!(pa > pb || (pa == pb && ts_sort_y(ew, a) < ts_sort_y(ew, b)))) break;
            ord[j] = (u8)a; ord[j - 1] = (u8)b;
            j--;
        }
    }
}


/* once: is this the one known title (game code + revision) with the expected code at the entry points? */
static void ts_detect(GBA *g)
{
    ts_checked = 1;
    ts_hle = 0;
    if (mem_read32(g, 0x080000AC) != 0x45525042u || mem_read8(g, 0x080000BC) != 0) return;   /* known game code, revision 0 */
    if (mem_read32(g, TS_UPDATE_OAM) != 0x2400B5F0u || mem_read32(g, TS_BUILD_PRIO) != 0x2200B510u ||
        mem_read32(g, TS_SORT) != 0x4657B5F0u) return;
    ts_hle = 1;
}

#ifdef GBA_HLECHECK
static u8 *chk_shadow;
static u32 chk_ret, chk_sp, chk_fn, chk_n, chk_bad;
static void chk_compare(GBA *g)
{
    static const u32 rng[3][2] = { { TS_SPRITES, 64 * 0x44 }, { TS_PRIOS, 128 }, { TS_ORDER, 64 } };
    int bad = 0;
    for (int k = 0; k < 3; k++)
        for (u32 o = rng[k][0]; o < rng[k][0] + rng[k][1]; o++)
            if (g->ewram[o] != chk_shadow[o]) { bad = 1; break; }
    chk_n++;
    if (bad && chk_bad++ < 5) printf("HLECHECK mismatch in fn %08X (call %u)\n", chk_fn, chk_n);
    chk_ret = 0;
}
void hlecheck_report(void) { printf("HLECHECK %u calls compared, %u mismatches\n", chk_n, chk_bad); }
#endif

/* Thumb entry hook: returns 1 when the routine was run natively */
static int ts_hook(GBA *g, u32 pc)
{
    void (*fn)(u8 *) = pc == TS_UPDATE_OAM ? ts_update_oam : pc == TS_BUILD_PRIO ? ts_build_prio :
                       pc == TS_SORT ? ts_sort : 0;
    if (!fn) return 0;
#ifdef GBA_HLECHECK
    if (!chk_shadow) chk_shadow = malloc(0x40000);
    memcpy(chk_shadow, g->ewram, 0x40000);
    fn(chk_shadow);
    chk_ret = g->r[14] & ~1u; chk_sp = g->r[13]; chk_fn = pc;
    return 0;                                   /* let the real code run, compare on return */
#else
    fn(g->ewram);
    g->store_seq++;
    g->cycles += pc == TS_SORT ? 900 : 600;
    u32 lr = g->r[14];                          /* bx lr */
    if (lr & 1) g->r[15] = (lr & ~1u) + 4;
    else { set_cpsr(g, g->cpsr & ~PSR_T); g->r[15] = (lr & ~3u) + 8; }
    return 1;
#endif
}

static void step_arm(GBA *g)
{
    u32 pc = g->r[15] - 8;
    if ((pc >> 24) == 3 && g->sound_hle) {
        if (pc == g->mix_pc) { hle_mix(g, pc); if (g->r[15] != pc + 8) return; }
        else if (pc == g->rev_pc) { g->cycles += 12u * g->r[8]; g->r[15] = pc + REV_EXIT + 8; return; }
    }
    u32 op = fetch32(g, pc);
#ifdef GBA_DEBUG
    if (gba_trace_n > 0) { printf("A %08X %08X r0=%08X r1=%08X r2=%08X r3=%08X sp=%08X cpsr=%08X\n", pc, op, g->r[0], g->r[1], g->r[2], g->r[3], g->r[13], g->cpsr); gba_trace_n--; }
#endif
    g->cycles += W32[pc >> 24 & 15];
    if ((cond_mask[op >> 28] >> (g->cpsr >> 28)) & 1) exec_arm(g, op);
    if (g->branched) g->branched = 0; else g->r[15] += 4;
}

/* ---------------------------------------------------------- Thumb state --- */

static void thumb_alu(GBA *g, u32 op)
{
    u32 rd = op & 7, rs = (op >> 3) & 7, o = (op >> 6) & 15;
    u32 a = g->r[rd], b = g->r[rs], carry = (g->cpsr & F_C) ? 1 : 0, res;
    switch (o) {
    case 0: res = a & b; set_nz(g, res); g->r[rd] = res; return;
    case 1: res = a ^ b; set_nz(g, res); g->r[rd] = res; return;
    case 2: res = shifter(g, a, 0, b & 0xFF, 1, &carry); g->cpsr = (g->cpsr & ~F_C) | (carry ? F_C : 0); set_nz(g, res); g->r[rd] = res; g->cycles++; return;
    case 3: res = shifter(g, a, 1, b & 0xFF, 1, &carry); g->cpsr = (g->cpsr & ~F_C) | (carry ? F_C : 0); set_nz(g, res); g->r[rd] = res; g->cycles++; return;
    case 4: res = shifter(g, a, 2, b & 0xFF, 1, &carry); g->cpsr = (g->cpsr & ~F_C) | (carry ? F_C : 0); set_nz(g, res); g->r[rd] = res; g->cycles++; return;
    case 5: g->r[rd] = alu(g, 5, a, b, &carry, 1); return;
    case 6: g->r[rd] = alu(g, 6, a, b, &carry, 1); return;
    case 7: res = shifter(g, a, 3, b & 0xFF, 1, &carry); g->cpsr = (g->cpsr & ~F_C) | (carry ? F_C : 0); set_nz(g, res); g->r[rd] = res; g->cycles++; return;
    case 8: res = a & b; set_nz(g, res); return;
    case 9: g->r[rd] = alu(g, 3, b, 0, &carry, 1); return;   /* NEG: 0 - b */
    case 10: alu(g, 10, a, b, &carry, 1); return;
    case 11: alu(g, 11, a, b, &carry, 1); return;
    case 12: res = a | b; set_nz(g, res); g->r[rd] = res; return;
    case 13: res = a * b; set_nz(g, res); g->r[rd] = res; g->cycles += 2; return;
    case 14: res = a & ~b; set_nz(g, res); g->r[rd] = res; return;
    default: res = ~b; set_nz(g, res); g->r[rd] = res; return;
    }
}

static void exec_thumb(GBA *g, u32 op)
{
    /* one dense switch on the top byte -> a single jump-table dispatch */
    switch (op >> 8) {
    case 0x00 ... 0x1F:
        if (((op >> 11) & 3) != 3) {                         /* format 1 */
            u32 rd = op & 7, rs = (op >> 3) & 7, amt = (op >> 6) & 31, carry = (g->cpsr & F_C) ? 1 : 0;
            u32 v = shifter(g, g->r[rs], (op >> 11) & 3, amt, 0, &carry);
            g->cpsr = (g->cpsr & ~F_C) | (carry ? F_C : 0);
            set_nz(g, v); g->r[rd] = v;
        } else {                                              /* format 2 */
            u32 rd = op & 7, a = g->r[(op >> 3) & 7], b = (op & (1u << 10)) ? ((op >> 6) & 7) : g->r[(op >> 6) & 7], c = 0;
            g->r[rd] = alu(g, (op & (1u << 9)) ? 2 : 4, a, b, &c, 1);
        }
        return;
    case 0x20 ... 0x3F: {                                     /* format 3 */
        u32 rd = (op >> 8) & 7, imm = op & 0xFF, c = (g->cpsr & F_C) ? 1 : 0;
        switch ((op >> 11) & 3) {
        case 0: set_nz(g, imm); g->r[rd] = imm; break;
        case 1: alu(g, 10, g->r[rd], imm, &c, 1); break;
        case 2: g->r[rd] = alu(g, 4, g->r[rd], imm, &c, 1); break;
        default: g->r[rd] = alu(g, 2, g->r[rd], imm, &c, 1); break;
        }
        return;
    }
    case 0x40 ... 0x43:                                       /* format 4 */
        thumb_alu(g, op);
        return;
    case 0x44 ... 0x5F:
        if (!(op & (1u << 12))) {
            if (!(op & (1u << 11))) {
                /* format 5: hi register ops / BX */
                u32 rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15, c = 0;
                u32 b = g->r[rs];
                switch ((op >> 8) & 3) {
                case 0: { u32 r = g->r[rd] + b; if (rd == 15) { branch_thumb(g, r); g->cycles += 2; } else g->r[rd] = r; break; }
                case 1: alu(g, 10, g->r[rd], b, &c, 1); break;
                case 2: if (rd == 15) { branch_thumb(g, b); g->cycles += 2; } else g->r[rd] = b; break;
                default: bx(g, b); g->cycles += 2; break;
                }
                return;
            }
            /* format 6: PC-relative load */
            u32 addr = (g->r[15] & ~3u) + ((op & 0xFF) << 2);
            g->r[(op >> 8) & 7] = mem_read32(g, addr);
            g->cycles += W32[addr >> 24 & 15] + 1;
            return;
        }
        {
            u32 rd = op & 7, addr = g->r[(op >> 3) & 7] + g->r[(op >> 6) & 7];
            if (!(op & (1u << 9))) {                          /* format 7 */
                u32 l = (op >> 11) & 1, b = (op >> 10) & 1;
                if (l) g->r[rd] = b ? mem_read8(g, addr) : ror32(mem_read32(g, addr), (addr & 3) * 8);
                else if (b) mem_write8(g, addr, g->r[rd]); else mem_write32(g, addr, g->r[rd]);
                g->cycles += (b ? W16 : W32)[addr >> 24 & 15] + (l ? 1 : 0);
            } else {                                          /* format 8 */
                switch ((op >> 10) & 3) {
                case 0: mem_write16(g, addr, g->r[rd]); break;
                case 1: g->r[rd] = (u32)(s32)(s8)mem_read8(g, addr); break;
                case 2: g->r[rd] = ror32(mem_read16(g, addr), (addr & 1) * 8); break;
                default: g->r[rd] = (addr & 1) ? (u32)(s32)(s8)mem_read8(g, addr) : (u32)(s32)(s16)mem_read16(g, addr); break;
                }
                g->cycles += W16[addr >> 24 & 15] + (((op >> 10) & 3) ? 1 : 0);
            }
        }
        return;
    case 0x60 ... 0x7F: {                                     /* format 9 */
        u32 rd = op & 7, base = g->r[(op >> 3) & 7], off = (op >> 6) & 31;
        u32 b = (op >> 12) & 1, l = (op >> 11) & 1, addr = base + (b ? off : off << 2);
        if (l) g->r[rd] = b ? mem_read8(g, addr) : ror32(mem_read32(g, addr), (addr & 3) * 8);
        else if (b) mem_write8(g, addr, g->r[rd]); else mem_write32(g, addr, g->r[rd]);
        g->cycles += (b ? W16 : W32)[addr >> 24 & 15] + (l ? 1 : 0);
        return;
    }
    case 0x80 ... 0x9F:
        if (!(op & (1u << 12))) {                             /* format 10 */
            u32 rd = op & 7, addr = g->r[(op >> 3) & 7] + (((op >> 6) & 31) << 1);
            if (op & (1u << 11)) g->r[rd] = ror32(mem_read16(g, addr), (addr & 1) * 8);
            else mem_write16(g, addr, g->r[rd]);
            g->cycles += W16[addr >> 24 & 15] + ((op >> 11) & 1);
        } else {                                              /* format 11 */
            u32 rd = (op >> 8) & 7, addr = g->r[13] + ((op & 0xFF) << 2);
            if (op & (1u << 11)) g->r[rd] = ror32(mem_read32(g, addr), (addr & 3) * 8);
            else mem_write32(g, addr, g->r[rd]);
            g->cycles += W32[addr >> 24 & 15] + ((op >> 11) & 1);
        }
        return;
    case 0xA0 ... 0xBF:
        if (!(op & (1u << 12))) {                             /* format 12 */
            u32 rd = (op >> 8) & 7, imm = (op & 0xFF) << 2;
            g->r[rd] = (op & (1u << 11)) ? g->r[13] + imm : (g->r[15] & ~3u) + imm;
            return;
        }
        if (((op >> 8) & 0xF) == 0) {                         /* format 13 */
            u32 imm = (op & 0x7F) << 2;
            g->r[13] = (op & 0x80) ? g->r[13] - imm : g->r[13] + imm;
            return;
        }
        if (((op >> 9) & 3) == 2) {                           /* format 14: push / pop */
            u32 list = op & 0xFF, sp = g->r[13];
            int extra = (op >> 8) & 1, n = extra;
            for (int i = 0; i < 8; i++) if (list & (1u << i)) n++;
            if (!(op & (1u << 11))) {                         /* PUSH */
                sp -= (u32)n * 4;
                u32 a = sp;
                for (int i = 0; i < 8; i++) if (list & (1u << i)) { mem_write32(g, a, g->r[i]); a += 4; }
                if (extra) mem_write32(g, a, g->r[14]);
                g->r[13] = sp;
            } else {                                          /* POP */
                u32 a = sp;
                for (int i = 0; i < 8; i++) if (list & (1u << i)) { g->r[i] = mem_read32(g, a); a += 4; }
                if (extra) { u32 v = mem_read32(g, a); a += 4; branch_thumb(g, v); g->cycles += 2; }
                g->r[13] = a;
            }
            g->cycles += (u64)n * W32[sp >> 24 & 15] + 1;
            return;
        }
        return;
    case 0xC0 ... 0xDF:
        if (!(op & (1u << 12))) {                             /* format 15 */
            u32 rb = (op >> 8) & 7, list = op & 0xFF, base = g->r[rb], a = base;
            int n = 0;
            for (int i = 0; i < 8; i++) if (list & (1u << i)) n++;
            if (n == 0) {                                     /* empty list quirk */
                if (op & (1u << 11)) { branch_thumb(g, mem_read32(g, a)); }
                else mem_write32(g, a, g->r[15] + 2);
                g->r[rb] = base + 0x40;
                return;
            }
            int first = 1;
            for (int i = 0; i < 8; i++) {
                if (!(list & (1u << i))) continue;
                if (op & (1u << 11)) g->r[i] = mem_read32(g, a);
                else mem_write32(g, a, (i == (int)rb && !first) ? base + (u32)n * 4 : g->r[i]);
                first = 0; a += 4;
            }
            if (!((op & (1u << 11)) && (list & (1u << rb)))) g->r[rb] = base + (u32)n * 4;
            g->cycles += (u64)n * W32[base >> 24 & 15] + 1;
            return;
        }
        {
            u32 cond = (op >> 8) & 15;
            if (cond == 15) {                                 /* SWI */
                if (bios_swi(g, op & 0xFF)) g->branched = 1;   /* HLE wait: re-run the SWI */
                g->cycles += 3;
                return;
            }
            if (cond == 14) return;                           /* undefined */
            if ((cond_mask[cond] >> (g->cpsr >> 28)) & 1) {
                u32 tgt = g->r[15] + (u32)((s32)(s8)(op & 0xFF) << 1);
                if (tgt < g->r[15]) idle_branch(g, g->r[15] - 4, tgt);
                branch_thumb(g, tgt);
                g->cycles += 2;
            }
        }
        return;
    default:
        if (!(op & (1u << 12))) {
            if (!(op & (1u << 11))) {                         /* format 18: B */
                s32 off = ((s32)(op << 21)) >> 20;
                if (off < 0) idle_branch(g, g->r[15] - 4, g->r[15] + (u32)off);
                branch_thumb(g, g->r[15] + (u32)off);
                g->cycles += 2;
                return;
            }
            return;
        }
        /* format 19: long branch with link */
        if (!(op & (1u << 11))) {
            s32 off = ((s32)(op << 21)) >> 9;                 /* 11 high bits << 12, sign-extended */
            g->r[14] = g->r[15] + (u32)off;
        } else {
            u32 t = g->r[14] + ((op & 0x7FF) << 1);
            g->r[14] = (g->r[15] - 2) | 1;
            branch_thumb(g, t);
            g->cycles += 2;
        }
        return;
    }
}

/* instruction fetch: direct paths for ROM pages, IWRAM and EWRAM */
static inline u32 fetch16(GBA *g, u32 pc)
{
    u32 r = pc >> 24;
    if (r >= 0x08 && r <= 0x0D) {
        u32 o = pc & 0x1FFFFFE;
        if (o < g->rom_size) {
            u32 pg = o >> ROM_PAGE_SHIFT;
            const u8 *p = g->rom_tab[pg];
            if (!p) p = g->rom_fault(g, pg);
            g->rom_ref[pg] = 1;
            return *(const u16 *)(p + (o & (ROM_PAGE_SIZE - 1)));
        }
    } else if (r == 0x03) return *(const u16 *)&g->iwram[pc & 0x7FFE];
    else if (r == 0x02) return *(const u16 *)&g->ewram[pc & 0x3FFFE];
    return mem_read16(g, pc);
}

static inline u32 fetch32(GBA *g, u32 pc)
{
    u32 r = pc >> 24;
    if (r >= 0x08 && r <= 0x0D) {
        u32 o = pc & 0x1FFFFFC;
        if (o < g->rom_size) {
            u32 pg = o >> ROM_PAGE_SHIFT;
            const u8 *p = g->rom_tab[pg];
            if (!p) p = g->rom_fault(g, pg);
            g->rom_ref[pg] = 1;
            return *(const u32 *)(p + (o & (ROM_PAGE_SIZE - 1)));
        }
    } else if (r == 0x03) return *(const u32 *)&g->iwram[pc & 0x7FFC];
    else if (r == 0x02) return *(const u32 *)&g->ewram[pc & 0x3FFFC];
    return mem_read32(g, pc);
}

static void step_thumb(GBA *g)
{
    u32 pc = g->r[15] - 4;
#ifdef GBA_HLECHECK
    if (chk_ret && pc == chk_ret && g->r[13] == chk_sp) chk_compare(g);
#endif
    if ((pc & 0xFFFFF000u) == 0x08006000u && ts_hle && g->sound_hle && ts_hook(g, pc)) return;
    u32 op = fetch16(g, pc);
#ifdef GBA_DEBUG
    if (gba_trace_n > 0) { printf("T %08X %04X r0=%08X r1=%08X r2=%08X r3=%08X sp=%08X cpsr=%08X\n", pc, op, g->r[0], g->r[1], g->r[2], g->r[3], g->r[13], g->cpsr); gba_trace_n--; }
#endif
    g->cycles += W16[pc >> 24 & 15];
    exec_thumb(g, op);
    if (g->branched) g->branched = 0; else g->r[15] += 2;
}

/* -------------------------------------------------------------- run loop --- */

void cpu_run(GBA *g, u64 until)
{
    if (!cond_ready) cond_init();
    while (g->cycles < until) {
        if (g->irq_line && !(g->cpsr & PSR_I)) cpu_take_irq(g);
        if (g->halted || g->idle) { g->halt_cycles += until - g->cycles; g->cycles = until; return; }
        gba_icount++;
#ifdef GBA_PROF
        {   /* instruction histogram per 64-byte block: 02=EWRAM 03=IWRAM 08=ROM */
            extern u32 *gba_prof;
            u32 pc = g->r[15] - ((g->cpsr & PSR_T) ? 4 : 8), reg = pc >> 24;
            u32 ri = reg == 2 ? 0 : reg == 3 ? 1 : reg == 8 ? 2 : 3;
            gba_prof[(ri << 18) | ((pc & 0xFFFFFF) >> 6)]++;
        }
#endif
        if (g->cpsr & PSR_T) step_thumb(g); else step_arm(g);
    }
}
