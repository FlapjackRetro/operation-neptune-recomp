/* SDL2 front end: window with the guest's screen, keyboard -> BIOS key queue. */
#include "frontend.h"
#include "runtime16.h"
#include "audio.h"
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define KQ 64
static int g_kq[KQ]; static volatile unsigned g_kr, g_kw;     /* single producer, single consumer */
#define RQ 128
static int g_rq[RQ]; static volatile unsigned g_rr, g_rw;
static pthread_mutex_t g_fm = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_frame[640 * 350 * 3]; static int g_fw, g_fh; static volatile int g_fnew;
static volatile int g_quit;
static int g_audio_rate = 48000;

int  fe_available(void) { return 1; }
int  fe_quit_requested(void) { return g_quit; }
void fe_push_key(int k) { if (g_kw - g_kr < KQ) { g_kq[g_kw % KQ] = k; __sync_synchronize(); g_kw++; } }
int  fe_pop_key(void) { if (g_kr == g_kw) return -1; int k = g_kq[g_kr % KQ]; __sync_synchronize(); g_kr++; return k; }

void fe_push_raw(int c) { if (g_rw - g_rr < RQ) { g_rq[g_rw % RQ] = c; __sync_synchronize(); g_rw++; } }
int  fe_pop_raw(void) { if (g_rr == g_rw) return -1; int c = g_rq[g_rr % RQ]; __sync_synchronize(); g_rr++; return c; }

/* SDL scancode -> set-1 make code (arrows/nav go out without the E0 prefix, like keypad keys with NumLock off) */
static int set1_of(SDL_Scancode sc)
{
    static const unsigned char az[26] = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return az[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 0x02 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return 0x3B + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) { static const unsigned char kp[9] = {0x4F,0x50,0x51,0x4B,0x4C,0x4D,0x47,0x48,0x49}; return kp[sc - SDL_SCANCODE_KP_1]; }
    switch (sc) {
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1C;
    case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_HOME: return 0x47; case SDL_SCANCODE_UP: return 0x48; case SDL_SCANCODE_PAGEUP: return 0x49;
    case SDL_SCANCODE_LEFT: return 0x4B; case SDL_SCANCODE_RIGHT: return 0x4D; case SDL_SCANCODE_END: return 0x4F;
    case SDL_SCANCODE_DOWN: return 0x50; case SDL_SCANCODE_PAGEDOWN: return 0x51; case SDL_SCANCODE_INSERT: case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_DELETE: return 0x53;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1D; case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    default: return 0;
    }
}

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

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    audio_render((int16_t *)stream, len / 4, g_audio_rate);
}

int fe_run(void (*start_guest)(void), volatile int *guest_done)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); start_guest(); return 1; }
    SDL_Window *win = SDL_CreateWindow("Operation Neptune (recomp) build " __DATE__ " " __TIME__, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *rd = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!rd) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); start_guest(); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(rd, 640, 480);       /* 4:3 letterbox: DOS pixels are not square */
    SDL_StartTextInput();
    SDL_AudioDeviceID adev = 0;
    {   /* AdLib + PC speaker mix; the callback pulls from audio.cpp. Failure just means no sound. */
        SDL_AudioSpec want, have; SDL_zero(want);
        want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
        adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (adev) { g_audio_rate = have.freq; SDL_PauseAudioDevice(adev, 0); }
        else fprintf(stderr, "SDL audio: %s (continuing without sound)\n", SDL_GetError());
    }
    SDL_Texture *tex = NULL; int tw = 0, th = 0;
    start_guest();                                 /* spawns the guest thread */
    unsigned last_ms = SDL_GetTicks(), frames = 0; unsigned long last_isr = 0, last_calls = 0;
    SDL_SetWindowTitle(win, "Operation Neptune (recomp) build " __DATE__ " " __TIME__);
    while (!*guest_done && !g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_quit = 1;
            else if (e.type == SDL_KEYDOWN) { int c = set1_of(e.key.keysym.scancode); if (c) fe_push_raw(c); on_keydown(e.key.keysym); }
            else if (e.type == SDL_KEYUP) { int c = set1_of(e.key.keysym.scancode); if (c) fe_push_raw(c | 0x80); }
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
            snprintf(title, sizeof title, "Operation Neptune [" __TIME__ "] timer %lu/s ticks %lu/s %u fps (isr total %lu, calls %lu)",
                     (i - last_isr) * 1000 / (t - last_ms), (c - last_calls) * 1000 / (t - last_ms), frames * 1000 / (t - last_ms), i, c);
            SDL_SetWindowTitle(win, title); last_isr = i; last_calls = c; frames = 0; last_ms = t;
        }
    }
    if (tex) SDL_DestroyTexture(tex);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyRenderer(rd); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
