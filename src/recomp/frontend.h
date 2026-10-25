/* Host window / keyboard front end. Real SDL2 build (NEP_SDL) or a no-op stub.
 * Threading: the guest runs on its own big-stack thread, so SDL (which wants
 * the main thread) is driven from main(); the two exchange frames and keys
 * through the functions below, all of which are thread-safe. */
#ifndef NEP_FRONTEND_H
#define NEP_FRONTEND_H
#include <stdint.h>
int  fe_available(void);                               /* 1 = a real window exists */
int  fe_run(void (*start_guest)(void), volatile int *guest_done);   /* main thread loop; returns at quit */
void fe_submit_frame(const uint8_t *rgb, int w, int h); /* guest thread: newest screen */
int  fe_pop_key(void);                                  /* guest thread: (scan << 8) | ascii, or -1 */
int  fe_quit_requested(void);
void fe_push_key(int scan_ascii);                       /* used by the SDL event code and tests */
#endif
