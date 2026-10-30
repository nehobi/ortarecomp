/*
 * Options overlay and frame counter.
 *
 * F1, or Back and Start together on a controller, opens the menu; the game
 * waits while it is open (the renderer holds it in Swap). Up/Down choose a
 * line, Left/Right (or A/Enter) change it, B/Escape/F1 close it. Changes take
 * effect at once (render scale and anti-aliasing from the next game frame)
 * and are saved when the menu closes.
 *
 * Text is SDL's built-in 8x8 debug font, drawn by a software renderer into a
 * surface, so no font or GPU code of our own is needed.
 */

#include "overlay.h"

#include "../config.h"

#include <stdio.h>
#include <string.h>

enum { ITEM_SCALE, ITEM_MSAA, ITEM_ASPECT, ITEM_SOFT, ITEM_VOLUME, ITEM_FPS, ITEM_VSH, ITEM_FVF, ITEM_INTERP, ITEM_CLOSE, ITEM_COUNT };

static bool open_, dirty = true, swallow;
static int selected;
static bool start_held, back_held;
static SDL_Surface *image;
static SDL_Renderer *painter;

/* Frame counter: frames presented in the last whole second. */
static Uint64 count_start;
static int counted, fps = -1;

bool pdo_overlay_open(void)
{
    return open_;
}

static void set_open(bool on)
{
    if (open_ == on)
        return;
    open_ = on;
    dirty = true;
    if (on)
        selected = 0;
    else {
        swallow = true;
        pdo_settings_save();
    }
}

static int next_msaa(int m, int dir)
{
    static const int steps[] = { 1, 2, 4, 8 };
    int i = 0;
    while (i < 3 && steps[i] != m)
        i++;
    i += dir;
    return steps[i < 0 ? 0 : i > 3 ? 3 : i];
}

/* Change the selected line by one step: dir -1 or +1. */
static void change(int dir)
{
    pdo_settings *s = pdo_settings_get();

    switch (selected) {
    case ITEM_SCALE:  s->render_scale = SDL_clamp(s->render_scale + dir, 1, 4); break;
    case ITEM_MSAA:   s->msaa = next_msaa(s->msaa, dir); break;
    case ITEM_ASPECT: s->aspect = (s->aspect + PDO_ASPECT_COUNT + dir) % PDO_ASPECT_COUNT; break;
    case ITEM_SOFT:   s->soft_filter = !s->soft_filter; break;
    case ITEM_VOLUME: s->master_volume = SDL_clamp(s->master_volume + 5 * dir, 0, 100); break;
    case ITEM_FPS:    s->show_fps = !s->show_fps; break;
    case ITEM_VSH:    s->gpu_vertex = !s->gpu_vertex; break;
    case ITEM_FVF:    s->gpu_fixed = !s->gpu_fixed; break;
    case ITEM_INTERP: s->frame_interp = !s->frame_interp; break;
    case ITEM_CLOSE:  if (dir > 0) set_open(false); break;
    }
    dirty = true;
}

static void move(int dir)
{
    selected = (selected + ITEM_COUNT + dir) % ITEM_COUNT;
    dirty = true;
}

/* A gamepad button or key, mapped to a menu action. */
enum { ACT_NONE, ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_OK, ACT_BACK };

static void act(int a)
{
    switch (a) {
    case ACT_UP:    move(-1); break;
    case ACT_DOWN:  move(+1); break;
    case ACT_LEFT:  change(-1); break;
    case ACT_RIGHT: change(+1); break;
    case ACT_OK:    change(+1); break;
    case ACT_BACK:  set_open(false); break;
    }
}

bool pdo_overlay_event(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode k = ev->key.key;
        if (k == SDLK_F1 && !ev->key.repeat) {
            set_open(!open_);
            return true;
        }
        if (!open_)
            return false;
        act(k == SDLK_UP ? ACT_UP : k == SDLK_DOWN ? ACT_DOWN : k == SDLK_LEFT ? ACT_LEFT
            : k == SDLK_RIGHT ? ACT_RIGHT : k == SDLK_RETURN || k == SDLK_SPACE ? ACT_OK
            : k == SDLK_ESCAPE || k == SDLK_BACKSPACE ? ACT_BACK : ACT_NONE);
        return true;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        bool down = ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        Uint8 b = ev->gbutton.button;

        if (b == SDL_GAMEPAD_BUTTON_START)
            start_held = down;
        if (b == SDL_GAMEPAD_BUTTON_BACK)
            back_held = down;
        if (down && start_held && back_held) {
            set_open(!open_);
            return true;
        }
        if (!open_ || !down)
            return open_;
        act(b == SDL_GAMEPAD_BUTTON_DPAD_UP ? ACT_UP : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? ACT_DOWN
            : b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? ACT_LEFT : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? ACT_RIGHT
            : b == SDL_GAMEPAD_BUTTON_SOUTH ? ACT_OK : b == SDL_GAMEPAD_BUTTON_EAST ? ACT_BACK : ACT_NONE);
        return true;
    }
    return false;
}

void pdo_overlay_frame(void)
{
    Uint64 now = SDL_GetTicks();

    if (!count_start)
        count_start = now;
    counted++;
    if (now - count_start >= 1000) {
        int f = (int)((Uint64)counted * 1000 / (now - count_start));
        if (f != fps)
            dirty = true;
        fps = f;
        counted = 0;
        count_start = now;
    }
}

bool pdo_overlay_visible(void)
{
    return open_ || pdo_settings_get()->show_fps;
}

