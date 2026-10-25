/*
 * ega.c - planar EGA model: four 64K planes behind A000:0000, sequencer map
 * mask, graphics-controller write modes 0-3, read modes 0/1, latches, bit
 * mask, rotate/function, palette registers. Enough for a 1991 EGA game
 * (mode 0Dh 320x200 and 0Eh 640x200, 16 colours).
 */
#include "cpu.h"
#include "ega.h"
#include <stdio.h>
#include <string.h>

static uint8_t plane[4][65536];
static uint8_t latch[4];
static uint8_t seq_idx, seq_map = 0x0F;
static uint8_t gc_idx, gc[9] = { 0, 0, 0, 0, 0, 0, 0x05, 0x0F, 0xFF };
static uint8_t crtc_idx, crtc[32];
static uint8_t attr_idx, attr_flip, attr_pal[16] = { 0,1,2,3,4,5,0x14,7,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F };
static int g_mode = 3;
/* VGA DAC: 256 x 6-bit RGB. Entries 0..63 start as the EGA rgbRGB table. */
static uint8_t dac[256][3];
static int dac_wr_idx, dac_rd_idx, dac_phase;
static int dac_init_done;
static void dac_init(void)
{
    if (dac_init_done) return; dac_init_done = 1;
    for (int c = 0; c < 64; c++) {
        dac[c][0] = (uint8_t)(((c >> 2 & 1) * 0x2A) + ((c >> 5 & 1) * 0x15));
        dac[c][1] = (uint8_t)(((c >> 1 & 1) * 0x2A) + ((c >> 4 & 1) * 0x15));
        dac[c][2] = (uint8_t)(((c & 1) * 0x2A) + ((c >> 3 & 1) * 0x15));
    }
}
void ega_dac_set(int i, int r, int g, int b) { dac_init(); dac[i & 255][0] = r & 63; dac[i & 255][1] = g & 63; dac[i & 255][2] = b & 63; }
void ega_dac_get(int i, int *r, int *g, int *b) { dac_init(); *r = dac[i & 255][0]; *g = dac[i & 255][1]; *b = dac[i & 255][2]; }
uint16_t g_ega_start;

void ega_set_palette_reg(int i, int v) { if (i >= 0 && i < 16) attr_pal[i] = (uint8_t)(v & 0x3F); }
void ega_set_page(int page)
{
    int sz = g_mode == 0x0D ? 0x2000 : g_mode == 0x0E ? 0x4000 : 0x8000;
    g_ega_start = (uint16_t)(page * sz);
}
int ega_graphics(void) { return g_mode >= 0x0D && g_mode <= 0x10; }

void ega_set_mode(int mode, int clear)
{
    g_mode = mode;
    if (clear) memset(plane, 0, sizeof plane);
    seq_map = 0x0F; memset(gc, 0, sizeof gc); gc[6] = 0x05; gc[7] = 0x0F; gc[8] = 0xFF;
    g_ega_start = 0;
}

void ega_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3C0:
        if (!attr_flip) attr_idx = v & 0x1F; else if (attr_idx < 16) attr_pal[attr_idx] = v & 0x3F;
        attr_flip ^= 1; break;
    case 0x3C7: dac_rd_idx = v; dac_phase = 0; break;
    case 0x3C8: dac_wr_idx = v; dac_phase = 0; break;
    case 0x3C9: dac_init(); dac[dac_wr_idx & 255][dac_phase++] = v & 63;
                if (dac_phase == 3) { dac_phase = 0; dac_wr_idx++; } break;
    case 0x3C4: seq_idx = v & 7; break;
    case 0x3C5: if (seq_idx == 2) seq_map = v & 0x0F; break;
    case 0x3CE: gc_idx = v & 0x0F; break;
    case 0x3CF: if (gc_idx < 9) gc[gc_idx] = v; break;
    case 0x3D4: crtc_idx = v & 0x1F; break;
    case 0x3D5: crtc[crtc_idx] = v;
                g_ega_start = (uint16_t)((crtc[0x0C] << 8) | crtc[0x0D]); break;
    }
}

int ega_port_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x3DA: attr_flip = 0; { static uint8_t t; t ^= 0x09; *v = t; } return 1;
    case 0x3C9: dac_init(); *v = dac[dac_rd_idx & 255][dac_phase++];
                if (dac_phase == 3) { dac_phase = 0; dac_rd_idx++; } return 1;
    case 0x3C5: *v = seq_idx == 2 ? seq_map : 0; return 1;
    case 0x3CF: *v = gc_idx < 9 ? gc[gc_idx] : 0; return 1;
    }
    return 0;
}

