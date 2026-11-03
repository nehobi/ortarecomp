/*
 * The boot logos, skipped with a press (the skip_logos setting, on by
 * default; as Concept of Love does for JSRF).
 *
 * The title screen is an object (pointer at 0x004741B0) whose state at
 * +0x1A0 runs 0 Sega, 1 Smilebit, 2 Sofdec, then 3 the title. Each frame
 * 0x000C0940 calls the state's handler (0x000BF690, 0x000BF740,
 * 0x000BF7F0) and moves to the state it returns. A card's steps, at +0x10:
 * 0 fades in and starts the logo's sprite animation (kept at +0x34, +0x38,
 * +0x3C); 2 waits for the animation's time (a float three pointers down:
 * sprite +0x24, +0x50, +8) to reach the card's end (300.0 at 0x00314D88
 * for the first two, the one at 0x00315150 for Sofdec), then fades out; 3
 * waits for the fade, frees the sprite and returns the next card.
 *
 * Jumping straight to the title left a card's sprite up and the title
 * without its logo: what the cards set up is needed. So a press of A or
 * Start instead puts the animation at its end, and every card after it
 * too: each fades out at once and the next follows the console's own path.
 * On the console a press does nothing here.
 */

#include <stdbool.h>

#include <SDL3/SDL.h>

#include "recomp_types.h"
#include "config.h"

#define TITLE_STATE     0x1A0
#define TITLE_STEP      0x10
#define STATE_TITLE     3
#define STEP_SHOWING    2

static const uint32_t sprite_at[STATE_TITLE] = { 0x34, 0x38, 0x3C };
static const uint32_t end_at[STATE_TITLE] = { 0x00314D88, 0x00314D88, 0x00315150 };

bool pdo_input_skip_press(void);

/* The title screen's frame (thiscall, ecx = the title object; ret) */
void sub_000C0940_enter(void)
{
    static bool skipping;
    uint32_t o = g_ecx, card, sprite, anim;

    if (!pdo_settings_get()->skip_logos)
        return;
    card = MEM32(o + TITLE_STATE);
    if (card >= STATE_TITLE) {
        skipping = false;
        return;
    }
    if (!skipping && pdo_input_skip_press()) {
        skipping = true;
        SDL_Log("intro: logos skipped from card %u", card);
    }
    if (!skipping || MEM32(o + TITLE_STEP) != STEP_SHOWING)
        return;
    sprite = MEM32(o + sprite_at[card]);
    anim = sprite ? MEM32(sprite + 0x24) : 0;
    anim = anim ? MEM32(anim + 0x50) : 0;
    if (anim)
        MEM32(anim + 8) = MEM32(end_at[card]);  /* the float's bits */
}
