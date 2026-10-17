/*
 * Software voice mixer. See mixer.h.
 *
 * Debugging switches (environment):
 *   PDO_AUDIO_DUMP=file.wav   record exactly what is mixed (float stereo)
 *   PDO_AUDIO_STATS=1         log peak level and clipped samples per second
 */

#include "audio/mixer.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES     512
#define BLOCK_FRAMES   256
#define XADPCM_BLOCK   36      /* bytes per channel per block */
#define MAX_PACKETS    64

typedef struct packet {
    uint32_t addr, bytes, tag;
} packet;

extern ptrdiff_t g_xbox_mem_offset;

typedef struct voice {
    bool           used;
    pdo_voice_desc desc;
    bool           playing, looping;
    double         pos;                 /* in frames */
    uint32_t       loop_start, loop_len;/* frames; len 0 = whole buffer */
    float          volume, headroom;    /* linear */
    double         step;                /* frames per output frame */
    float          gain[2][PDO_BIN_SPEAKERS];
    /* ADPCM: the last decoded block, both channels */
    int32_t        cached_block;
    int16_t        cache[2][72];
    /* Packet queue (streams): the head packet is desc.data/desc.bytes. */
    bool           queued;
    packet         q[MAX_PACKETS];
    int            q_head, q_count;
    pdo_packet_done_fn done;
    void          *done_user;
} voice;

static voice voices[MAX_VOICES];
static SDL_Mutex *lock;
static SDL_AudioStream *stream;
static SDL_Thread *pacer;
static SDL_AtomicInt pacer_quit;

static SDL_IOStream *dump;
static Uint64 dump_frames;
static bool stats;
static float stat_peak;
static Uint64 stat_clipped, stat_frames;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static float db100_to_gain(int32_t v)
{
    if (v <= -10000)
        return 0.0f;
    return powf(10.0f, (float)v / 2000.0f);
}

static voice *get(int id)
{
    if (id <= 0 || id > MAX_VOICES || !voices[id - 1].used)
        return NULL;
    return &voices[id - 1];
}