static inline int in_window(uint32_t a) { return ega_graphics() && a >= 0xA0000 && a < 0xB0000; }

static uint8_t rotr8(uint8_t v, int n) { n &= 7; return (uint8_t)((v >> n) | (v << (8 - n))); }

static unsigned g_wstat[4][4][2][4];   /* write mode, function, enable-set/reset?, map-mask class */
void ega_stats(void)
{
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 2; c++) for (int d = 0; d < 4; d++)
        if (g_wstat[a][b][c][d]) fprintf(stderr, "  ega writes: mode %d fn %d setreset %d mapmask %s = %u\n", a, b, c,
            d == 0 ? "none" : d == 1 ? "one-plane" : d == 2 ? "all" : "some", g_wstat[a][b][c][d]);
}
int recomp_mem_write8(CPU *cpu, uint32_t addr, uint8_t val)
{
    (void)cpu;
    if (!in_window(addr)) return 0;
    { int m = seq_map, pc = __builtin_popcount(m); g_wstat[gc[5] & 3][(gc[3] >> 3) & 3][gc[1] ? 1 : 0][pc == 0 ? 0 : pc == 1 ? 1 : pc == 4 ? 2 : 3]++; }
    uint16_t off = (uint16_t)(addr - 0xA0000);
    int wm = gc[5] & 3, fn = (gc[3] >> 3) & 3;
    uint8_t out[4];
    for (int p = 0; p < 4; p++) {
        uint8_t d;
        switch (wm) {
        case 0: d = (gc[1] >> p & 1) ? ((gc[0] >> p & 1) ? 0xFF : 0x00) : rotr8(val, gc[3]); break;
        case 1: d = latch[p]; break;
        case 2: d = (val >> p & 1) ? 0xFF : 0x00; break;
        default: d = (gc[0] >> p & 1) ? 0xFF : 0x00; break;
        }
        uint8_t bm = gc[8];
        if (wm == 3) bm &= rotr8(val, gc[3]);
        if (wm != 1) {
            switch (fn) { case 1: d &= latch[p]; break; case 2: d |= latch[p]; break; case 3: d ^= latch[p]; break; }
        }
        out[p] = wm == 1 ? d : (uint8_t)((d & bm) | (latch[p] & ~bm));
    }
    for (int p = 0; p < 4; p++) if (seq_map >> p & 1) plane[p][off] = out[p];
    return 1;
}

int recomp_mem_read8(CPU *cpu, uint32_t addr, uint8_t *outv)
{
    (void)cpu;
    if (!in_window(addr)) return 0;
    uint16_t off = (uint16_t)(addr - 0xA0000);
    for (int p = 0; p < 4; p++) latch[p] = plane[p][off];
    if (!(gc[5] & 8)) { *outv = latch[gc[4] & 3]; return 1; }      /* read mode 0 */
    uint8_t r = 0xFF;                                               /* read mode 1: colour compare */
    for (int p = 0; p < 4; p++) if (gc[7] >> p & 1) {
        uint8_t want = (gc[2] >> p & 1) ? 0xFF : 0x00;
        r &= (uint8_t)~(latch[p] ^ want);
    }
    *outv = r; return 1;
}

static void rgb(int c, uint8_t *o)
{
    o[0] = (uint8_t)(((c >> 2 & 1) * 0xAA) + ((c >> 5 & 1) * 0x55));
    o[1] = (uint8_t)(((c >> 1 & 1) * 0xAA) + ((c >> 4 & 1) * 0x55));
    o[2] = (uint8_t)(((c & 1) * 0xAA) + ((c >> 3 & 1) * 0x55));
}

int ega_dump_ppm(const char *path)
{
    int w = g_mode == 0x0D ? 320 : 640, h = g_mode == 0x10 ? 350 : 200;
    if (!ega_graphics()) return -1;
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    int bpl = w / 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t o = (uint16_t)(g_ega_start + y * bpl + x / 8); int bit = 7 - (x & 7);
            int c = 0;
            for (int p = 0; p < 4; p++) c |= (plane[p][o] >> bit & 1) << p;
            dac_init(); uint8_t *d = dac[attr_pal[c]]; uint8_t px[3] = { (uint8_t)(d[0] * 255 / 63), (uint8_t)(d[1] * 255 / 63), (uint8_t)(d[2] * 255 / 63) }; fwrite(px, 1, 3, f);
        }
    fclose(f); return 0;
}

/* debug: dump the raw planes (4 x 64K) for offline inspection */
int ega_dump_planes(const char *path)
{
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fwrite(plane, 1, sizeof plane, f); fclose(f); return 0;
}
