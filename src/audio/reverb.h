#ifndef PDO_REVERB_H
#define PDO_REVERB_H

/*
 * I3DL2 environmental reverb: what the Xbox DSP's standard effects image
 * ran on the I3DL2 send, driven by IDirectSound_SetI3DL2Listener.
 *
 * The parameters are the I3DL2 listener's, in its own units: levels in
 * hundredths of a dB, times in seconds, diffusion and density in percent.
 */

typedef struct pdo_i3dl2 {
    int   room, room_hf;            /* -10000..0 */
    float room_rolloff;
    float decay_time;               /* 0.1..20 s */
    float decay_hf_ratio;           /* 0.1..2 */
    int   reflections;              /* -10000..1000 */
    float reflections_delay;        /* 0..0.3 s */
    int   reverb;                   /* -10000..2000 */
    float reverb_delay;             /* 0..0.1 s */
    float diffusion, density;       /* 0..100 */
    float hf_reference;             /* 20..20000 Hz */
} pdo_i3dl2;

/* Not thread-safe on its own: the mixer calls both under its lock. */
void pdo_reverb_set(const pdo_i3dl2 *p);

/* Run `frames` frames of the mono send through the reverb and add the
 * result to the stereo pairs in out. */
void pdo_reverb_process(const float *send, float *out, int frames, int rate);

#endif
