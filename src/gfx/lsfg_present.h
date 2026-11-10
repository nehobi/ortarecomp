/*
 * Lossless Scaling frame generation (lib/lsfg) on SDL's GPU device.
 *
 * lib/lsfg records into SDL's own Vulkan command buffers, on SDL's device,
 * through the handles patches/SDL gives out: no second device, no copies
 * through the CPU. The renderer draws each game frame's window picture
 * into a texture, has the frames between it and the last one generated
 * (pdo_lsfg_record), then shows those and the frame itself, each copied
 * into a swap chain image of its own (pdo_lsfg_record_output).
 */
#ifndef PDO_LSFG_PRESENT_H
#define PDO_LSFG_PRESENT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A GPU device with what the frame generation shaders need (the Vulkan
 * memory model, storage image writes without a format); NULL if the GPU
 * lacks them, then make a plain one. */
SDL_GPUDevice *pdo_lsfg_create_device(void);

/* Takes SDL's Vulkan objects and Lossless Scaling's shaders (translated
 * from dll, a moment the first time; cached in cache). False, with the
 * reason in pdo_lsfg_status(), when it cannot run. */
bool pdo_lsfg_start(SDL_GPUDevice *dev, const char *dll, const char *cache);
/* The GPU must be idle. */
void pdo_lsfg_stop(void);

/* multiplier: pictures per game frame (2-4); performance: Lossless
 * Scaling's performance mode. Taken up at the next game frame. */
void pdo_lsfg_configure(int multiplier, bool performance);

/* Records taking in a game frame (src: COLOR_TARGET only, w x h, drawn
 * in this command buffer or before) and generating the frames between it
 * and the last. The number generated, 0 for the first. Between passes. */
uint32_t pdo_lsfg_record(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t w, uint32_t h);

/* After the GPU has done pdo_lsfg_record's work: whether the frame cut to
 * another scene, or is the one after (show it alone: frames generated
 * across a cut morph one picture into the other). */
bool pdo_lsfg_scene_cut(void);

/* Time between the pictures shown for a game frame (nanoseconds): the
 * game frame interval measured so far over shown. Once per game frame. */
uint64_t pdo_lsfg_spacing(uint32_t shown);

/* Records copying generated frame index (0 the earliest), or with index
 * -1 the game frame itself (src as given to pdo_lsfg_record), to dst, a
 * swap chain texture of w x h. Between passes. */
void pdo_lsfg_record_output(SDL_GPUCommandBuffer *cmd, int index, SDL_GPUTexture *src, uint32_t sw, uint32_t sh,
                            SDL_GPUTexture *dst, uint32_t w, uint32_t h);

/* Why it is not running, or what it last went wrong with; "" when fine. */
const char *pdo_lsfg_status(void);

/* Where Lossless.dll is looked for when the settings name none: the app's
 * own folder's lossless_scaling\Lossless.dll, then Steam's default library.
 * The first that exists goes to out; false if none does. */
bool pdo_lsfg_find_dll(const char *data_dir, char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
