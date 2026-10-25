/*
 * runtime16.c - DOS / BIOS / PC hardware model for the lifted ON.EXE.
 *
 * Structure follows sp00nznet/dinopark's runtime (MIT), trimmed to what a
 * Turbo C 2.0 large-model program needs and written without that project's
 * debugging scaffolding. Image lives at linear 0 (load segment 0), so
 * `segment * 16 + offset` of the original program is the memory address.
 *
 * Environment variables:
 *   NEP_TRACE=1    log every DOS/BIOS call and dispatch miss
 *   NEP_SECONDS=n  watchdog; exit after n seconds (default 20)
 *   NEP_KEYS=...   scripted keystrokes, one ASCII char per key press
 */
#include "runtime16.h"
#include "x87.h"
#include "ega.h"
#include "frontend.h"
#include <ctype.h>
#include <dirent.h>
#include "portable.h"
#include <pthread.h>
#include <setjmp.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOADSEG   0x0000
#define PSP_SEG   0x9F00
#define ENV_SEG   0x9F10
#define TOP_SEG   0xA000
#define MAX_FILES 32
#define BIOS_SEG  0xF000          /* default vectors point at a ROM iret */
#define BIOS_IRET 0xFF53

int nep_exit_code = 0;
static CPU *g_cpu;
static jmp_buf g_exit_jmp;
static int g_trace;
static char g_game_dir[260] = ".";
static FILE *g_files[MAX_FILES];
static uint16_t g_dta_seg = PSP_SEG, g_dta_off = 0x80;
static uint16_t g_vec_seg[256], g_vec_off[256];
static uint16_t g_alloc_seg = 0x4000;      /* DOS memory above the program image */
static uint8_t  g_video_mode = 3;
static unsigned g_dos_calls;

static void trace(const char *fmt, ...)
{
    if (!g_trace) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

void recomp_div0(const char *kind)
{
    static int fired;
    if (fired++ < 6) fprintf(stderr, "[DIV0] guest %s by zero; registers left unchanged\n", kind);
}

void nep_set_game_dir(const char *dir) { snprintf(g_game_dir, sizeof g_game_dir, "%s", dir); }

/* ---------- guest strings and files ---------- */

static void guest_str(CPU *cpu, uint16_t seg, uint16_t off, char *out, int max)
{
    uint32_t a = seg_off(seg, off); int i = 0;
    for (; i < max - 1 && cpu->mem[a + i]; i++) out[i] = (char)cpu->mem[a + i];
    out[i] = 0;
}

/* "A:\TLC\ON\NEP000.DAT" -> <game_dir>/NEP000.DAT, case-insensitive match. */
static FILE *open_game_file(const char *dosname, const char *mode)
{
    const char *base = dosname;
    for (const char *p = dosname; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    char path[600];
    snprintf(path, sizeof path, "%s/%s", g_game_dir, base);
    FILE *f = fopen(path, mode);
    if (f) return f;
    DIR *d = opendir(g_game_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) if (!strcasecmp(e->d_name, base)) {
            snprintf(path, sizeof path, "%s/%s", g_game_dir, e->d_name);
            closedir(d); return fopen(path, mode);
        }
        closedir(d);
    }
    return NULL;
}

static void dos_fail(CPU *cpu, uint16_t err) { cpu->ax = err; cpu->flags |= FLAG_CF; }
static void dos_ok(CPU *cpu) { cpu->flags &= (uint16_t)~FLAG_CF; }

/* ---------- time ---------- */

static unsigned long host_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)(t.tv_sec * 1000UL + t.tv_nsec / 1000000UL);
}
static unsigned long bios_ticks(void) { return host_ms() * 182UL / 10000UL; }   /* 18.2 Hz */

/* ---------- INT 21h ---------- */

