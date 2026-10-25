/* Audio: AdLib (YM3812 via ymfm) on ports 388/389, PC speaker from PIT channel 2 + port 61h.
 * audio_render() is called from the SDL audio thread (or from recomp_tick for NEP_WAV); everything
 * is behind one mutex because the guest thread writes the ports. */
#ifndef NEP_AUDIO_H
#define NEP_AUDIO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void    audio_init(void);
int     audio_port_out(uint16_t port, uint8_t v);          /* 1 = handled */
int     audio_port_in(uint16_t port, uint8_t *v);          /* 1 = handled */
void    audio_set_port61(uint8_t v);                       /* speaker gate/data bits */
void    audio_render(int16_t *stereo, int frames, int rate);
void    audio_wav_open(const char *path, int rate);
void    audio_wav_feed(void);                              /* append audio for the host time elapsed */
void    audio_wav_close(void);
#ifdef __cplusplus
}
#endif
#endif
