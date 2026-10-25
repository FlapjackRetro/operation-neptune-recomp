#include <stdio.h>
/* Host test of src/recomp/x87.h against a flat fake guest memory. */
#include <assert.h>
#include <stdint.h>
typedef struct { uint8_t mem[1 << 16]; } CPU;
static uint8_t mem_read8(CPU *c, uint16_t s, uint16_t o) { (void)s; return c->mem[o]; }
static void mem_write8(CPU *c, uint16_t s, uint16_t o, uint8_t v) { (void)s; c->mem[o] = v; }
#include "../src/recomp/x87.h"
static CPU cpu_; static CPU *cpu = &cpu_;
int main(void)
{
    /* every width round-trips */
    double ds[] = { 0.0, 1.0, -2.5, 3.14159265358979, 1e300, -1e-300 };
    for (unsigned i = 0; i < sizeof ds / sizeof *ds; i++) {
        x87_wr(cpu, 0, 100, 8, ds[i]);  assert(x87_rd(cpu, 0, 100, 8) == ds[i]);
        x87_wr(cpu, 0, 100, 10, ds[i]); assert(x87_rd(cpu, 0, 100, 10) == ds[i]);
    }
    x87_wr(cpu, 0, 0, 4, 1.5); assert(x87_rd(cpu, 0, 0, 4) == 1.5);
    /* known 80-bit encoding of 1.0: mantissa 8000..., exp 3FFF */
    x87_wr(cpu, 0, 200, 10, 1.0);
    assert(cpu->mem[207] == 0x80 && cpu->mem[8 + 200] == 0xFF && cpu->mem[9 + 200] == 0x3F);
    /* stack + arithmetic: (6 - 2) / 4 via fsub / fdiv, then fsubr */
    x87_push(2.0); x87_push(6.0);               /* ST0=6 ST1=2 */
    x87_arith(4, &X87_ST(0), X87_ST(1));        /* ST0 = 6-2 = 4 */
    x87_arith(5, &X87_ST(0), 10.0);             /* subr: 10-4 = 6 */
    assert(X87_ST(0) == 6.0); x87_pop(); x87_pop(); assert(g_x87.top == 0);
    /* compare -> status word -> sahf-style flags */
    x87_cmp(1.0, 2.0); assert(x87_sw() & X87_C0);
    x87_cmp(2.0, 2.0); assert(x87_sw() & X87_C3 && !(x87_sw() & X87_C0));
    x87_cmp(3.0, 2.0); assert(!(x87_sw() & (X87_C0 | X87_C2 | X87_C3)));
    /* integer stores honour RC (Borland's ftol sets truncate = 0xC00) */
    g_x87.cw = 0x037F; x87_wr(cpu, 0, 300, -4, 2.5); assert(x87_rd(cpu, 0, 300, -4) == 2.0);
    x87_wr(cpu, 0, 300, -4, 3.5); assert(x87_rd(cpu, 0, 300, -4) == 4.0);
    g_x87.cw = 0x0F7F; x87_wr(cpu, 0, 300, -4, -2.9); assert(x87_rd(cpu, 0, 300, -4) == -2.0);
    x87_wr(cpu, 0, 300, -2, 1e9); assert(x87_rd(cpu, 0, 300, -2) == -32768.0);
    /* fxam */
    x87_push(0.0); x87_fxam(); assert(x87_sw() & X87_C3); x87_pop();
    x87_push(-1.0); x87_fxam(); assert(x87_sw() & X87_C1 && x87_sw() & X87_C2); x87_pop();
    puts("x87 ok"); return 0;
}
