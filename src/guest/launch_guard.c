/*
 * Pandora's Box's PANZER DRAGOON, which we cannot play yet.
 *
 * The original Panzer Dragoon on the Orta disc is another program,
 * d:\pd1.xbe. Leaving the Box (0x000B8080) takes the next scene from the
 * Box object (pointer at 0x00474058, +0x7D4); -2 means that program: it
 * stores -2 as the next scene (0x00342718) and writes the scene table at
 * that index, takes everything down, saves, and tail-jumps to 0x000120D0,
 * which shuts down sound and graphics and calls XLaunchNewImageA. Only
 * default.xbe is recompiled, so there is nothing to launch, and the game
 * cannot run on after that.
 *
 * So the Box leaves for the title instead (scene 10, as it is entered at
 * boot): the same path as EXIT, nothing taken down. A stretch goal is to
 * recompile pd1.xbe too and launch it.
 */

#include <SDL3/SDL.h>

#include "recomp_types.h"

#define BOX_OBJECT      0x00474058u
#define BOX_NEXT_SCENE  0x7D4u
#define SCENE_PD1       0xFFFFFFFEu     /* -2: launch d:\pd1.xbe */
#define SCENE_TITLE     10u

/* Pandora's Box: leaving it (ret) */
void sub_000B8080_enter(void)
{
    uint32_t box = MEM32(BOX_OBJECT);

    if (box && MEM32(box + BOX_NEXT_SCENE) == SCENE_PD1) {
        SDL_Log("launch: PANZER DRAGOON (d:\\pd1.xbe) is not recompiled yet; back to the title");
        MEM32(box + BOX_NEXT_SCENE) = SCENE_TITLE;
    }
}
