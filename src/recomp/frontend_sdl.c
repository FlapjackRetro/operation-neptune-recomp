/* SDL2 front end: window with the guest's screen, keyboard -> BIOS key queue. */
#include "frontend.h"
#include "runtime16.h"
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define KQ 64
static int g_kq[KQ]; static volatile unsigned g_kr, g_kw;     /* single producer, single consumer */
static pthread_mutex_t g_fm = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_frame[640 * 350 * 3]; static int g_fw, g_fh; static volatile int g_fnew;
static volatile int g_quit;

int  fe_available(void) { return 1; }
int  fe_quit_requested(void) { return g_quit; }
void fe_push_key(int k) { if (g_kw - g_kr < KQ) { g_kq[g_kw % KQ] = k; __sync_synchronize(); g_kw++; } }
int  fe_pop_key(void) { if (g_kr == g_kw) return -1; int k = g_kq[g_kr % KQ]; __sync_synchronize(); g_kr++; return k; }

void fe_submit_frame(const uint8_t *rgb, int w, int h)
{
    pthread_mutex_lock(&g_fm);
    memcpy(g_frame, rgb, (size_t)w * h * 3); g_fw = w; g_fh = h; g_fnew = 1;
    pthread_mutex_unlock(&g_fm);
}

/* US-layout set 1 scan codes for the keys games test via AH */
static int scan_of_ascii(int c)
{
    static const char *row1 = "1234567890-=", *row2 = "qwertyuiop[]", *row3 = "asdfghjkl;'", *row4 = "zxcvbnm,./";
    const char *p; c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((p = strchr(row1, c)) && c) return 0x02 + (int)(p - row1);
    if ((p = strchr(row2, c)) && c) return 0x10 + (int)(p - row2);
    if ((p = strchr(row3, c)) && c) return 0x1E + (int)(p - row3);
    if ((p = strchr(row4, c)) && c) return 0x2C + (int)(p - row4);
    if (c == ' ') return 0x39;
    return 0;
}

static void on_keydown(SDL_Keysym ks)
{
    switch (ks.sym) {
    case SDLK_RETURN: case SDLK_KP_ENTER: fe_push_key(0x1C00 | 13); return;
    case SDLK_ESCAPE: fe_push_key(0x0100 | 27); return;
    case SDLK_BACKSPACE: fe_push_key(0x0E00 | 8); return;
    case SDLK_TAB: fe_push_key(0x0F00 | 9); return;
    case SDLK_UP: fe_push_key(0x4800); return;     case SDLK_DOWN: fe_push_key(0x5000); return;
    case SDLK_LEFT: fe_push_key(0x4B00); return;   case SDLK_RIGHT: fe_push_key(0x4D00); return;
    case SDLK_HOME: fe_push_key(0x4700); return;   case SDLK_END: fe_push_key(0x4F00); return;
    case SDLK_PAGEUP: fe_push_key(0x4900); return; case SDLK_PAGEDOWN: fe_push_key(0x5100); return;
    case SDLK_INSERT: fe_push_key(0x5200); return; case SDLK_DELETE: fe_push_key(0x5300); return;
    default: break;
    }
    if (ks.sym >= SDLK_F1 && ks.sym <= SDLK_F10) { fe_push_key((0x3B + (int)(ks.sym - SDLK_F1)) << 8); return; }
    /* Ctrl+letter has no TEXTINPUT; everything printable arrives as text */
    if ((ks.mod & KMOD_CTRL) && ks.sym >= SDLK_a && ks.sym <= SDLK_z)
        fe_push_key((scan_of_ascii((int)ks.sym) << 8) | (int)(ks.sym - SDLK_a + 1));
}

int fe_run(void (*start_guest)(void), volatile int *guest_done)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); start_guest(); return 1; }
    SDL_Window *win = SDL_CreateWindow("Operation Neptune (recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *rd = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!rd) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); start_guest(); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(rd, 640, 480);       /* 4:3 letterbox: DOS pixels are not square */
    SDL_StartTextInput();
    SDL_Texture *tex = NULL; int tw = 0, th = 0;
    start_guest();                                 /* spawns the guest thread */
    unsigned last_ms = SDL_GetTicks(), frames = 0; unsigned long last_isr = 0, last_calls = 0;
    while (!*guest_done && !g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_quit = 1;
            else if (e.type == SDL_KEYDOWN) on_keydown(e.key.keysym);
            else if (e.type == SDL_TEXTINPUT) {
                for (const char *c = e.text.text; *c; c++) if ((unsigned char)*c < 128)
                    fe_push_key((scan_of_ascii(*c) << 8) | (unsigned char)*c);
            }
        }
        pthread_mutex_lock(&g_fm);
        if (g_fnew) {
            if (!tex || tw != g_fw || th != g_fh) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(rd, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, g_fw, g_fh);
                tw = g_fw; th = g_fh;
            }
            if (tex) SDL_UpdateTexture(tex, NULL, g_frame, g_fw * 3);
            g_fnew = 0;
        }
        pthread_mutex_unlock(&g_fm);
        SDL_RenderClear(rd);
        if (tex) SDL_RenderCopy(rd, tex, NULL, NULL);
        SDL_RenderPresent(rd);                     /* vsync paces the loop */
        frames++;
        unsigned t = SDL_GetTicks();
        if (t - last_ms >= 1000) {                 /* diagnostics in the title bar */
            char title[160]; unsigned long i = g_stat_isrs, c = g_stat_calls;
            snprintf(title, sizeof title, "Operation Neptune (recomp) - timer %lu/s, ticks %lu/s, %u fps",
                     i - last_isr, c - last_calls, frames);
            SDL_SetWindowTitle(win, title); last_isr = i; last_calls = c; frames = 0; last_ms = t;
        }
    }
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(rd); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
