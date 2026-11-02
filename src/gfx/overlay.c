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
#include "../input_map.h"

#include <stdio.h>
#include <string.h>

enum { ITEM_SCALE, ITEM_MSAA, ITEM_ASPECT, ITEM_FIT, ITEM_SOFT, ITEM_VOLUME, ITEM_FPS, ITEM_VSH, ITEM_FVF, ITEM_INTERP,
       ITEM_CONTROLS, ITEM_CLOSE, ITEM_COUNT };

/* The Controls page: a line per Xbox button, then these two. While a line
 * waits for its new key or controller button, capture is set. */
enum { CTL_MOUSE = PDO_IN_COUNT, CTL_MSPEED, CTL_RESET, CTL_BACK, CTL_COUNT };
static bool controls, capture;

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
    if (on) {
        selected = 0;
        controls = capture = false;
    }
    else {
        swallow = true;
        pdo_settings_save();
    }
}

/* Anti-aliasing steps: off, FXAA (cheap, on the finished frame), then
 * MSAA 2x, 4x, 8x. */
static void next_aa(pdo_settings *s, int dir)
{
    static const struct { int msaa; bool fxaa; } steps[] = { { 1, false }, { 1, true }, { 2, false }, { 4, false },
                                                              { 8, false } };
    int i = 0, n = (int)SDL_arraysize(steps);
    while (i < n - 1 && !(steps[i].msaa == s->msaa && steps[i].fxaa == (s->fxaa && s->msaa == 1)))
        i++;
    i = SDL_clamp(i + dir, 0, n - 1);
    s->msaa = steps[i].msaa;
    s->fxaa = steps[i].fxaa;
}

/* Change the selected line by one step: dir -1 or +1. */
static void change(int dir)
{
    pdo_settings *s = pdo_settings_get();

    switch (selected) {
    case ITEM_SCALE:  s->render_scale = SDL_clamp(s->render_scale + dir, 1, 4); break;
    case ITEM_MSAA:   next_aa(s, dir); break;
    case ITEM_ASPECT: s->aspect = (s->aspect + PDO_ASPECT_COUNT + dir) % PDO_ASPECT_COUNT; break;
    case ITEM_FIT:    s->fit = (s->fit + PDO_FIT_COUNT + dir) % PDO_FIT_COUNT; break;
    case ITEM_SOFT:   s->soft_filter = !s->soft_filter; break;
    case ITEM_VOLUME: s->master_volume = SDL_clamp(s->master_volume + 5 * dir, 0, 100); break;
    case ITEM_FPS:    s->show_fps = !s->show_fps; break;
    case ITEM_VSH:    s->gpu_vertex = !s->gpu_vertex; break;
    case ITEM_FVF:    s->gpu_fixed = !s->gpu_fixed; break;
    case ITEM_INTERP: s->frame_interp = !s->frame_interp; break;
    case ITEM_CONTROLS: if (dir > 0) { controls = true; selected = 0; } break;
    case ITEM_CLOSE:  if (dir > 0) set_open(false); break;
    }
    dirty = true;
}

static void move(int dir)
{
    int n = controls ? CTL_COUNT : ITEM_COUNT;
    selected = (selected + n + dir) % n;
    dirty = true;
}

/* Controls page: OK on a button waits for its new key or controller input;
 * Left clears the line's key and controller input. */
static void control_act(int a, int dir)
{
    pdo_settings *st = pdo_settings_get();

    if (selected == CTL_MOUSE) {
        st->mouse_aim = !st->mouse_aim;
    } else if (selected == CTL_MSPEED) {
        st->mouse_speed = SDL_clamp(st->mouse_speed + 10 * dir, 10, 400);
    } else if (selected == CTL_BACK || (a < 0 && dir < 0 && selected >= PDO_IN_COUNT)) {
        controls = false;
        selected = ITEM_CONTROLS;
    } else if (selected == CTL_RESET) {
        if (dir > 0)
            pdo_input_reset();
    } else if (dir > 0) {
        capture = true;
    } else {
        pdo_input_bindings()[selected].key = SDL_SCANCODE_UNKNOWN;
        pdo_input_bindings()[selected].pad = -1;
    }
    dirty = true;
}

/* A gamepad button or key, mapped to a menu action. */
enum { ACT_NONE, ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_OK, ACT_BACK };