void dos_int21(CPU *cpu)
{
    char name[300];
    g_dos_calls++;
    trace("[21] ah=%02X ax=%04X bx=%04X cx=%04X dx=%04X ds=%04X\n",
          cpu->ah, cpu->ax, cpu->bx, cpu->cx, cpu->dx, cpu->ds);
    switch (cpu->ah) {
    case 0x02: fputc(cpu->dl, stdout); break;
    case 0x09: {
        uint32_t a = seg_off(cpu->ds, cpu->dx);
        while (cpu->mem[a] != '$') fputc(cpu->mem[a++], stdout);
        break; }
    case 0x0E: cpu->al = 4; break;                                   /* select drive: 4 drives */
    case 0x19: cpu->al = 0; break;                                   /* current drive A: */
    case 0x1A: g_dta_seg = cpu->ds; g_dta_off = cpu->dx; break;
    case 0x25: g_vec_seg[cpu->al] = cpu->ds; g_vec_off[cpu->al] = cpu->dx; break;
    case 0x2A: cpu->cx = 1991; cpu->dh = 6; cpu->dl = 1; cpu->al = 6; break;
    case 0x2C: { time_t t = time(NULL); struct tm *tm = localtime(&t);
                 cpu->ch = (uint8_t)tm->tm_hour; cpu->cl = (uint8_t)tm->tm_min;
                 cpu->dh = (uint8_t)tm->tm_sec; cpu->dl = 0; break; }
    case 0x2F: cpu->es = g_dta_seg; cpu->bx = g_dta_off; break;
    case 0x30: cpu->ax = 0x0005; cpu->bx = 0; cpu->cx = 0; break;       /* DOS 5.0 */
    case 0x33: cpu->dl = 0; break;                                   /* break flag */
    case 0x35: cpu->es = g_vec_seg[cpu->al]; cpu->bx = g_vec_off[cpu->al]; break;
    case 0x3B: dos_ok(cpu); break;                                   /* chdir: accept */
    case 0x3C: case 0x3D: {
        guest_str(cpu, cpu->ds, cpu->dx, name, sizeof name);
        const char *mode = cpu->ah == 0x3C ? "w+b" : ((cpu->al & 3) ? "r+b" : "rb");
        FILE *f = open_game_file(name, mode);
        if (!f && cpu->ah == 0x3D && (cpu->al & 3)) f = open_game_file(name, "rb");
        int h = -1;
        if (f) for (int i = 5; i < MAX_FILES; i++) if (!g_files[i]) { g_files[i] = f; h = i; break; }
        trace("    open \"%s\" -> %d\n", name, h);
        if (h < 0) { if (f) fclose(f); dos_fail(cpu, 2); } else { cpu->ax = (uint16_t)h; dos_ok(cpu); }
        break; }
    case 0x3E: {
        if (cpu->bx < MAX_FILES && g_files[cpu->bx]) { fclose(g_files[cpu->bx]); g_files[cpu->bx] = NULL; dos_ok(cpu); }
        else if (cpu->bx < 5) dos_ok(cpu); else dos_fail(cpu, 6);
        break; }
    case 0x3F: {
        if (cpu->bx == 0) { cpu->ax = 0; dos_ok(cpu); break; }             /* stdin: EOF */
        if (cpu->bx >= MAX_FILES || !g_files[cpu->bx]) { dos_fail(cpu, 6); break; }
        uint32_t a = seg_off(cpu->ds, cpu->dx);
        size_t n = fread(&cpu->mem[a], 1, cpu->cx, g_files[cpu->bx]);
        cpu->ax = (uint16_t)n; dos_ok(cpu); break; }
    case 0x40: {
        uint32_t a = seg_off(cpu->ds, cpu->dx);
        if (cpu->bx == 1 || cpu->bx == 2) { fwrite(&cpu->mem[a], 1, cpu->cx, cpu->bx == 1 ? stdout : stderr); cpu->ax = cpu->cx; dos_ok(cpu); break; }
        if (cpu->bx >= MAX_FILES || !g_files[cpu->bx]) { dos_fail(cpu, 6); break; }
        cpu->ax = (uint16_t)fwrite(&cpu->mem[a], 1, cpu->cx, g_files[cpu->bx]); dos_ok(cpu); break; }
    case 0x41: dos_fail(cpu, 2); break;
    case 0x42: {
        if (cpu->bx >= MAX_FILES || !g_files[cpu->bx]) { dos_fail(cpu, 6); break; }
        long off = (long)(((uint32_t)cpu->cx << 16) | cpu->dx);
        fseek(g_files[cpu->bx], off, cpu->al == 0 ? SEEK_SET : cpu->al == 1 ? SEEK_CUR : SEEK_END);
        long p = ftell(g_files[cpu->bx]);
        cpu->dx = (uint16_t)(p >> 16); cpu->ax = (uint16_t)p; dos_ok(cpu); break; }
    case 0x43: dos_ok(cpu); cpu->cx = 0x20; break;                   /* file attributes */
    case 0x44:                                                       /* IOCTL */
        if (cpu->al == 0) { cpu->dx = cpu->bx < 3 ? 0x80D3 : 0x0000; dos_ok(cpu); }
        else if (cpu->al == 1) dos_ok(cpu);
        else if (cpu->al == 8) { cpu->ax = 1; dos_ok(cpu); }        /* media fixed */
        else dos_fail(cpu, 1);
        break;
    case 0x47: cpu->mem[seg_off(cpu->ds, cpu->si)] = 0; dos_ok(cpu); break;   /* cwd: root */
    case 0x48: {                                                     /* allocate paragraphs */
        if ((uint32_t)g_alloc_seg + cpu->bx > 0x9000) { cpu->bx = (uint16_t)(0x9000 - g_alloc_seg); dos_fail(cpu, 8); break; }
        cpu->ax = g_alloc_seg; g_alloc_seg = (uint16_t)(g_alloc_seg + cpu->bx); dos_ok(cpu); break; }
    case 0x49: dos_ok(cpu); break;
    case 0x4A: dos_ok(cpu); break;                                   /* resize: always fits */
    case 0x4B: dos_fail(cpu, 2); break;                              /* exec */
    case 0x4C: nep_exit_code = cpu->al; cpu->halted = 1; longjmp(g_exit_jmp, 1);
    case 0x4D: cpu->ax = 0; break;
    case 0x4E: case 0x4F: dos_fail(cpu, 18); break;                  /* findfirst/next: nothing */
    case 0x62: cpu->bx = PSP_SEG; break;
    case 0x63: cpu->ds = 0; cpu->si = 0; dos_ok(cpu); break;
    default:
        fprintf(stderr, "[21] UNIMPLEMENTED ah=%02X al=%02X\n", cpu->ah, cpu->al);
        dos_fail(cpu, 1);
    }
}

