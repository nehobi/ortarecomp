/*
 * I3DL2 environmental reverb. See reverb.h.
 *
 * Built from what the I3DL2 parameters mean, not from the Xbox DSP image:
 *
 *   send -> room level, room HF low-pass -> pre-delay line
 *     early reflections: taps from ReflectionsDelay on, at Reflections level,
 *       alternating left and right
 *     late reverb: from ReflectionsDelay + ReverbDelay, at Reverb level,
 *       through allpass diffusers (Diffusion) into an 8-line feedback delay
 *       network (orthogonal Hadamard feedback, line lengths scaled by
 *       Density), each line losing 60 dB in DecayTime at low frequencies
 *       and in DecayTime * DecayHFRatio at HFReference
 *
 * The late input is scaled so that the tail's energy matches the Reverb
 * level whatever the decay time, as I3DL2 defines that level.
 */

#include "audio/reverb.h"

#include <math.h>
#include <stdbool.h>

#define PRE_LEN   32768             /* > (0.3 + 0.1 s + taps) at 48 kHz */
#define LINES     8
#define LINE_LEN  4096
#define DIFFUSERS 4
#define DIFF_LEN  512
#define TAPS      6

/* Line lengths at 48 kHz, density 100%: primes between 30 and 67 ms. */
static const int line_base[LINES] = { 1427, 1777, 1973, 2099, 2297, 2557, 2879, 3221 };
static const int diff_len[DIFFUSERS] = { 149, 211, 293, 383 };
/* Early reflection taps after the first: ms after ReflectionsDelay, gain. */
static const float tap_ms[TAPS] = { 0.0f, 4.3f, 7.9f, 12.1f, 17.3f, 23.9f };
static const float tap_gain[TAPS] = { 1.0f, 0.84f, 0.71f, 0.6f, 0.5f, 0.42f };

static struct {
    pdo_i3dl2 p;
    bool      set, on;              /* a listener was given; it is audible */
    int       rate;
    /* derived */
    float     in_gain, in_a;        /* room level; room HF one-pole */
    float     refl_gain, late_gain;
    int       refl_at[TAPS], late_at;
    float     diff_k;
    int       len[LINES];
    float     loop_b[LINES], loop_a[LINES];
    /* state */
    float     in_lp;
    float     pre[PRE_LEN];
    int       pre_pos;
    float     diff[DIFFUSERS][DIFF_LEN];
    int       diff_pos[DIFFUSERS];
    float     line[LINES][LINE_LEN];
    int       line_pos[LINES];
    float     lp[LINES];
} r;

static float mb(int v)
{
    return v <= -10000 ? 0.0f : powf(10.0f, (float)v / 2000.0f);
}

/* The pole a of y = (1 - a) x + a y' whose gain at w (radians per sample) is
 * g, for 0 < g <= 1; DC gain is 1. */
static float one_pole_for(float g, float w)
{
    float c = cosf(w), g2, b, d;

    if (g >= 0.9999f)
        return 0.0f;
    if (g < 0.0001f)
        g = 0.0001f;
    g2 = g * g;
    b = 1.0f - g2 * c;
    d = b * b - (1.0f - g2) * (1.0f - g2);
    return (b - sqrtf(d > 0 ? d : 0)) / (1.0f - g2);
}