static void act(int a)
{
    if (controls) {
        switch (a) {
        case ACT_UP:    move(-1); break;
        case ACT_DOWN:  move(+1); break;
        case ACT_LEFT:  control_act(-1, -1); break;
        case ACT_RIGHT: case ACT_OK: control_act(0, +1); break;
        case ACT_BACK:  controls = false; selected = ITEM_CONTROLS; dirty = true; break;
        }
        return;
    }
    switch (a) {
    case ACT_UP:    move(-1); break;
    case ACT_DOWN:  move(+1); break;
    case ACT_LEFT:  change(-1); break;
    case ACT_RIGHT: change(+1); break;
    case ACT_OK:    change(+1); break;
    case ACT_BACK:  set_open(false); break;
    }
}

/* While a Controls line waits: the next key or controller input is its new
 * binding (Escape cancels). True: the event was taken. */
static bool capture_event(const SDL_Event *ev)
{
    pdo_binding *b = &pdo_input_bindings()[selected];

    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        if (ev->key.key != SDLK_ESCAPE && ev->key.key != SDLK_F1)
            b->key = ev->key.scancode;
    } else if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b->pad = ev->gbutton.button;
    } else if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
               (ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
               ev->gaxis.value > 16000) {
        b->pad = PDO_PAD_AXIS + ev->gaxis.axis;
    } else {
        return ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP || ev->type == SDL_EVENT_KEY_UP ||
               ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    }
    capture = false;
    dirty = true;
    return true;
}

