#ifndef PDO_MIXER_H
#define PDO_MIXER_H

/*
 * Software voice mixer: what the Xbox APU did for DirectSound, on SDL3 audio.
 *
 * Voices read their sample data straight out of guest memory (the game owns
 * the buffers and may rewrite them while they play, as streamed music does).
 * Everything is mixed in float at 48 kHz into the Xbox's speaker mixbins and
 * then folded to the output layout, so channel routing follows what the game
 * asked for rather than what a stereo buffer "usually" means.
 */

#include <stdbool.h>
#include <stdint.h>

#define PDO_MIX_RATE 48000

/* Xbox DSMIXBIN_* speaker bins. Higher bins are effect sends. */
enum {
    PDO_BIN_FRONT_LEFT = 0,
    PDO_BIN_FRONT_RIGHT = 1,
    PDO_BIN_FRONT_CENTER = 2,
    PDO_BIN_LOW_FREQUENCY = 3,
    PDO_BIN_BACK_LEFT = 4,
    PDO_BIN_BACK_RIGHT = 5,
    PDO_BIN_SPEAKERS = 6,
};

typedef enum pdo_format {
    PDO_FMT_PCM8,
    PDO_FMT_PCM16,
    PDO_FMT_XADPCM,     /* WAVE_FORMAT_XBOX_ADPCM (0x0069) */
} pdo_format;

#define PDO_MAX_VOICE_BINS 8

typedef struct pdo_voice_bin {
    uint32_t bin;
    int32_t  volume;    /* hundredths of a dB, <= 0 */
} pdo_voice_bin;

typedef struct pdo_voice_desc {
    pdo_format format;
    int        channels;            /* 1 or 2 */
    uint32_t   sample_rate;
    uint32_t   samples_per_block;   /* ADPCM only */
    uint32_t   data;                /* guest address, 0 until known */
    uint32_t   bytes;
    int        bin_count;
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    bool       positional;          /* DSBCAPS_CTRL3D */
} pdo_voice_desc;

bool pdo_mixer_init(void);
void pdo_mixer_shutdown(void);

int  pdo_voice_create(const pdo_voice_desc *desc);  /* returns id > 0, or 0 */
void pdo_voice_destroy(int id);

/* All byte offsets and sizes are in the buffer's own format, as the game
 * sees them. */
void pdo_voice_set_data(int id, uint32_t guest_addr, uint32_t bytes);
void pdo_voice_play(int id, bool looping);
void pdo_voice_stop(int id);
bool pdo_voice_playing(int id, bool *looping);
void pdo_voice_set_position(int id, uint32_t byte_offset);
uint32_t pdo_voice_position(int id);
void pdo_voice_set_loop_region(int id, uint32_t start, uint32_t length);
void pdo_voice_set_volume(int id, int32_t hundredths_db);
void pdo_voice_set_headroom(int id, uint32_t hundredths_db);
void pdo_voice_set_frequency(int id, uint32_t hz);   /* 0 = the format's rate */
void pdo_voice_set_bins(int id, const pdo_voice_bin *bins, int count);

uint32_t pdo_voice_block_align(int id);

#endif