static const uint8_t *guest(uint32_t va)
{
    return (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static uint32_t spb(const voice *v)
{
    return v->desc.samples_per_block ? v->desc.samples_per_block : 64;
}

static uint32_t frame_bytes(const voice *v)
{
    return (v->desc.format == PDO_FMT_PCM8 ? 1 : 2) * (uint32_t)v->desc.channels;
}

static uint32_t total_frames(const voice *v)
{
    if (!v->desc.data)
        return 0;
    if (v->desc.format == PDO_FMT_XADPCM)
        return v->desc.bytes / (XADPCM_BLOCK * (uint32_t)v->desc.channels) * spb(v);
    return v->desc.bytes / frame_bytes(v);
}

static uint32_t bytes_to_frames(const voice *v, uint32_t bytes)
{
    if (v->desc.format == PDO_FMT_XADPCM)
        return bytes / (XADPCM_BLOCK * (uint32_t)v->desc.channels) * spb(v);
    return bytes / frame_bytes(v);
}

static uint32_t frames_to_bytes(const voice *v, uint32_t frames)
{
    if (v->desc.format == PDO_FMT_XADPCM)
        return frames / spb(v) * XADPCM_BLOCK * (uint32_t)v->desc.channels;
    return frames * frame_bytes(v);
}

static void update_step(voice *v, uint32_t hz)
{
    v->step = (double)(hz ? hz : v->desc.sample_rate) / PDO_MIX_RATE;
}

/* Which channel of the buffer a mixbin pair feeds. One pair per channel in
 * order for multichannel buffers; every pair takes the mono channel. */
static void update_gains(voice *v)
{
    memset(v->gain, 0, sizeof v->gain);
    for (int i = 0; i < v->desc.bin_count; i++) {
        int ch = v->desc.channels == 1 ? 0 : i % v->desc.channels;
        uint32_t bin = v->desc.bins[i].bin;
        if (bin < PDO_BIN_SPEAKERS)
            v->gain[ch][bin] = db100_to_gain(v->desc.bins[i].volume);
        else {
            static uint32_t seen;
            if (bin < 32 && !(seen & (1u << bin))) {
                seen |= 1u << bin;
                SDL_Log("audio: mixbin %u (effect send) not mixed yet", bin);
            }
        }
    }
}

/* ── Xbox ADPCM ───────────────────────────────────────────────────────────
 * IMA ADPCM in 36-byte blocks per channel: a 4-byte header (int16 sample,
 * uint8 step index, reserved) then 32 bytes of nibbles, low nibble first.
 * Multichannel blocks interleave the channels four bytes at a time. A block
 * yields 64 samples: the header sample, then 63 nibbles (the last nibble of
 * a block is not played). */

static const int16_t ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static void decode_block(voice *v, uint32_t block)
{
    int chs = v->desc.channels;
    const uint8_t *b = guest(v->desc.data + block * XADPCM_BLOCK * (uint32_t)chs);
    uint32_t n = spb(v);

    for (int ch = 0; ch < chs; ch++) {
        const uint8_t *h = b + ch * 4;
        int pred = (int16_t)(h[0] | h[1] << 8);
        int idx = h[2] > 88 ? 88 : h[2];
        uint32_t out = 0;

        v->cache[ch][out++] = (int16_t)pred;
        for (uint32_t word = 0; word < 8 && out < n; word++) {
            const uint8_t *d = b + 4 * chs + (word * chs + ch) * 4;
            for (int byte = 0; byte < 4 && out < n; byte++) {
                for (int half = 0; half < 2 && out < n; half++) {
                    int nib = (d[byte] >> (half * 4)) & 15;
                    int step = ima_steps[idx];
                    int diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    pred += (nib & 8) ? -diff : diff;
                    if (pred > 32767) pred = 32767;
                    if (pred < -32768) pred = -32768;
                    idx += ima_index[nib];
                    if (idx < 0) idx = 0;
                    if (idx > 88) idx = 88;
                    v->cache[ch][out++] = (int16_t)pred;
                }
            }
        }
    }
    v->cached_block = (int32_t)block;
}

static float sample_at(voice *v, int ch, uint32_t frame)
{
    switch (v->desc.format) {
    case PDO_FMT_PCM8:
        return ((float)guest(v->desc.data)[frame * v->desc.channels + ch] - 128.0f) / 128.0f;
    case PDO_FMT_PCM16: {
        const uint8_t *p = guest(v->desc.data) + (frame * v->desc.channels + ch) * 2;
        return (float)(int16_t)(p[0] | p[1] << 8) / 32768.0f;
    }
    case PDO_FMT_XADPCM: {
        uint32_t block = frame / spb(v);
        if ((int32_t)block != v->cached_block)
            decode_block(v, block);
        return (float)v->cache[ch][frame % spb(v)] / 32768.0f;
    }
    }
    return 0.0f;
}

/* ── Mixing ───────────────────────────────────────────────────────────── */

/* Make the head of the queue the voice's data (or none). */
static void load_head(voice *v)
{
    if (v->q_count) {
        v->desc.data = v->q[v->q_head].addr;
        v->desc.bytes = v->q[v->q_head].bytes;
    } else {
        v->desc.data = 0;
        v->desc.bytes = 0;
    }
    v->cached_block = -1;
}

static void pop_head(voice *v, bool flushed)
{
    packet p = v->q[v->q_head];

    v->q_head = (v->q_head + 1) % MAX_PACKETS;
    v->q_count--;
    load_head(v);
    if (v->done)
        v->done(v->done_user, p.tag, flushed);
}

/* A packet-queue voice: play the head packet, moving to the next one at its
 * end without a gap (the fraction of a frame carries over). Interpolation
 * does not look across a packet boundary. */
static void mix_queue(voice *v, float bins[][PDO_BIN_SPEAKERS], int frames)
{
    float g = v->volume * v->headroom;

    for (int i = 0; i < frames; i++) {
        uint32_t total, f0, f1;
        float t;

        while (v->q_count && v->pos >= (total = total_frames(v))) {
            v->pos -= total;
            pop_head(v, false);
        }
        if (!v->q_count) {
            v->pos = 0;
            return;
        }
        total = total_frames(v);
        f0 = (uint32_t)v->pos;
        f1 = f0 + 1 < total ? f0 + 1 : f0;
        t = (float)(v->pos - f0);
        for (int ch = 0; ch < v->desc.channels; ch++) {
            float s = sample_at(v, ch, f0);
            s += (sample_at(v, ch, f1) - s) * t;
            s *= g;
            for (int b = 0; b < PDO_BIN_SPEAKERS; b++)
                bins[i][b] += s * v->gain[ch][b];
        }
        v->pos += v->step;
    }
}

static void mix_voice(voice *v, float bins[][PDO_BIN_SPEAKERS], int frames)
{
    uint32_t total = total_frames(v);
    uint32_t ls, le;
    float g = v->volume * v->headroom;

    if (!total)
        return;
    ls = v->looping ? v->loop_start : 0;
    le = (v->looping && v->loop_len) ? v->loop_start + v->loop_len : total;
    if (le > total) le = total;
    if (ls >= le) ls = 0;

    for (int i = 0; i < frames; i++) {
        uint32_t f0, f1;
        float t;

        /* Wrap before taking this frame's sample, not instead of it:
         * skipping the frame left one silent output sample per lap of a
         * looping buffer, a click ~2.7 times a second under streamed music
         * (64 KB ring at 44.1 kHz), and lost a frame of time per lap. */
        if (v->pos >= le) {
            if (!v->looping) {
                v->playing = false;
                return;
            }
            v->pos -= (le - ls);
            if (v->pos < ls) v->pos = ls;
        }
        f0 = (uint32_t)v->pos;
        f1 = f0 + 1;
        t = (float)(v->pos - f0);
        if (f1 >= le)
            f1 = v->looping ? ls : f0;

        for (int ch = 0; ch < v->desc.channels; ch++) {
            float s = sample_at(v, ch, f0);
            s += (sample_at(v, ch, f1) - s) * t;
            s *= g;
            for (int b = 0; b < PDO_BIN_SPEAKERS; b++)
                bins[i][b] += s * v->gain[ch][b];
        }
        v->pos += v->step;
    }
}

static void write_dump_header(void)
{
    /* RIFF/WAVE, IEEE float stereo, sizes patched as the file grows. */
    uint32_t data = (uint32_t)(dump_frames * 8);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4) = 36 + data;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 3;
    *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = PDO_MIX_RATE;
    *(uint32_t *)(h + 28) = PDO_MIX_RATE * 8;
    *(uint16_t *)(h + 32) = 8;
    *(uint16_t *)(h + 34) = 32;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = data;
    SDL_SeekIO(dump, 0, SDL_IO_SEEK_SET);
    SDL_WriteIO(dump, h, sizeof h);
    SDL_SeekIO(dump, 0, SDL_IO_SEEK_END);
}

static void mix_block(float *out, int frames)
{
    static float bins[BLOCK_FRAMES][PDO_BIN_SPEAKERS];
    const float k = 0.70710678f;

    memset(bins, 0, sizeof(bins[0]) * (size_t)frames);
    SDL_LockMutex(lock);
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].playing) {
            if (voices[i].queued)
                mix_queue(&voices[i], bins, frames);
            else
                mix_voice(&voices[i], bins, frames);
        }
    SDL_UnlockMutex(lock);

    /* Fold the speaker bins to stereo. LFE is not folded in, as a stereo
     * receiver would drop it too. */
    for (int i = 0; i < frames; i++) {
        float *b = bins[i];
        float l = b[PDO_BIN_FRONT_LEFT] + k * b[PDO_BIN_FRONT_CENTER] + k * b[PDO_BIN_BACK_LEFT];
        float r = b[PDO_BIN_FRONT_RIGHT] + k * b[PDO_BIN_FRONT_CENTER] + k * b[PDO_BIN_BACK_RIGHT];

        if (stats) {
            float m = fmaxf(fabsf(l), fabsf(r));
            if (m > stat_peak) stat_peak = m;
            if (m > 1.0f) stat_clipped++;
        }
        out[i * 2]     = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        out[i * 2 + 1] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
    }

    if (dump) {
        SDL_WriteIO(dump, out, (size_t)frames * 8);
        dump_frames += (Uint64)frames;
        if (dump_frames % PDO_MIX_RATE < (Uint64)frames)
            write_dump_header();
    }
    if (stats) {
        stat_frames += (Uint64)frames;
        if (stat_frames >= PDO_MIX_RATE) {
            SDL_Log("audio: peak %.2f dBFS, %llu clipped frames in the last second",
                    stat_peak > 0 ? 20.0 * log10(stat_peak) : -120.0,
                    (unsigned long long)stat_clipped);
            stat_frames = 0;
            stat_peak = 0;
            stat_clipped = 0;
        }
    }
}

