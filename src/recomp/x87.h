/*
 * x87.h - host x87 for lifted code (the helpers pcrecomp's lift16 emits).
 *
 * ON.EXE's float code is Borland 8087-emulator encoded (CD 34..3D); the lift
 * normalises it to real ESC opcodes and calls these. Eight doubles, TOP, a
 * status word and a control word. 80-bit values are held as double: enough
 * for a 1991 game's float use, and not bit-exact with a real 8087.
 *
 * Needs mem_read8/mem_write8(cpu, seg, off) from cpu.h.
 */
#ifndef X87_H
#define X87_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { double st[8]; int top; uint16_t sw, cw; } X87State;
#ifdef X87_SHARED
extern X87State g_x87;      /* one FP stack for every generated chunk (defined in runtime16.c) */
#else
static X87State g_x87 = { {0}, 0, 0, 0x037F };   /* single-TU host test */
#endif

#define X87_ST(i)  (g_x87.st[(g_x87.top + (i)) & 7])
#define X87_C0 0x0100
#define X87_C1 0x0200
#define X87_C2 0x0400
#define X87_C3 0x4000

static inline void x87_push(double v) { g_x87.top = (g_x87.top - 1) & 7; g_x87.st[g_x87.top] = v; }
static inline void x87_pop(void) { g_x87.top = (g_x87.top + 1) & 7; }
static inline uint16_t x87_sw(void) { return (uint16_t)((g_x87.sw & ~0x3800) | (g_x87.top << 11)); }

/* reg field of the arithmetic group: 0 add, 1 mul, 4 sub (dst-v), 5 subr (v-dst),
 * 6 div (dst/v), 7 divr (v/dst). */
static inline void x87_arith(int op, double *dst, double v)
{
    switch (op) {
    case 0: *dst = *dst + v; break;
    case 1: *dst = *dst * v; break;
    case 4: *dst = *dst - v; break;
    case 5: *dst = v - *dst; break;
    case 6: *dst = *dst / v; break;
    case 7: *dst = v / *dst; break;
    }
}

static inline void x87_cmp(double a, double b)
{
    g_x87.sw &= (uint16_t)~(X87_C0 | X87_C2 | X87_C3);
    if (isnan(a) || isnan(b)) g_x87.sw |= X87_C0 | X87_C2 | X87_C3;
    else if (a < b)  g_x87.sw |= X87_C0;
    else if (a == b) g_x87.sw |= X87_C3;
}

static inline void x87_fxam(void)
{
    double v = X87_ST(0);
    g_x87.sw &= (uint16_t)~(X87_C0 | X87_C1 | X87_C2 | X87_C3);
    if (signbit(v)) g_x87.sw |= X87_C1;
    switch (fpclassify(v)) {
    case FP_NAN:       g_x87.sw |= X87_C0; break;
    case FP_INFINITE:  g_x87.sw |= X87_C2 | X87_C0; break;
    case FP_ZERO:      g_x87.sw |= X87_C3; break;
    case FP_SUBNORMAL: g_x87.sw |= X87_C3 | X87_C2; break;
    default:           g_x87.sw |= X87_C2; break;
    }
}

static inline void x87_unhandled(const char *bytes)
{
    static int seen;                       /* may be data lifted as code: log, carry on */
    if (seen++ < 16) fprintf(stderr, "x87: unhandled opcode %s\n", bytes);
}

/* Round to integer under the control word's RC field. */
static inline double x87_round(double v)
{
    switch ((g_x87.cw >> 10) & 3) {
    case 1:  return floor(v);
    case 2:  return ceil(v);
    case 3:  return trunc(v);
    default: return nearbyint(v);          /* ties to even in the default mode */
    }
}