static void derive(void)
{
    const pdo_i3dl2 *p = &r.p;
    float fs = (float)r.rate;
    float w = 2.0f * 3.14159265f * fminf(p->hf_reference, 0.45f * fs) / fs;
    float scale = 0.6f + 0.4f * fminf(fmaxf(p->density, 0.0f), 100.0f) / 100.0f;
    float decay = fmaxf(p->decay_time, 0.1f);
    float hf_ratio = fminf(fmaxf(p->decay_hf_ratio, 0.1f), 1.0f);
    float energy = 0.0f;

    r.on = r.set && p->room > -10000 && (p->reflections > -10000 || p->reverb > -10000);
    r.in_gain = mb(p->room);
    r.in_a = one_pole_for(mb(p->room_hf), w);
    r.refl_gain = mb(p->reflections) / sqrtf((float)TAPS * 0.5f);
    r.late_gain = mb(p->reverb);
    for (int i = 0; i < TAPS; i++) {
        int at = (int)((p->reflections_delay + tap_ms[i] / 1000.0f) * fs);
        r.refl_at[i] = at < 1 ? 1 : at >= PRE_LEN ? PRE_LEN - 1 : at;
    }
    r.late_at = (int)((p->reflections_delay + p->reverb_delay) * fs);
    if (r.late_at < 1) r.late_at = 1;
    if (r.late_at >= PRE_LEN) r.late_at = PRE_LEN - 1;
    r.diff_k = 0.7f * fminf(fmaxf(p->diffusion, 0.0f), 100.0f) / 100.0f;

    for (int i = 0; i < LINES; i++) {
        int n = (int)(line_base[i] * scale * fs / 48000.0f);
        float g_lo, g_hi, a;

        r.len[i] = n < 64 ? 64 : n >= LINE_LEN ? LINE_LEN - 1 : n;
        /* -60 dB over the decay time, at DC and at the HF reference */
        g_lo = powf(10.0f, -3.0f * (float)r.len[i] / (decay * fs));
        g_hi = powf(10.0f, -3.0f * (float)r.len[i] / (decay * hf_ratio * fs));
        a = one_pole_for(g_hi / g_lo, w);
        r.loop_a[i] = a;
        r.loop_b[i] = g_lo * (1.0f - a);
        energy += g_lo * g_lo;
    }
    /* An impulse into a line that keeps g^2 of its energy each lap sums to
     * 1 / (1 - g^2): scale the input so the tail carries unit energy. */
    energy /= LINES;
    r.late_gain *= sqrtf(1.0f - fminf(energy, 0.9999f));
}

void pdo_reverb_set(const pdo_i3dl2 *p)
{
    r.p = *p;
    r.set = true;
    if (r.rate)
        derive();
}

static float pre_at(int delay)
{
    return r.pre[(r.pre_pos - delay) & (PRE_LEN - 1)];
}

void pdo_reverb_process(const float *send, float *out, int frames, int rate)
{
    const float h = 0.35355339f;    /* 1 / sqrt(8): orthogonal Hadamard */

    if (rate != r.rate) {
        r.rate = rate;
        derive();
    }
    for (int n = 0; n < frames; n++) {
        float x, l = 0.0f, rr = 0.0f, late, v[LINES];

        /* room level and room HF */
        x = r.on ? send[n] * r.in_gain : 0.0f;
        r.in_lp = x + r.in_a * (r.in_lp - x);
        if (fabsf(r.in_lp) < 1e-20f) r.in_lp = 0.0f;
        r.pre_pos = (r.pre_pos + 1) & (PRE_LEN - 1);
        r.pre[r.pre_pos] = r.in_lp;

        /* early reflections */
        for (int i = 0; i < TAPS; i++) {
            float s = pre_at(r.refl_at[i]) * tap_gain[i];
            if (i & 1) rr += s; else l += s;
        }
        l *= r.refl_gain;
        rr *= r.refl_gain;

        /* diffusion */
        late = pre_at(r.late_at) * r.late_gain;
        for (int i = 0; i < DIFFUSERS; i++) {
            float *d = r.diff[i];
            int p = r.diff_pos[i], q = (p + diff_len[i]) % DIFF_LEN;
            float dl = d[q], y = dl - r.diff_k * late;
            d[p] = late + r.diff_k * y;
            r.diff_pos[i] = p ? p - 1 : DIFF_LEN - 1;
            late = y;
        }

        /* the network: read, damp, mix, feed back */
        for (int i = 0; i < LINES; i++) {
            float y = r.line[i][(r.line_pos[i] + r.len[i]) & (LINE_LEN - 1)];
            r.lp[i] = r.loop_b[i] * y + r.loop_a[i] * r.lp[i];
            if (fabsf(r.lp[i]) < 1e-20f) r.lp[i] = 0.0f;
            v[i] = r.lp[i];
        }
        /* even lines left, odd lines right: each side gets half the
         * network's energy, the two together all of it */
        for (int i = 0; i < LINES; i += 2) {
            l += (i & 2) ? -v[i] : v[i];
            rr += (i & 4) ? -v[i + 1] : v[i + 1];
        }
        for (int span = 1; span < LINES; span <<= 1)
            for (int i = 0; i < LINES; i += span << 1)
                for (int j = i; j < i + span; j++) {
                    float a = v[j], b = v[j + span];
                    v[j] = a + b;
                    v[j + span] = a - b;
                }
        for (int i = 0; i < LINES; i++) {
            int p = r.line_pos[i] = (r.line_pos[i] - 1) & (LINE_LEN - 1);
            r.line[i][p] = v[i] * h + late * ((i & 1) ? -h : h);
        }

        out[n * 2] += l;
        out[n * 2 + 1] += rr;
    }
}