static void SDLCALL stream_cb(void *userdata, SDL_AudioStream *s, int additional, int total)
{
    float buf[BLOCK_FRAMES * 2];

    while (additional > 0) {
        int frames = additional / 8;
        if (frames > BLOCK_FRAMES) frames = BLOCK_FRAMES;
        if (frames <= 0) frames = 1;
        mix_block(buf, frames);
        SDL_PutAudioStreamData(s, buf, frames * 8);
        additional -= frames * 8;
    }
}

/* Without an audio device the game still needs its play cursors to move at
 * the real rate, or streaming code waits forever. */
static int SDLCALL pacer_main(void *unused)
{
    float buf[BLOCK_FRAMES * 2];
    Uint64 start = SDL_GetTicksNS(), mixed = 0;

    while (!SDL_GetAtomicInt(&pacer_quit)) {
        Uint64 due = (SDL_GetTicksNS() - start) * PDO_MIX_RATE / SDL_NS_PER_SECOND;
        while (mixed + BLOCK_FRAMES <= due) {
            mix_block(buf, BLOCK_FRAMES);
            mixed += BLOCK_FRAMES;
        }
        SDL_Delay(2);
    }
    return 0;
}

/* ── API ──────────────────────────────────────────────────────────────── */

bool pdo_mixer_init(void)
{
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, PDO_MIX_RATE };
    const char *path = getenv("PDO_AUDIO_DUMP");
    const char *st = getenv("PDO_AUDIO_STATS");

    if (lock)
        return true;
    lock = SDL_CreateMutex();
    stats = st && *st && *st != '0';
    if (path && *path) {
        dump = SDL_IOFromFile(path, "wb");
        if (dump)
            write_dump_header();
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, stream_cb, NULL);
    if (stream) {
        SDL_ResumeAudioStreamDevice(stream);
        SDL_Log("audio: %s, 48 kHz stereo", SDL_GetCurrentAudioDriver());
    } else {
        SDL_Log("audio: no output device (%s); mixing silently in real time", SDL_GetError());
        pacer = SDL_CreateThread(pacer_main, "pdo_audio", NULL);
    }
    return true;
}

