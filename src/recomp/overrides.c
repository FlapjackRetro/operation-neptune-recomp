/*
 * overrides.c - native replacements for functions skipped by lift_full.py's
 * OVERRIDES table (see the list at the foot of gen/recomp_all.h).
 *
 * All four belong to Borland's 8087 emulator, which never runs here: float
 * opcodes are normalised to real ESC instructions at lift time and executed by
 * x87.h. Each one therefore does nothing but return the way the original did
 * (a far return, as in the lifted `retf`).
 */
#include "recomp_all.h"

#define FAR_RETURN() do { uint16_t _ip = pop16(cpu); uint16_t _cs = pop16(cpu); \
    if (_ip != 0xFFFF && _ip != 0) recomp_dispatch(cpu, _cs, _ip); } while (0)

void fn_00283(CPU *cpu) { FAR_RETURN(); }   /* emulator init hook, ds:[3F1A] */
void fn_002BD(CPU *cpu) { FAR_RETURN(); }   /* emulator (re)install entry */
void fn_002BE(CPU *cpu) { FAR_RETURN(); }   /* emulator installer */
void fn_00343(CPU *cpu) { FAR_RETURN(); }   /* emulator exit hook, ds:[3F1C] */
