#ifndef PDO_CONFIG_H
#define PDO_CONFIG_H

#include <stdbool.h>

#define PDO_PATH_MAX 1024

typedef struct pdo_config {
    char game_dir[PDO_PATH_MAX];    /* folder holding default.xbe and media/ */
} pdo_config;

/* Options the overlay menu (F1) changes; saved with the rest. */
typedef struct pdo_settings {
    int  render_scale;      /* 1-4: render targets at this multiple of the game's size */
    int  msaa;              /* 1, 2, 4 or 8 samples */
    int  aspect;            /* PDO_ASPECT_*; 16:9 is the game's own widescreen mode */
    int  fit;               /* PDO_FIT_*: how the picture fills the window */
    bool soft_filter;       /* TV-like vertical softening at presentation */
    int  master_volume;     /* 0-100 % */
    bool show_fps;
    bool gpu_vertex;        /* vertex programs on the GPU (shaders/vsh.vert), not the CPU */
    bool gpu_fixed;         /* fixed-function vertices on the GPU (shaders/fvf.vert) */
    bool frame_interp;      /* frames between the game's 60, blended (renderer.c) */
    bool fullscreen;        /* borderless, the whole monitor (Alt+Enter) */
    bool fxaa;              /* edge smoothing on the finished frame (shaders/screen.frag) */
    int  win_x, win_y, win_w, win_h;    /* the window when not fullscreen (w 0: not set yet) */
    bool win_maximized;
    bool mouse_aim;         /* the mouse steers the left stick (input_hle.c) */
    int  mouse_speed;       /* 10-400 % */
    bool unlock_box;        /* every Pandora's Box item open, the save untouched (src/guest/unlocks.c) */
} pdo_settings;

enum { PDO_ASPECT_4_3, PDO_ASPECT_16_9, PDO_ASPECT_COUNT };
/* The whole picture with bars; stretched to the window; zoomed to fill it,
 * the overflow cut off. */
enum { PDO_FIT_BARS, PDO_FIT_STRETCH, PDO_FIT_ZOOM, PDO_FIT_COUNT };

/* Whether the game set itself up in its widescreen mode (anamorphic 16:9:
 * a wider view squeezed into 640x480), which it reads once at start:
 * src/guest/widescreen.c. */
bool pdo_widescreen_active(void);

/* The current settings: defaults until pdo_config_load reads the file. */
pdo_settings *pdo_settings_get(void);
/* Write the settings, keeping the game folder the file already holds. */
bool pdo_settings_save(void);

/* Where settings, saves and imported game files live. With a portable.txt
 * next to the executable this is the executable's folder; otherwise the
 * per-user data folder. Always ends in a path separator. */
const char *pdo_data_dir(void);

bool pdo_config_load(pdo_config *cfg);
bool pdo_config_save(const pdo_config *cfg);

#endif
