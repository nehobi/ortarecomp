/* Settings file: ortarecomp.ini, simple key=value lines. */

#include "config.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define CONFIG_NAME "ortarecomp.ini"

static char data_dir[PDO_PATH_MAX];

const char *pdo_data_dir(void)
{
    const char *base;
    char *pref;

    if (data_dir[0])
        return data_dir;

    base = SDL_GetBasePath();
    if (base) {
        char probe[PDO_PATH_MAX];
        snprintf(probe, sizeof probe, "%sportable.txt", base);
        if (SDL_GetPathInfo(probe, NULL)) {
            SDL_strlcpy(data_dir, base, sizeof data_dir);
            return data_dir;
        }
    }

    pref = SDL_GetPrefPath("OrtaRecomp", "OrtaRecomp");
    if (pref) {
        SDL_strlcpy(data_dir, pref, sizeof data_dir);
        SDL_free(pref);
    } else if (base) {
        SDL_strlcpy(data_dir, base, sizeof data_dir);
    }
    return data_dir;
}

static void config_path(char *out, size_t len)
{
    snprintf(out, len, "%s%s", pdo_data_dir(), CONFIG_NAME);
}

bool pdo_config_load(pdo_config *cfg)
{
    char path[PDO_PATH_MAX];
    char *text, *line, *save = NULL;
    size_t size;

    memset(cfg, 0, sizeof *cfg);
    config_path(path, sizeof path);
    text = SDL_LoadFile(path, &size);
    if (!text)
        return false;

    for (line = SDL_strtok_r(text, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';')
            continue;
        *eq = '\0';
        if (strcmp(line, "game_dir") == 0)
            SDL_strlcpy(cfg->game_dir, eq + 1, sizeof cfg->game_dir);
    }
    SDL_free(text);
    return true;
}

bool pdo_config_save(const pdo_config *cfg)
{
    char path[PDO_PATH_MAX], text[PDO_PATH_MAX + 128];
    int n;

    config_path(path, sizeof path);
    n = snprintf(text, sizeof text,
                 "# Orta Recomp settings\n"
                 "game_dir=%s\n", cfg->game_dir);
    if (n < 0 || (size_t)n >= sizeof text)
        return false;
    return SDL_SaveFile(path, text, (size_t)n);
}
