/* runtime16.h - host surface the lifted Operation Neptune code calls into. */
#ifndef NEP_RUNTIME16_H
#define NEP_RUNTIME16_H
#include "cpu.h"

void    int_handler(CPU *cpu, int vec);
void    dos_int21(CPU *cpu);
void    bios_int10(CPU *cpu);
void    bios_int16(CPU *cpu);
void    mouse_int33(CPU *cpu);
uint8_t port_in8(CPU *cpu, uint16_t port);
void    port_out8(CPU *cpu, uint16_t port, uint8_t val);
uint16_t port_in16(CPU *cpu, uint16_t port);
void    port_out16(CPU *cpu, uint16_t port, uint16_t val);

int  recomp_dispatch(CPU *cpu, unsigned seg, unsigned off);   /* generated */
void dispatch_near(CPU *cpu, unsigned seg, unsigned off);
void dispatch_far(CPU *cpu, unsigned seg, unsigned off);
void recomp_dump_misses(const char *path);

void recomp_div0(const char *kind);

/* boot */
int  nep_load_image(CPU *cpu, const char *exe_path);
void nep_set_game_dir(const char *dir);
void nep_run(CPU *cpu);                 /* runs the startup; returns at exit */
extern int nep_exit_code;
extern volatile unsigned long g_stat_isrs, g_stat_calls;   /* timer ISRs run / recomp_tick calls so far */
void nep_dump_frame(CPU *cpu, const char *ppm_path);

#endif
