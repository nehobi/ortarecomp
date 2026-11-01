/*
 * Widescreen: Panzer Dragoon Orta's own 16:9 mode.
 *
 * The display setup (0x0003D020) asks the game's display object (at
 * 0x0041A06C) for its video mode, query 15, and with bit 8 of the answer
 * renders anamorphic 16:9: the pixel aspect it keeps at 0x00342724 times
 * 4/3, and a flag at 0x00342754 that the camera and the screen layout read
 * (the HUD's round radar comes out narrow, to be stretched back). Our
 * kernel's video flags never set that bit, so the game stayed 4:3. With
 * the aspect setting at 16:9 we set it, and the window pass shows the
 * frame at 16:9 (renderer.c, present_to_window).
 */

#include <SDL3/SDL.h>

#include "recomp_types.h"
#include "config.h"

static bool active;     /* the game was told widescreen, at its setup */

bool pdo_widescreen_active(void)
{
    return active;
}

/* The display object's query (this, id, arg; ret 0xC): id 15 returns its
 * video mode (low byte: 1 or 2, the pixel aspect; bit 8: widescreen). */
extern void sub_00281330_gen(void);
void sub_00281330(void)
{
    uint32_t id = MEM32(g_esp + 8);

    sub_00281330_gen();
    if (id == 15 && pdo_settings_get()->aspect == PDO_ASPECT_16_9) {
        g_eax |= 0x100;
        if (!active)
            SDL_Log("widescreen: the game renders 16:9 (anamorphic)");
        active = true;
    }
}
