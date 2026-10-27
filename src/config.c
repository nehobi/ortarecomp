/* Settings file: ortarecomp.ini, simple key=value lines. */

#include "config.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define CONFIG_NAME "ortarecomp.ini"

static char data_dir[PDO_PATH_MAX];

static pdo_settings settings = { 1, 1, PDO_ASPECT_4_3, false, 100, false, true, true };

pdo_settings *pdo_settings_get(void)
{
    return &settings;
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

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
        else if (strcmp(line, "render_scale") == 0)
            settings.render_scale = clamp_int(SDL_atoi(eq + 1), 1, 4);
        else if (strcmp(line, "msaa") == 0) {
            int m = SDL_atoi(eq + 1);
            settings.msaa = m >= 8 ? 8 : m >= 4 ? 4 : m >= 2 ? 2 : 1;
        } else if (strcmp(line, "aspect") == 0)
            settings.aspect = strcmp(eq + 1, "stretch") == 0 ? PDO_ASPECT_STRETCH : PDO_ASPECT_4_3;
        else if (strcmp(line, "soft_filter") == 0)
            settings.soft_filter = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "master_volume") == 0)
            settings.master_volume = clamp_int(SDL_atoi(eq + 1), 0, 100);
        else if (strcmp(line, "show_fps") == 0)
            settings.show_fps = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "gpu_vertex") == 0)
            settings.gpu_vertex = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "gpu_fixed") == 0)
            settings.gpu_fixed = SDL_atoi(eq + 1) != 0;
    }
    SDL_free(text);
    return true;
}

bool pdo_config_save(const pdo_config *cfg)
{
    char path[PDO_PATH_MAX], text[PDO_PATH_MAX + 512];
    int n;

    config_path(path, sizeof path);
    n = snprintf(text, sizeof text,
                 "# Orta Recomp settings\n"
                 "game_dir=%s\n"
                 "render_scale=%d\n"
                 "msaa=%d\n"
                 "aspect=%s\n"
                 "soft_filter=%d\n"
                 "master_volume=%d\n"
                 "show_fps=%d\n"
                 "gpu_vertex=%d\n"
                 "gpu_fixed=%d\n",
                 cfg->game_dir, settings.render_scale, settings.msaa,
                 settings.aspect == PDO_ASPECT_STRETCH ? "stretch" : "4:3", settings.soft_filter,
                 settings.master_volume, settings.show_fps, settings.gpu_vertex,
                 settings.gpu_fixed);
    if (n < 0 || (size_t)n >= sizeof text)
        return false;
    return SDL_SaveFile(path, text, (size_t)n);
}

bool pdo_settings_save(void)
{
    /* The file's game folder, not this run's: --game-dir is not saved. The
     * load would also read the file's settings over ours, so keep them. */
    pdo_settings keep = settings;
    pdo_config cfg;
    bool ok;

    pdo_config_load(&cfg);
    settings = keep;
    ok = pdo_config_save(&cfg);
    if (!ok)
        SDL_Log("settings: could not save (%s)", SDL_GetError());
    return ok;
}
