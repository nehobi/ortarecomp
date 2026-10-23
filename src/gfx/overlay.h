#ifndef PDO_GFX_OVERLAY_H
#define PDO_GFX_OVERLAY_H

/* The options overlay (F1, or Back+Start on a controller) and the frame
 * counter. Drawn on the CPU into a 640x480 RGBA image that the renderer lays
 * over the game picture. */

#include <SDL3/SDL.h>

#include <stdbool.h>

#define PDO_OVERLAY_W 640
#define PDO_OVERLAY_H 480

/* An event for the overlay: true when it took it (the menu is open, or the
 * key or buttons that open it). */
bool pdo_overlay_event(const SDL_Event *ev);

/* The menu is open: the game waits. */
bool pdo_overlay_open(void);

/* Count a presented frame (for the frame counter). */
void pdo_overlay_frame(void);

/* Anything to draw: the menu or the frame counter. */
bool pdo_overlay_visible(void);

/* The overlay image, redrawn if it changed; *changed says whether it did.
 * NULL when nothing is visible. */
SDL_Surface *pdo_overlay_image(bool *changed);

/* Input for the game, from the input layer: true means hand the game an idle
 * pad. Blocks while the menu is open and, after it closes, until every key and
 * button the game reads is released, so the press that closed it does not
 * reach the game. any_pressed: the game's pad state has anything held. */
bool pdo_overlay_blocks_input(bool any_pressed);

#endif
