#ifndef NEP_EGA_H
#define NEP_EGA_H
#include <stdint.h>
void    ega_set_mode(int mode, int clear);
int     ega_graphics(void);
void    ega_set_page(int page);
void    ega_dac_set(int i, int r, int g, int b);
void    ega_dac_get(int i, int *r, int *g, int *b);
void    ega_port_out(uint16_t port, uint8_t v);
int     ega_port_in(uint16_t port, uint8_t *v);   /* 1 = handled */
void    ega_set_palette_reg(int idx, int val);
void    ega_stats(void);
int     ega_dump_planes(const char *path);
int     ega_render_rgb(uint8_t *out, int *w, int *h);  /* RGB24 screen, 0 = ok */
int     ega_dump_ppm(const char *path);           /* composed screen */
extern  uint8_t *g_vga_mem;                        /* guest memory (mode 13h frame buffer at A0000) */
extern  uint16_t g_ega_start;                      /* CRTC start address */
#endif
