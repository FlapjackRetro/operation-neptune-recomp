// AdLib/OPL2 (ymfm) + PC speaker mixer. See audio.h.
#include "audio.h"
#include "ymfm_opl.h"
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace {

static const uint32_t OPL_CLOCK = 3579545;

static double now_us()
{
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Guest code busy-waits on status reads (AdLib detection reads the status port ~100 times to wait out
// the 80 us timer); the recompiled code runs those loops far faster than ISA I/O, so every status read
// also advances a virtual clock by ~3 us.
static double g_skew_us = 0;
static double timer_now_us() { return now_us() + g_skew_us; }

// ymfm interface: the chip asks for two timers (80 us / 320 us ticks) that set the status flags the
// game's AdLib detection and some music drivers poll. Expiry is checked against the host clock.
class Iface : public ymfm::ymfm_interface
{
public:
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override
    {
        if (tnum > 1) return;
        m_deadline[tnum] = duration_in_clocks < 0 ? 0.0 : timer_now_us() + duration_in_clocks * 1e6 / OPL_CLOCK;
    }
    void poll_timers()
    {
        double t = timer_now_us();
        for (uint32_t i = 0; i < 2; i++)
            if (m_deadline[i] != 0.0 && t >= m_deadline[i]) { m_deadline[i] = 0.0; m_engine->engine_timer_expired(i); }
    }
private:
    double m_deadline[2] = { 0.0, 0.0 };
};

struct Audio {
    std::mutex mu;
    Iface iface;
    ymfm::ym3812 chip { iface };
    uint32_t chip_rate = 0;
    uint8_t  opl_addr = 0;
    // resampler state (linear interpolation from the chip rate)
    double   frac = 0; int32_t prev = 0, cur = 0;
    // PC speaker
    unsigned pit2_div = 0, pit2_lo = 0; int pit2_phase = 0, pit2_ctl = 0;
    uint8_t  p61 = 0; double spk_phase = 0;
    // WAV dump
    FILE *wav = nullptr; int wav_rate = 0; double wav_last = 0; uint32_t wav_bytes = 0;
    Audio() { chip.reset(); chip_rate = chip.sample_rate(OPL_CLOCK); }
};
Audio *g = nullptr;

inline int32_t chip_sample()
{
    ymfm::ym3812::output_data o;
    g->chip.generate(&o, 1);
    return o.data[0];
}

void render_locked(int16_t *out, int frames, int rate)
{
    g->iface.poll_timers();
    double step = (double)g->chip_rate / rate;
    bool spk_on = (g->p61 & 3) == 3 && g->pit2_div > 1;
    double spk_inc = spk_on ? (1193182.0 / g->pit2_div) / rate : 0;
    for (int i = 0; i < frames; i++) {
        g->frac += step;
        while (g->frac >= 1.0) { g->prev = g->cur; g->cur = chip_sample(); g->frac -= 1.0; }
        double s = g->prev + (g->cur - g->prev) * g->frac;           // OPL: ~+-32767 full scale
        s *= 0.8;
        if (spk_on) { g->spk_phase += spk_inc; if (g->spk_phase >= 1.0) g->spk_phase -= 1.0; s += g->spk_phase < 0.5 ? 5000 : -5000; }
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        out[2 * i] = out[2 * i + 1] = (int16_t)s;
    }
}

} // namespace

extern "C" {

void audio_init(void) { if (!g) g = new Audio(); }

int audio_port_out(uint16_t port, uint8_t v)
{
    if (!g) return 0;
    std::lock_guard<std::mutex> l(g->mu);
    switch (port) {
    case 0x388: { static int lg = -1; if (lg < 0) lg = getenv("NEP_AUDIOLOG") != NULL; if (lg) fprintf(stderr, "[opl] addr %02X\n", v); } g->opl_addr = v; g->chip.write_address(v); return 1;
    case 0x389: { static int lg = -1; if (lg < 0) lg = getenv("NEP_AUDIOLOG") != NULL; if (lg) fprintf(stderr, "[opl] data %02X\n", v); } g->chip.write_data(v); return 1;
    case 0x43:
        if ((v >> 6) == 2) { g->pit2_ctl = 1; g->pit2_phase = 0; } else if ((v >> 6) != 3) g->pit2_ctl = 0;
        return 0;                                      // the PIT model in the runtime still sees channel 0
    case 0x42:
        if (!g->pit2_ctl) return 1;
        if (g->pit2_phase == 0) { g->pit2_lo = v; g->pit2_phase = 1; }
        else { unsigned d = g->pit2_lo | ((unsigned)v << 8); g->pit2_div = d ? d : 65536; g->pit2_phase = 0; }
        return 1;
    }
    return 0;
}

int audio_port_in(uint16_t port, uint8_t *v)
{
    if (!g) return 0;
    if (port == 0x388) {
        std::lock_guard<std::mutex> l(g->mu);
        g_skew_us += 3.0;
        g->iface.poll_timers();
        *v = g->chip.read_status(); return 1;
    }
    return 0;
}

void audio_set_port61(uint8_t v) { if (g) { std::lock_guard<std::mutex> l(g->mu); g->p61 = v; } }

void audio_render(int16_t *stereo, int frames, int rate)
{
    if (!g) { memset(stereo, 0, (size_t)frames * 4); return; }
    std::lock_guard<std::mutex> l(g->mu);
    render_locked(stereo, frames, rate);
}

// ---- WAV dump (NEP_WAV=file): audio for the elapsed host time, appended from recomp_tick ----
void audio_wav_open(const char *path, int rate)
{
    if (!g) return;
    g->wav = fopen(path, "wb"); g->wav_rate = rate; g->wav_last = now_us(); g->wav_bytes = 0;
    if (g->wav) { uint8_t h[44] = { 0 }; fwrite(h, 1, 44, g->wav); }
}
void audio_wav_feed(void)
{
    if (!g || !g->wav) return;
    double t = now_us(); double dt = t - g->wav_last;
    if (dt < 20000) return;
    int frames = (int)(dt * g->wav_rate / 1e6); g->wav_last += frames * 1e6 / g->wav_rate;
    static int16_t buf[48000 * 2];
    if (frames > 48000) frames = 48000;
    std::lock_guard<std::mutex> l(g->mu);
    render_locked(buf, frames, g->wav_rate);
    fwrite(buf, 4, (size_t)frames, g->wav); g->wav_bytes += (uint32_t)frames * 4;
}
void audio_wav_close(void)
{
    if (!g || !g->wav) return;
    uint8_t h[44]; uint32_t rate = (uint32_t)g->wav_rate, br = rate * 4, n = g->wav_bytes, riff = 36 + n, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt, 4);
    memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4);
    memcpy(h + 32, &ba, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &n, 4);
    fseek(g->wav, 0, SEEK_SET); fwrite(h, 1, 44, g->wav); fclose(g->wav); g->wav = nullptr;
}

} // extern "C"
