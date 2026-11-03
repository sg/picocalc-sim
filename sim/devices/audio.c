#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcsim/bus.h"
#include "pcsim/devices.h"
#include "pcsim/sim.h"

// The mainboard's audio circuit (schematic v2.0): U501 buffers GP26 and GP27 into 220R over 100R
// with 100nF across the 100R (a 24 kHz low-pass), then 47uF into 1.8K (taking off the DC), ahead
// of the volume control, the headphone jack and the speaker amplifiers.

#define IN_RATE 192000
#define OUT_RATE 48000
#define DECIM (IN_RATE / OUT_RATE)
#define TAPS 96
#define RING 16384
// Playback runs this far behind the emulation, and skips ahead when it falls further behind.
#define PRIME (OUT_RATE / 20)
#define SLACK (OUT_RATE / 10)

#define R501 220.0
#define R503 100.0
#define C502 100e-9
#define C504 47e-6
#define R505 1800.0

static const double pi = 3.14159265358979323846;

struct picocalc_audio {
    unsigned pins[2];
    bool listening;
    double a_lp, a_hp;
    double lp[2], hp[2], hp_in[2];
    float fir[TAPS];
    // Each sample is stored twice, so the newest TAPS always lie in one run from pos.
    float hist[2][2 * TAPS];
    int pos, phase;

    FILE *wav;
    uint32_t wav_frames;

    atomic_bool play;
    int16_t ring[RING][2];
    _Atomic uint32_t head, tail;
    bool primed;
};

static double par(double a, double b)
{
    return a * b / (a + b);
}

picocalc_audio_t *picocalc_audio_create(unsigned left_pin, unsigned right_pin)
{
    picocalc_audio_t *a = calloc(1, sizeof(*a));
    a->pins[0] = left_pin;
    a->pins[1] = right_pin;
    // The coupling capacitor shorts at audio frequencies, so the low-pass sees 1.8K beside the
    // 100R, and the high-pass sees the divider's output resistance in series with the 1.8K.
    double tau_lp = par(R501, par(R503, R505)) * C502;
    double tau_hp = (R505 + par(R501, R503)) * C504;
    a->a_lp = exp(-1.0 / (tau_lp * IN_RATE));
    a->a_hp = exp(-1.0 / (tau_hp * IN_RATE));
    // Blackman-windowed sinc at 22 kHz, well down by 28 kHz, which would fold back to 20 kHz.
    double fc = 22000.0 / IN_RATE, sum = 0;
    for (int i = 0; i < TAPS; i++) {
        double x = i - (TAPS - 1) / 2.0;
        double w = 0.42 - 0.5 * cos(2 * pi * i / (TAPS - 1)) + 0.08 * cos(4 * pi * i / (TAPS - 1));
        double v = sin(2 * pi * fc * x) / (pi * x) * w;
        a->fir[i] = (float)v;
        sum += v;
    }
    for (int i = 0; i < TAPS; i++)
        a->fir[i] = (float)(a->fir[i] / sum);
    return a;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static void wav_header(picocalc_audio_t *a)
{
    uint32_t data = a->wav_frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, 2);
    put32(h + 24, OUT_RATE);
    put32(h + 28, OUT_RATE * 4);
    put16(h + 32, 4);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    fseek(a->wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), a->wav);
    fseek(a->wav, 0, SEEK_END);
}

static void emit(picocalc_audio_t *a)
{
    uint8_t out[4];
    int16_t f[2];
    for (int ch = 0; ch < 2; ch++) {
        const float *h = &a->hist[ch][a->pos];
        float s = 0;
        for (int k = 0; k < TAPS; k++)
            s += a->fir[k] * h[k];
        // Full scale is the largest step the pins can make.
        long v = lrintf(s * 32767.0f);
        f[ch] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        put16(out + 2 * ch, (uint16_t)f[ch]);
    }
    // A RIFF file ends at 4 GB, about six hours in.
    if (a->wav && a->wav_frames < 0x3ffffff0u) {
        fwrite(out, 1, sizeof(out), a->wav);
        // The header is kept current, so a run that is killed still leaves a file that plays.
        if (++a->wav_frames % OUT_RATE == 0)
            wav_header(a);
    }
    if (!atomic_load_explicit(&a->play, memory_order_relaxed))
        return;
    uint32_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&a->tail, memory_order_acquire) >= RING)
        return;
    a->ring[head % RING][0] = f[0];
    a->ring[head % RING][1] = f[1];
    atomic_store_explicit(&a->head, head + 1, memory_order_release);
}