void pdo_mixer_shutdown(void)
{
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
    if (pacer) {
        SDL_SetAtomicInt(&pacer_quit, 1);
        SDL_WaitThread(pacer, NULL);
        pacer = NULL;
    }
    if (dump) {
        write_dump_header();
        SDL_CloseIO(dump);
        dump = NULL;
    }
}

int pdo_voice_create(const pdo_voice_desc *desc)
{
    int id = 0;

    SDL_LockMutex(lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].used) {
            voice *v = &voices[i];
            memset(v, 0, sizeof *v);
            v->used = true;
            v->desc = *desc;
            v->volume = 1.0f;
            v->headroom = 1.0f;
            v->cached_block = -1;
            update_step(v, 0);
            update_gains(v);
            id = i + 1;
            break;
        }
    }
    SDL_UnlockMutex(lock);
    if (!id)
        SDL_Log("audio: out of voices");
    return id;
}

void pdo_voice_destroy(int id)
{
    SDL_LockMutex(lock);
    if (get(id))
        voices[id - 1].used = false;
    SDL_UnlockMutex(lock);
}

#define WITH_VOICE(id, body) do {               \
        voice *v;                               \
        SDL_LockMutex(lock);                    \
        if ((v = get(id)) != NULL) { body; }    \
        SDL_UnlockMutex(lock);                  \
    } while (0)

