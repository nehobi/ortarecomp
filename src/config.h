#ifndef PDO_CONFIG_H
#define PDO_CONFIG_H

#include <stdbool.h>

#define PDO_PATH_MAX 1024

typedef struct pdo_config {
    char game_dir[PDO_PATH_MAX];    /* folder holding default.xbe and media/ */
} pdo_config;

/* Where settings, saves and imported game files live. With a portable.txt
 * next to the executable this is the executable's folder; otherwise the
 * per-user data folder. Always ends in a path separator. */
const char *pdo_data_dir(void);

bool pdo_config_load(pdo_config *cfg);
bool pdo_config_save(const pdo_config *cfg);

#endif