/* ---------- BIOS ---------- */

void bios_int10(CPU *cpu)
{
    if (getenv("NEP_BT10")) { void *bt[10]; int n = backtrace(bt, 10); fprintf(stderr, "[10] ax=%04X from:\n", cpu->ax); backtrace_symbols_fd(bt + 1, n - 1, 2); }
    trace("[10] ax=%04X bx=%04X cx=%04X dx=%04X\n", cpu->ax, cpu->bx, cpu->cx, cpu->dx);
    switch (cpu->ah) {
    case 0x00: g_video_mode = cpu->al & 0x7F; ega_set_mode(g_video_mode, !(cpu->al & 0x80)); break;
    case 0x0F: cpu->al = g_video_mode; cpu->ah = 80; cpu->bh = 0; break;
    case 0x12: if (cpu->bl == 0x10) { cpu->bh = 0; cpu->bl = 3; cpu->cx = 0x0009; } break;   /* EGA info: 256K colour */
    case 0x1A: if (getenv("NEP_VGA")) { cpu->al = 0x1A; cpu->bl = 8; cpu->bh = 0; } else cpu->al = 0; break;   /* NEP_VGA=1 reports a VGA */
    case 0x10:                                                       /* palette */
        if (cpu->al == 0) ega_set_palette_reg(cpu->bl, cpu->bh);
        else if (cpu->al == 2) for (int i = 0; i < 16; i++) ega_set_palette_reg(i, cpu->mem[seg_off(cpu->es, (uint16_t)(cpu->dx + i))]);
        else if (cpu->al == 0x10) ega_dac_set(cpu->bx, cpu->dh, cpu->ch, cpu->cl);
        else if (cpu->al == 0x12 && getenv("NEP_DUMP_VGADRV") && cpu->mem[0x910] != 0xE9) {
            FILE *df = fopen(getenv("NEP_DUMP_VGADRV"), "wb");          /* the patched video driver, to be lifted */
            if (df) { fwrite(cpu->mem + 0x910, 1, 0x1070 - 0x910, df); fclose(df); }
            for (int i = 0; i < cpu->cx; i++) { uint32_t a = seg_off(cpu->es, (uint16_t)(cpu->dx + 3 * i));
                ega_dac_set(cpu->bx + i, cpu->mem[a], cpu->mem[a + 1], cpu->mem[a + 2]); } }
        else if (cpu->al == 0x12) for (int i = 0; i < cpu->cx; i++) { uint32_t a = seg_off(cpu->es, (uint16_t)(cpu->dx + 3 * i));
            ega_dac_set(cpu->bx + i, cpu->mem[a], cpu->mem[a + 1], cpu->mem[a + 2]); }
        break;
    case 0x05: ega_set_page(cpu->al); break;
    case 0x01: case 0x02: case 0x06: case 0x07: case 0x09: case 0x0A:
    case 0x0B: case 0x0C: case 0x0E: case 0x11: case 0x13: break;
    case 0x03: cpu->ax = 0; cpu->cx = 0; cpu->dx = 0; break;
    case 0x08: cpu->ax = 0x0720; break;
    case 0x0D: cpu->al = 0; break;
    default: fprintf(stderr, "[10] UNIMPLEMENTED ah=%02X\n", cpu->ah);
    }
}

