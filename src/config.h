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
    int  aspect;            /* PDO_ASPECT_* */
    bool soft_filter;       /* TV-like vertical softening at presentation */
    int  master_volume;     /* 0-100 % */
    bool show_fps;
    bool gpu_vertex;        /* vertex programs on the GPU (shaders/vsh.vert), not the CPU */
} pdo_settings;

enum { PDO_ASPECT_4_3, PDO_ASPECT_STRETCH, PDO_ASPECT_COUNT };

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