void pdo_voice_set_data(int id, uint32_t addr, uint32_t bytes)
{
    WITH_VOICE(id, v->desc.data = addr; v->desc.bytes = bytes; v->pos = 0; v->cached_block = -1);
}

void pdo_voice_play(int id, bool looping)
{
    WITH_VOICE(id, v->playing = true; v->looping = looping);
}

void pdo_voice_stop(int id)
{
    WITH_VOICE(id, v->playing = false);
}

bool pdo_voice_playing(int id, bool *looping)
{
    bool playing = false;
    WITH_VOICE(id, playing = v->playing; if (looping) *looping = v->looping);
    return playing;
}

void pdo_voice_set_position(int id, uint32_t byte_offset)
{
    WITH_VOICE(id, v->pos = bytes_to_frames(v, byte_offset));
}

uint32_t pdo_voice_position(int id)
{
    uint32_t pos = 0;
    WITH_VOICE(id, pos = frames_to_bytes(v, (uint32_t)v->pos));
    return pos;
}

void pdo_voice_set_loop_region(int id, uint32_t start, uint32_t length)
{
    WITH_VOICE(id, v->loop_start = bytes_to_frames(v, start); v->loop_len = bytes_to_frames(v, length));
}

void pdo_voice_set_volume(int id, int32_t hundredths_db)
{
    WITH_VOICE(id, v->volume = db100_to_gain(hundredths_db));
}

void pdo_voice_set_headroom(int id, uint32_t hundredths_db)
{
    WITH_VOICE(id, v->headroom = db100_to_gain(-(int32_t)hundredths_db));
}

void pdo_voice_set_frequency(int id, uint32_t hz)
{
    WITH_VOICE(id, update_step(v, hz));
}

void pdo_voice_set_bins(int id, const pdo_voice_bin *bins, int count)
{
    if (count > PDO_MAX_VOICE_BINS)
        count = PDO_MAX_VOICE_BINS;
    WITH_VOICE(id, memcpy(v->desc.bins, bins, sizeof(*bins) * (size_t)count);
               v->desc.bin_count = count; update_gains(v));
}

uint32_t pdo_voice_block_align(int id)
{
    uint32_t a = 1;
    WITH_VOICE(id, a = v->desc.format == PDO_FMT_XADPCM
                   ? XADPCM_BLOCK * (uint32_t)v->desc.channels : frame_bytes(v));
    return a;
}

void pdo_voice_set_packet_done(int id, pdo_packet_done_fn done, void *user)
{
    WITH_VOICE(id, v->done = done; v->done_user = user);
}

bool pdo_voice_queue(int id, uint32_t addr, uint32_t bytes, uint32_t tag)
{
    bool ok = false;
    voice *v;

    SDL_LockMutex(lock);
    if ((v = get(id)) != NULL && v->q_count < MAX_PACKETS) {
        packet *p = &v->q[(v->q_head + v->q_count) % MAX_PACKETS];
        p->addr = addr;
        p->bytes = bytes;
        p->tag = tag;
        v->queued = true;
        if (v->q_count++ == 0) {
            v->pos = 0;
            load_head(v);
        }
        ok = true;
    }
    SDL_UnlockMutex(lock);
    return ok;
}

int pdo_voice_queued(int id)
{
    int n = 0;
    WITH_VOICE(id, n = v->q_count);
    return n;
}

void pdo_voice_flush(int id)
{
    WITH_VOICE(id, while (v->q_count) pop_head(v, true); v->pos = 0);
}
