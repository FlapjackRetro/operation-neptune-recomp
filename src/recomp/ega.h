#ifndef NEP_EGA_H
#define NEP_EGA_H
#include <stdint.h>
void    ega_set_mode(int mode, int clear);
int     ega_graphics(void);
void    ega_port_out(uint16_t port, uint8_t v);
int     ega_port_in(uint16_t port, uint8_t *v);   /* 1 = handled */
void    ega_set_palette_reg(int idx, int val);
void    ega_stats(void);
int     ega_dump_planes(const char *path);
int     ega_dump_ppm(const char *path);           /* composed screen */
extern  uint16_t g_ega_start;                      /* CRTC start address */
#endif