static void on_wave(void *ctx, const float *frac, size_t n)
{
    picocalc_audio_t *a = ctx;
    for (size_t i = 0; i < n; i++) {
        for (int ch = 0; ch < 2; ch++) {
            double x = frac[2 * i + (size_t)ch];
            a->lp[ch] = x + a->a_lp * (a->lp[ch] - x);
            double y = a->a_hp * (a->hp[ch] + a->lp[ch] - a->hp_in[ch]);
            // Far below one LSB; left alone it would decay into denormals.
            if (fabs(y) < 1e-12)
                y = 0;
            a->hp_in[ch] = a->lp[ch];
            a->hp[ch] = y;
            a->hist[ch][a->pos] = a->hist[ch][a->pos + TAPS] = (float)y;
        }
        a->pos = (a->pos + 1) % TAPS;
        if (++a->phase == DECIM) {
            a->phase = 0;
            emit(a);
        }
    }
}

static bool listen(picocalc_audio_t *a)
{
    if (a->listening)
        return true;
    if (!pcsim_pin_wave(a->pins, 2, IN_RATE, on_wave, a)) {
        pcsim_log("audio output is not available");
        return false;
    }
    a->listening = true;
    return true;
}

bool picocalc_audio_record(picocalc_audio_t *a, const char *wav_path)
{
    a->wav = fopen(wav_path, "wb");
    if (!a->wav) {
        pcsim_log("cannot write %s", wav_path);
        return false;
    }
    wav_header(a);
    if (!listen(a)) {
        fclose(a->wav);
        a->wav = NULL;
        return false;
    }
    return true;
}

bool picocalc_audio_play(picocalc_audio_t *a)
{
    if (!listen(a))
        return false;
    atomic_store(&a->play, true);
    return true;
}

size_t picocalc_audio_pull(picocalc_audio_t *a, int16_t *frames, size_t n)
{
    uint32_t tail = atomic_load_explicit(&a->tail, memory_order_relaxed);
    uint32_t avail = atomic_load_explicit(&a->head, memory_order_acquire) - tail;
    size_t got = 0, take = 0;
    if (!a->primed && avail >= PRIME)
        a->primed = true;
    if (a->primed) {
        if (avail > PRIME + SLACK) {
            tail += avail - PRIME;
            avail = PRIME;
        }
        if (avail < n) {
            // Starved: wait until a full lead has built up again rather than stutter.
            got = take = avail;
            a->primed = false;
        } else {
            // The host's audio clock and the one the emulation is paced by drift apart slowly;
            // a frame repeated or dropped now and then takes that up.
            got = n;
            take = avail < PRIME / 2 && n > 1 ? n - 1 : avail > PRIME + PRIME / 2 && avail > n ? n + 1 : n;
        }
        for (size_t i = 0; i < got; i++) {
            uint32_t at = tail + (uint32_t)(i * take / got);
            frames[2 * i] = a->ring[at % RING][0];
            frames[2 * i + 1] = a->ring[at % RING][1];
        }
        atomic_store_explicit(&a->tail, tail + (uint32_t)take, memory_order_release);
    }
    memset(frames + 2 * got, 0, (n - got) * 2 * sizeof(int16_t));
    return got;
}

void picocalc_audio_close(picocalc_audio_t *a)
{
    if (!a || !a->wav)
        return;
    pcsim_machine_lock();
    wav_header(a);
    fclose(a->wav);
    a->wav = NULL;
    pcsim_machine_unlock();
}