bool pdo_overlay_event(const SDL_Event *ev)
{
    if (open_ && capture)
        return capture_event(ev);
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

static void draw_controls(void)
{
    const pdo_binding *b = pdo_input_bindings();
    const float x0 = 4, y0 = 1, w = 34, h = 2 + CTL_COUNT + 4;
    SDL_FRect panel = { (x0 - 1) * CHAR, (y0 - 1) * CHAR, (w + 2) * CHAR, (h + 1) * CHAR };

    SDL_SetRenderDrawColor(painter, 10, 14, 24, 220);
    SDL_RenderFillRect(painter, &panel);
    SDL_SetRenderDrawColor(painter, 120, 170, 220, 255);
    SDL_RenderRect(painter, &panel);
    SDL_SetRenderDrawColor(painter, 230, 235, 245, 255);
    text(x0, y0, "Controls        key        pad");
    for (int i = 0; i < CTL_COUNT; i++) {
        float y = y0 + 2 + i;
        char line[64];
        if (i < PDO_IN_COUNT) {
            const char *k = b[i].key != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(b[i].key) : "-";
            snprintf(line, sizeof line, "%-10s %-10.10s %.10s", pdo_input_button_name(i), k[0] ? k : "?",
                     pdo_input_pad_name(b[i].pad));
            if (i == selected && capture)
                snprintf(line, sizeof line, "%-10s press a key or button", pdo_input_button_name(i));
        } else if (i == CTL_MOUSE) {
            snprintf(line, sizeof line, "%-10s %s", "Mouse aim", pdo_settings_get()->mouse_aim ? "On" : "Off");
        } else if (i == CTL_MSPEED) {
            snprintf(line, sizeof line, "%-10s %d%%", "Mouse spd", pdo_settings_get()->mouse_speed);
        } else {
            snprintf(line, sizeof line, "%s", i == CTL_RESET ? "Reset to defaults" : "Back");
        }
        if (i == selected) {
            SDL_FRect bar = { (x0 - 0.5f) * CHAR, (y - 0.0f) * CHAR - 1, (w + 1) * CHAR, CHAR + 2 };
            SDL_SetRenderDrawColor(painter, 60, 100, 160, 255);
            SDL_RenderFillRect(painter, &bar);
            SDL_SetRenderDrawColor(painter, 255, 255, 255, 255);
            text(x0, y, ">");
        } else
            SDL_SetRenderDrawColor(painter, 200, 205, 215, 255);
        text(x0 + 2, y, line);
    }
    SDL_SetRenderDrawColor(painter, 150, 160, 175, 255);
    text(x0, y0 + h - 2, "A/Enter: rebind  Left: clear");
    text(x0, y0 + h - 1, "B/Esc: back   Mouse: Esc frees it");
}

static void draw_menu(void)
{
    const pdo_settings *s = pdo_settings_get();
    static const char *aspects[PDO_ASPECT_COUNT] = { "4:3", "16:9" };
    static const char *fits[PDO_FIT_COUNT] = { "Bars", "Stretch", "Zoom" };
    char lines[ITEM_COUNT][48];
    const float x0 = 4, y0 = 1, w = 32, h = 2 + ITEM_COUNT * 2 + 3;    /* fits the 240 rows */
    SDL_FRect panel = { (x0 - 1) * CHAR, (y0 - 1) * CHAR, (w + 2) * CHAR, (h + 1) * CHAR };

    snprintf(lines[ITEM_SCALE], sizeof lines[0], "%-15s%dx", "Resolution", s->render_scale);
    if (s->msaa > 1)
        snprintf(lines[ITEM_MSAA], sizeof lines[0], "%-15s%dx MSAA", "Anti-aliasing", s->msaa);
    else if (s->fxaa)
        snprintf(lines[ITEM_MSAA], sizeof lines[0], "%-15sFXAA", "Anti-aliasing");
    else
        snprintf(lines[ITEM_MSAA], sizeof lines[0], "%-15sOff", "Anti-aliasing");
    /* The game reads widescreen once, at start: say so until it has. */
    snprintf(lines[ITEM_ASPECT], sizeof lines[0], "%-15s%s%s", "Aspect ratio", aspects[s->aspect],
             (s->aspect == PDO_ASPECT_16_9) != pdo_widescreen_active() ? " (restart)" : "");
    snprintf(lines[ITEM_FIT], sizeof lines[0], "%-15s%s", "Fit to window", fits[s->fit]);
    snprintf(lines[ITEM_SOFT], sizeof lines[0], "%-15s%s", "TV softening", s->soft_filter ? "On" : "Off");
    snprintf(lines[ITEM_VOLUME], sizeof lines[0], "%-15s%d%%", "Volume", s->master_volume);
    snprintf(lines[ITEM_FPS], sizeof lines[0], "%-15s%s", "Show FPS", s->show_fps ? "On" : "Off");
    snprintf(lines[ITEM_VSH], sizeof lines[0], "%-15s%s", "Vertex progs", s->gpu_vertex ? "GPU" : "CPU");
    snprintf(lines[ITEM_FVF], sizeof lines[0], "%-15s%s", "Fixed-func", s->gpu_fixed ? "GPU" : "CPU");
    snprintf(lines[ITEM_INTERP], sizeof lines[0], "%-15s%s", "Frame interp", s->frame_interp ? "On" : "Off");
    snprintf(lines[ITEM_CONTROLS], sizeof lines[0], "Controls...");
    snprintf(lines[ITEM_CLOSE], sizeof lines[0], "Close");

    SDL_SetRenderDrawColor(painter, 10, 14, 24, 220);
    SDL_RenderFillRect(painter, &panel);
    SDL_SetRenderDrawColor(painter, 120, 170, 220, 255);
    SDL_RenderRect(painter, &panel);
    SDL_SetRenderDrawColor(painter, 230, 235, 245, 255);
    text(x0, y0, "Orta Recomp options");
    for (int i = 0; i < ITEM_COUNT; i++) {
        float y = y0 + 2 + i * 2;
        if (i == selected) {
            SDL_FRect bar = { (x0 - 0.5f) * CHAR, (y - 0.5f) * CHAR, (w + 1) * CHAR, 2 * CHAR };
            SDL_SetRenderDrawColor(painter, 60, 100, 160, 255);
            SDL_RenderFillRect(painter, &bar);
            SDL_SetRenderDrawColor(painter, 255, 255, 255, 255);
            text(x0, y, ">");
        } else
            SDL_SetRenderDrawColor(painter, 200, 205, 215, 255);
        text(x0 + 2, y, lines[i]);
        if (i == selected && i != ITEM_CLOSE && i != ITEM_CONTROLS)
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
        dirty = true;
    }
    {   /* The layer is shown with the game's frame: in its widescreen mode
         * that is stretched to 16:9, so draw a quarter narrower to come out
         * right (the FPS box and the menu were widened over the picture). */
        static int wide = -1;
        if (wide != (int)pdo_widescreen_active()) {
            wide = pdo_widescreen_active();
            SDL_SetRenderScale(painter, SCALE * (wide ? 0.75f : 1.0f), SCALE);
            dirty = true;
        }
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
    if (open_ && controls)
        draw_controls();
    else if (open_)
        draw_menu();
    SDL_FlushRenderer(painter);
    dirty = false;
    *changed = true;
    return image;
}