bool pdo_overlay_blocks_input(bool any_pressed)
{
    if (open_)
        return true;
    if (swallow && any_pressed)
        return true;
    swallow = false;
    return false;
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

#define SCALE 2.0f      /* 8x8 font at 16x16 on the 640x480 image */
#define CHAR  8.0f      /* in unscaled units */

static void text(float col, float row, const char *s)
{
    SDL_RenderDebugText(painter, col * CHAR, row * CHAR, s);
}

static void draw_menu(void)
{
    const pdo_settings *s = pdo_settings_get();
    static const char *aspects[PDO_ASPECT_COUNT] = { "4:3", "Stretch" };
    char lines[ITEM_COUNT][48];
    const float x0 = 4, y0 = 6, w = 32, h = 3 + ITEM_COUNT * 2 + 3;
    SDL_FRect panel = { (x0 - 1) * CHAR, (y0 - 1) * CHAR, (w + 2) * CHAR, (h + 1) * CHAR };

    snprintf(lines[ITEM_SCALE], sizeof lines[0], "%-15s%dx", "Resolution", s->render_scale);
    if (s->msaa > 1)
        snprintf(lines[ITEM_MSAA], sizeof lines[0], "%-15s%dx", "Anti-aliasing", s->msaa);
    else
        snprintf(lines[ITEM_MSAA], sizeof lines[0], "%-15sOff", "Anti-aliasing");
    snprintf(lines[ITEM_ASPECT], sizeof lines[0], "%-15s%s", "Aspect ratio", aspects[s->aspect]);
    snprintf(lines[ITEM_SOFT], sizeof lines[0], "%-15s%s", "TV softening", s->soft_filter ? "On" : "Off");
    snprintf(lines[ITEM_VOLUME], sizeof lines[0], "%-15s%d%%", "Volume", s->master_volume);
    snprintf(lines[ITEM_FPS], sizeof lines[0], "%-15s%s", "Show FPS", s->show_fps ? "On" : "Off");
    snprintf(lines[ITEM_VSH], sizeof lines[0], "%-15s%s", "Vertex progs", s->gpu_vertex ? "GPU" : "CPU");
    snprintf(lines[ITEM_FVF], sizeof lines[0], "%-15s%s", "Fixed-func", s->gpu_fixed ? "GPU" : "CPU");
    snprintf(lines[ITEM_INTERP], sizeof lines[0], "%-15s%s", "Frame interp", s->frame_interp ? "On" : "Off");
    snprintf(lines[ITEM_CLOSE], sizeof lines[0], "Close");

    SDL_SetRenderDrawColor(painter, 10, 14, 24, 220);
    SDL_RenderFillRect(painter, &panel);
    SDL_SetRenderDrawColor(painter, 120, 170, 220, 255);
    SDL_RenderRect(painter, &panel);
    SDL_SetRenderDrawColor(painter, 230, 235, 245, 255);
    text(x0, y0, "Orta Recomp options");
    for (int i = 0; i < ITEM_COUNT; i++) {
        float y = y0 + 3 + i * 2;
        if (i == selected) {
            SDL_FRect bar = { (x0 - 0.5f) * CHAR, (y - 0.5f) * CHAR, (w + 1) * CHAR, 2 * CHAR };
            SDL_SetRenderDrawColor(painter, 60, 100, 160, 255);
            SDL_RenderFillRect(painter, &bar);
            SDL_SetRenderDrawColor(painter, 255, 255, 255, 255);
            text(x0, y, ">");
        } else
            SDL_SetRenderDrawColor(painter, 200, 205, 215, 255);
        text(x0 + 2, y, lines[i]);
        if (i == selected && i != ITEM_CLOSE)
            text(x0 + w - 4, y, "< >");
    }
    SDL_SetRenderDrawColor(painter, 150, 160, 175, 255);
    text(x0, y0 + h - 2, "Arrows/D-pad, B or F1 closes");
}

SDL_Surface *pdo_overlay_image(bool *changed)
{
    *changed = false;
    if (!pdo_overlay_visible())
        return NULL;
    if (!image) {
        image = SDL_CreateSurface(PDO_OVERLAY_W, PDO_OVERLAY_H, SDL_PIXELFORMAT_RGBA32);
        painter = image ? SDL_CreateSoftwareRenderer(image) : NULL;
        if (!painter) {
            SDL_Log("overlay: cannot draw (%s)", SDL_GetError());
            return NULL;
        }
        SDL_SetRenderDrawBlendMode(painter, SDL_BLENDMODE_NONE);
        SDL_SetRenderScale(painter, SCALE, SCALE);
        dirty = true;
    }
    if (!dirty)
        return image;
    SDL_SetRenderDrawColor(painter, 0, 0, 0, 0);
    SDL_RenderClear(painter);
    if (pdo_settings_get()->show_fps) {
        char buf[32];
        SDL_FRect bg = { 0, 0, 10 * CHAR, 2 * CHAR };
        snprintf(buf, sizeof buf, fps < 0 ? "-- fps" : "%d fps", fps);
        SDL_SetRenderDrawColor(painter, 0, 0, 0, 160);
        SDL_RenderFillRect(painter, &bg);
        SDL_SetRenderDrawColor(painter, 255, 255, 0, 255);
        text(0.5f, 0.5f, buf);
    }
    if (open_)
        draw_menu();
    SDL_FlushRenderer(painter);
    dirty = false;
    *changed = true;
    return image;
}