/* kind: 4 float, 8 double, 10 extended; -2/-4/-8 signed integers */
#define X87_RD8(i)  ((uint64_t)mem_read8(cpu, seg, (uint16_t)(off + (i))))
static inline double x87_rd(CPU *cpu, uint16_t seg, uint16_t off, int kind)
{
    uint64_t u = 0; int n = kind < 0 ? -kind : (kind == 10 ? 10 : kind);
    for (int i = 0; i < (n > 8 ? 8 : n); i++) u |= X87_RD8(i) << (8 * i);
    switch (kind) {
    case 4:  { uint32_t w = (uint32_t)u; float f; memcpy(&f, &w, 4); return f; }
    case 8:  { double d; memcpy(&d, &u, 8); return d; }
    case 10: {
        int e = (int)(X87_RD8(8) | (X87_RD8(9) << 8));
        int neg = e >> 15; e &= 0x7FFF;
        double m = (double)u;              /* 64-bit mantissa, explicit integer bit */
        double r = (e == 0 && u == 0) ? 0.0
                 : e == 0x7FFF ? ((u << 1) ? NAN : INFINITY)
                 : ldexp(m, e - 16383 - 63);
        return neg ? -r : r;
    }
    case -2: return (double)(int16_t)u;
    case -4: return (double)(int32_t)u;
    default: return (double)(int64_t)u;
    }
}

static inline void x87_wr(CPU *cpu, uint16_t seg, uint16_t off, int kind, double v)
{
    uint64_t u = 0; int n = 0;
    switch (kind) {
    case 4:  { float f = (float)v; uint32_t w; memcpy(&w, &f, 4); u = w; n = 4; break; }
    case 8:  { memcpy(&u, &v, 8); n = 8; break; }
    case 10: {
        uint64_t mant = 0; int e = 0, neg = signbit(v) != 0; double a = fabs(v);
        if (isnan(v))      { e = 0x7FFF; mant = 0xC000000000000000ull; }
        else if (isinf(v)) { e = 0x7FFF; mant = 0x8000000000000000ull; }
        else if (a != 0)   { int ex; double f = frexp(a, &ex);     /* f in [0.5,1) */
                             mant = (uint64_t)ldexp(f, 64); e = ex - 1 + 16383; }
        for (int i = 0; i < 8; i++) mem_write8(cpu, seg, (uint16_t)(off + i), (uint8_t)(mant >> (8 * i)));
        e |= neg << 15;
        mem_write8(cpu, seg, (uint16_t)(off + 8), (uint8_t)e);
        mem_write8(cpu, seg, (uint16_t)(off + 9), (uint8_t)(e >> 8));
        return;
    }
    default: {                             /* integer store: round per RC, "indefinite" on overflow */
        double r = x87_round(v); int bits = -kind * 8;
        double lim = ldexp(1.0, bits - 1);
        int64_t iv = (isnan(r) || r >= lim || r < -lim)
                   ? (bits == 16 ? INT16_MIN : bits == 32 ? INT32_MIN : INT64_MIN)
                   : (int64_t)r;
        u = (uint64_t)iv; n = -kind; break;
    }
    }
    for (int i = 0; i < n; i++) mem_write8(cpu, seg, (uint16_t)(off + i), (uint8_t)(u >> (8 * i)));
}
/* Packed BCD (fbld / fbstp): 18 digits in bytes 0..8 (low digit = low nibble
 * of byte 0), sign in bit 7 of byte 9. fbstp rounds per the control word. */
static inline double x87_bcd_rd(CPU *cpu, uint16_t seg, uint16_t off)
{
    double v = 0, m = 1;
    for (int i = 0; i < 9; i++) {
        uint8_t b = mem_read8(cpu, seg, (uint16_t)(off + i));
        v += (b & 15) * m; m *= 10; v += (b >> 4) * m; m *= 10;
    }
    return (mem_read8(cpu, seg, (uint16_t)(off + 9)) & 0x80) ? -v : v;
}

static inline void x87_bcd_wr(CPU *cpu, uint16_t seg, uint16_t off, double v)
{
    double r = x87_round(v); int neg = signbit(r) != 0; r = fabs(r);
    uint64_t u = (isnan(r) || r >= 1e18) ? 0 : (uint64_t)r;
    for (int i = 0; i < 9; i++) {
        uint8_t lo = u % 10; u /= 10; uint8_t hi = u % 10; u /= 10;
        mem_write8(cpu, seg, (uint16_t)(off + i), (uint8_t)(lo | (hi << 4)));
    }
    mem_write8(cpu, seg, (uint16_t)(off + 9), neg ? 0x80 : 0);
}
#endif
