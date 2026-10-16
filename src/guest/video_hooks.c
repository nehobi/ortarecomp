/*
 * Display and video hooks the xboxrecomp kernel layer calls.
 *
 * Upstream implements these in its xbox_video library (a Win32/Media
 * Foundation window). We don't build that library: the display will be our
 * own SDL3 presenter (milestone 2). Until then these record what the kernel
 * asks for and log it once, so a call is visible rather than silently lost.
 */

#include <SDL3/SDL.h>

#include <stdint.h>

static uint32_t fb_va, fb_pitch;

static void once(const char *what)
{
    SDL_Log("video: %s requested (not implemented yet)", what);
}

int xbox_VideoPlayFile(const char *host_path)
{
    static bool logged;
    if (!logged) {
        logged = true;
        SDL_Log("video: title asked to play %s (FMV not implemented yet)", host_path);
    }
    return -1;
}

int xbox_VideoIsPlaying(void)
{
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    static bool logged;
    if (!logged) {
        logged = true;
        once("framebuffer window");
    }
}

void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch)
{
    fb_va = va;
    fb_pitch = pitch;
}

void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    fb_va = va;
    fb_pitch = pitch;
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
}