static const char *g_keys; static int g_key_pos;
static int g_pend = -1;                         /* key fetched by a peek, not yet consumed: (scan<<8)|ascii */
/* next key without consuming: scripted NEP_KEYS first, then the window's queue */
static int peek_key(void)
{
    if (g_pend >= 0) return g_pend;
    if (!g_keys) { g_keys = getenv("NEP_KEYS"); if (!g_keys) g_keys = ""; }
    if (g_keys[g_key_pos]) g_pend = (unsigned char)g_keys[g_key_pos++];
    else g_pend = fe_pop_key();
    return g_pend;
}

void bios_int16(CPU *cpu)
{
    trace("[16] ah=%02X\n", cpu->ah);
    switch (cpu->ah) {
    case 0x00: case 0x10: {
        int k = peek_key();
        while (k < 0 && fe_available()) {           /* wait for the window; the timer keeps running */
            recomp_tick(cpu); usleep(2000); k = peek_key();
        }
        if (k < 0) { fprintf(stderr, "[16] guest waits for a key and none is scripted; stopping\n");
                     { void *bt[12]; int n = backtrace(bt, 12); backtrace_symbols_fd(bt, n, 2); }
                     cpu->halted = 1; longjmp(g_exit_jmp, 2); }
        g_pend = -1; cpu->al = (uint8_t)(k & 0xFF); cpu->ah = (uint8_t)(k >> 8); break; }
    case 0x01: case 0x11: {
        int k = peek_key();
        if (k >= 0) { cpu->flags &= (uint16_t)~FLAG_ZF; cpu->al = (uint8_t)(k & 0xFF); cpu->ah = (uint8_t)(k >> 8); }
        else cpu->flags |= FLAG_ZF;
        break; }
    case 0x02: cpu->al = 0; break;
    default: fprintf(stderr, "[16] UNIMPLEMENTED ah=%02X\n", cpu->ah);
    }
}

void mouse_int33(CPU *cpu) { cpu->ax = 0; }                          /* no mouse driver */

void int_handler(CPU *cpu, int vec)
{
    trace("[INT %02X] ax=%04X\n", vec, cpu->ax);
    switch (vec) {
    case 0x11: cpu->ax = 0x0061; break;                              /* EGA, 1 floppy, no 8087 */
    case 0x12: cpu->ax = 640; break;
    case 0x13: cpu->ah = 0x80; cpu->flags |= FLAG_CF; break;        /* disk: not ready */
    case 0x15: cpu->ah = 0x86; cpu->flags |= FLAG_CF; break;
    case 0x1A:
        if (cpu->ah == 0) { unsigned long t = bios_ticks(); cpu->cx = (uint16_t)(t >> 16); cpu->dx = (uint16_t)t; cpu->al = 0; }
        break;
    case 0x21: dos_int21(cpu); break;
    case 0x33: mouse_int33(cpu); break;
    case 0x02: case 0x03: break;                                     /* NMI / breakpoint: ignore */
    default: fprintf(stderr, "[INT %02X] UNIMPLEMENTED ax=%04X\n", vec, cpu->ax);
    }
}

/* ---------- ports ---------- */

static uint8_t g_port61, g_retrace;
uint8_t port_in8(CPU *cpu, uint16_t port)
{
    (void)cpu;
    { uint8_t v; if (ega_port_in(port, &v)) return v; }
    switch (port) {
    case 0x61:  g_port61 ^= 0x10; return g_port61;                   /* refresh bit toggles */
    case 0x40:  return (uint8_t)host_ms();
    case 0x60:  return 0;
    default:    trace("[in %03X]\n", port); return 0xFF;
    }
}
void port_out8(CPU *cpu, uint16_t port, uint8_t val)
{
    (void)cpu;
    if ((port >= 0x3C0 && port <= 0x3CF) || port == 0x3D4 || port == 0x3D5) ega_port_out(port, val);
    else if (port == 0x61) g_port61 = (uint8_t)((g_port61 & 0x10) | (val & ~0x10));
    else trace("[out %03X] = %02X\n", port, val);
}
uint16_t port_in16(CPU *cpu, uint16_t port) { return (uint16_t)(port_in8(cpu, port) | (port_in8(cpu, (uint16_t)(port + 1)) << 8)); }
void port_out16(CPU *cpu, uint16_t port, uint16_t v) { port_out8(cpu, port, (uint8_t)v); port_out8(cpu, (uint16_t)(port + 1), (uint8_t)(v >> 8)); }

/* ---------- image loading ---------- */

int nep_load_image(CPU *cpu, const char *path)
{
    g_cpu = cpu; g_vga_mem = cpu->mem;
    for (int v = 0; v < 256; v++) { g_vec_seg[v] = BIOS_SEG; g_vec_off[v] = BIOS_IRET; }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)sz);
    if (fread(file, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(file); return -1; }
    fclose(f);
    if (file[0] != 'M' || file[1] != 'Z') { fprintf(stderr, "not an MZ executable\n"); free(file); return -1; }

    uint16_t nrel = (uint16_t)(file[6] | file[7] << 8), hdr = (uint16_t)(file[8] | file[9] << 8);
    uint16_t ss = (uint16_t)(file[0x0E] | file[0x0F] << 8), sp = (uint16_t)(file[0x10] | file[0x11] << 8);
    uint16_t ip = (uint16_t)(file[0x14] | file[0x15] << 8), cs = (uint16_t)(file[0x16] | file[0x17] << 8);
    uint16_t reloff = (uint16_t)(file[0x18] | file[0x19] << 8);
    long img = (long)hdr * 16;
    memcpy(cpu->mem, file + img, (size_t)(sz - img));
    for (int i = 0; i < nrel; i++) {
        uint16_t ro = (uint16_t)(file[reloff + 4 * i] | file[reloff + 4 * i + 1] << 8);
        uint16_t rs = (uint16_t)(file[reloff + 4 * i + 2] | file[reloff + 4 * i + 3] << 8);
        uint32_t a = ((uint32_t)rs << 4) + ro;
        uint16_t w = (uint16_t)(cpu->mem[a] | cpu->mem[a + 1] << 8);
        w = (uint16_t)(w + LOADSEG);
        cpu->mem[a] = (uint8_t)w; cpu->mem[a + 1] = (uint8_t)(w >> 8);
    }
    free(file);

    /* PSP and environment above the image (Turbo C's startup scans the env) */
    uint32_t psp = seg_off(PSP_SEG, 0), env = seg_off(ENV_SEG, 0);
    cpu->mem[psp] = 0xCD; cpu->mem[psp + 1] = 0x20;
    cpu->mem[psp + 2] = TOP_SEG & 0xFF; cpu->mem[psp + 3] = TOP_SEG >> 8;
    cpu->mem[psp + 0x2C] = ENV_SEG & 0xFF; cpu->mem[psp + 0x2D] = ENV_SEG >> 8;
    cpu->mem[psp + 0x80] = 0; cpu->mem[psp + 0x81] = 0x0D;
    static const char *const envv[] = { "COMSPEC=A:\\COMMAND.COM", "PATH=A:\\" };
    static const char prog[] = "A:\\TLC\\ON\\ON.EXE";
    memset(&cpu->mem[env], 0, 0x400);
    uint32_t e = env;
    for (size_t i = 0; i < sizeof envv / sizeof *envv; i++) { size_t n = strlen(envv[i]) + 1; memcpy(&cpu->mem[e], envv[i], n); e += n; }
    cpu->mem[e++] = 0; cpu->mem[e++] = 1; cpu->mem[e++] = 0;
    memcpy(&cpu->mem[e], prog, sizeof prog);

    cpu->cs = (uint16_t)(cs + LOADSEG); cpu->ip = ip;
    cpu->ss = (uint16_t)(ss + LOADSEG); cpu->sp = sp;
    cpu->ds = PSP_SEG; cpu->es = PSP_SEG;
    fprintf(stderr, "loaded %ld image bytes, %u relocs; entry %04X:%04X SS:SP %04X:%04X\n",
            sz - img, nrel, cpu->cs, cpu->ip, cpu->ss, cpu->sp);
    return 0;
}

/* ---------- timer interrupt ----------
 * Lifted code never gets interrupted, so the guest's INT 08h handler is run
 * from recomp_tick (called on loop back-edges) at 18.2 Hz of host time. The
 * handler runs on the guest stack with the whole CPU saved and restored, so
 * a handler that chains to a BIOS we do not have cannot unbalance anything. */
int g_recomp_tick_budget = 4000;
static unsigned long g_next_tick;
static int g_in_isr;

static void run_guest_isr(CPU *cpu, int vec)
{
    if (g_vec_seg[vec] == BIOS_SEG) return;            /* still the ROM handler */
    CPU saved = *cpu;
    cpu->flags &= (uint16_t)~FLAG_IF;
    push16(cpu, cpu->flags); push16(cpu, 0x0000); push16(cpu, 0xFFFF);
    g_in_isr = 1;
    static int n;
    int ok = recomp_dispatch(cpu, g_vec_seg[vec], g_vec_off[vec]);
    if (n++ < 3) trace("[isr %02X] %04X:%04X dispatched=%d\n", vec, g_vec_seg[vec], g_vec_off[vec], ok);
    g_in_isr = 0;
    *cpu = saved;
}

void recomp_tick(CPU *cpu)
{
    g_recomp_tick_budget = 4000;
    unsigned long now = host_ms();
    if (fe_available()) {
        static unsigned long next_present;
        if (fe_quit_requested()) { cpu->halted = 1; longjmp(g_exit_jmp, 3); }
        if (now >= next_present) {
            static uint8_t buf[640 * 350 * 3]; int w, h;
            if (!ega_render_rgb(buf, &w, &h)) fe_submit_frame(buf, w, h);
            next_present = now + 16;
        }
    }
    { static unsigned long next; const char *pat = getenv("NEP_FRAMES");   /* NEP_FRAMES=prefix, NEP_FRAMES_MS=interval */
      if (pat && now >= next) { static int k; char nm[300]; const char *ms = getenv("NEP_FRAMES_MS");
          snprintf(nm, sizeof nm, "%s%03d.ppm", pat, k++); ega_dump_ppm(nm); next = now + (ms ? (unsigned)atoi(ms) : 1000); } }
    if (g_in_isr) return;
    /* PIT channel 0 at its default rate: 1193182 / 65536 = 18.2065 Hz (54.9254 ms). The ISR can only run
     * here, so when the guest spends a long time between loop back-edges ticks arrive late; run the
     * missed ones (a few at a time) so game time keeps pace with the wall clock. */
    static double next_us; static unsigned long isrs, calls, stat_ms;
    double now_us = (double)now * 1000.0;
    calls++;
    if (next_us == 0) next_us = now_us;
    int burst = 0;
    while (now_us >= next_us && burst < 4) {
        next_us += 54925.4; burst++; isrs++;
        run_guest_isr(cpu, 0x08);
    }
    if (now_us > next_us + 1000000.0) next_us = now_us + 54925.4;    /* never replay a long stall */
    { static int stat = -1; if (stat < 0) stat = getenv("NEP_STAT") != NULL;
      if (stat && now >= stat_ms + 1000) { fprintf(stderr, "[stat] isr/s=%lu tick-calls/s=%lu\n", isrs, calls); isrs = calls = 0; stat_ms = now; } }
}

/* ---------- function entry census (NEP_CENSUS=file) ---------- */
#define CENSUS_N 4096
static struct { const char *name; unsigned long n; } g_census[CENSUS_N];
static int g_census_on;
void recomp_enter(const char *fn)
{
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("NEP_DSWATCH") != NULL;
    if (dbg && g_cpu) {      /* NEP_DSWATCH: log DS changes with the last functions entered */
        static const char *ring[24]; static int rp, fired, last_ds = 0x2343;
        ring[rp++ % 24] = fn;
        if (g_cpu->ds != last_ds) { fprintf(stderr, "[ds] %04X -> %04X (cs=%04X) recent:", last_ds, g_cpu->ds, g_cpu->cs);
            for (int i = 1; i <= 8; i++) fprintf(stderr, " %s", ring[(rp - i + 48) % 24] ? ring[(rp - i + 48) % 24] : "-");
            fprintf(stderr, "\n"); last_ds = g_cpu->ds; if (++fired > 40) dbg = 0; }
    }
    if (!g_census_on) return;
    unsigned h = (unsigned)(((uintptr_t)fn >> 3) * 2654435761u) % CENSUS_N;
    for (int i = 0; i < CENSUS_N; i++, h = (h + 1) % CENSUS_N) {
        if (g_census[h].name == fn) { g_census[h].n++; return; }
        if (!g_census[h].name) { g_census[h].name = fn; g_census[h].n = 1; return; }
    }
}
static void census_dump(const char *path)
{
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < CENSUS_N; i++) if (g_census[i].name) fprintf(f, "%s %lu\n", g_census[i].name, g_census[i].n);
    fclose(f);
}

/* ---------- running ---------- */

extern void fn_00000(CPU *cpu);

#ifndef _WIN32
static pthread_t g_run_thread;
static void on_usr1(int sig)
{
    (void)sig;
    void *bt[48]; int n = backtrace(bt, 48);
    fprintf(stderr, "[watchdog] guest call stack (innermost first):\n");
    backtrace_symbols_fd(bt, n, 2);
    _exit(3);
}
#endif
static void watchdog_report(void)
{
    fprintf(stderr, "\n[watchdog] time limit reached: cs=%04X ds=%04X ss:sp=%04X:%04X, %u DOS calls\n",
            g_cpu->cs, g_cpu->ds, g_cpu->ss, g_cpu->sp, g_dos_calls);
    recomp_dump_misses("work/nep_misses.txt");
    { const char *o = getenv("NEP_FRAME"); if (o) ega_dump_ppm(o); }
    if (getenv("NEP_TEXT")) for (int r = 0; r < 25; r++) { char ln[81]; for (int c = 0; c < 80; c++) { uint8_t ch = g_cpu->mem[0xB8000 + (r * 80 + c) * 2]; ln[c] = ch >= 32 && ch < 127 ? (char)ch : ' '; } ln[80] = 0; fprintf(stderr, "|%s\n", ln); }
    ega_stats();
    { const char *o = getenv("NEP_PLANES"); if (o) ega_dump_planes(o); }
}
#ifndef _WIN32
static void on_alarm(int sig)
{
    (void)sig;
    watchdog_report();
    pthread_kill(g_run_thread, SIGUSR1);        /* backtrace from the guest thread */
    sleep(2);
    _exit(3);
}
#else
static unsigned g_wd_seconds;
static void *wd_thread(void *arg) { (void)arg; Sleep(g_wd_seconds * 1000u); watchdog_report(); fflush(stderr); _exit(3); return NULL; }
#endif

void nep_run(CPU *cpu)
{
    g_trace = getenv("NEP_TRACE") != NULL;
    g_census_on = getenv("NEP_CENSUS") != NULL;
    const char *s = getenv("NEP_SECONDS");
    /* Watchdog: always when headless (default 20 s); with a window only if NEP_SECONDS is set. */
    unsigned wd = s ? (unsigned)atoi(s) : (fe_available() ? 0 : 20);
#ifndef _WIN32
    g_run_thread = pthread_self();
    signal(SIGUSR1, on_usr1);
    signal(SIGALRM, on_alarm); if (wd) alarm(wd);
#else
    if (wd) { pthread_t t; g_wd_seconds = wd; pthread_create(&t, NULL, wd_thread, NULL); }
#endif
    cpu->flags |= FLAG_IF;
    if (setjmp(g_exit_jmp) == 0) fn_00000(cpu);
    recomp_dump_misses("work/nep_misses.txt");
    { const char *o = getenv("NEP_FRAME"); if (o) ega_dump_ppm(o); }
    fprintf(stderr, "[exit] code %d after %u DOS calls\n", nep_exit_code, g_dos_calls);
}

void nep_dump_frame(CPU *cpu, const char *ppm_path) { (void)cpu; ega_dump_ppm(ppm_path); }
