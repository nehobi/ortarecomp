/*
 * Orta Recomp - entry point.
 *
 *   OrtaRecomp                  normal start (setup on first run)
 *   OrtaRecomp --setup          choose the game files again
 *   OrtaRecomp --game-dir DIR   use DIR for this run (not saved)
 *   OrtaRecomp --import-iso F   import a disc image without the dialogs
 *   OrtaRecomp --check          check the game files, print, exit
 *   OrtaRecomp --boot           also run the title (in development)
 */

#include "assets.h"
#include "config.h"
#include "setup_ui.h"
#include "sha256.h"

#ifdef PDO_HAVE_RECOMP
#include "guest/guest.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <string.h>

#define APP_NAME "Orta Recomp"

typedef struct options {
    bool        setup, check, boot;
    const char *game_dir;
    const char *import_iso;
} options;

static bool parse_args(int argc, char **argv, options *o)
{
    int i;

    SDL_zerop(o);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--setup") == 0)
            o->setup = true;
        else if (strcmp(argv[i], "--check") == 0)
            o->check = true;
        else if (strcmp(argv[i], "--boot") == 0)
            o->boot = true;
        else if (strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc)
            o->game_dir = argv[++i];
        else if (strcmp(argv[i], "--import-iso") == 0 && i + 1 < argc)
            o->import_iso = argv[++i];
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

static void report(const char *game_dir, const pdo_game_check *check)
{
    char file_hex[65], image_hex[65];

    pdo_sha256_hex(check->xbe.file_sha256, file_hex);
    pdo_sha256_hex(check->xbe.image_sha256, image_hex);
    printf("game folder : %s\n", game_dir);
    printf("title       : %s (%08X)\n", check->xbe.title, check->xbe.title_id);
    printf("verdict     : %s\n", pdo_xbe_verdict_text(check->verdict));
    printf("xbe sha256  : %s\n", file_hex);
    printf("image sha256: %s\n", image_hex);
    printf("entry point : 0x%08X\n", check->xbe.entry);
    printf("XDK build   : %u\n", check->xbe.xdk_build);
}

static void cli_progress(void *userdata, double fraction, const char *file)
{
    static int last = -1;
    int pct = (int)(fraction * 100);

    if (pct != last) {
        last = pct;
        fprintf(stderr, "\rimporting %3d%%", pct);
        if (pct == 100)
            fputc('\n', stderr);
    }
}

#ifdef PDO_HAVE_RECOMP
static int start_guest(const char *game_dir, const pdo_game_check *check, bool boot)
{
    char path[PDO_PATH_MAX], save_dir[PDO_PATH_MAX], image_hex[65];
    void *xbe;
    size_t size;
    int rc = 1;

    pdo_sha256_hex(check->xbe.image_sha256, image_hex);
    if (strcmp(image_hex, pdo_guest_built_from()) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME,
            "This build was recompiled from a different default.xbe than the "
            "one in your game files. Rebuild from your own copy.", NULL);
        return 1;
    }

    snprintf(path, sizeof path, "%s/default.xbe", game_dir);
    snprintf(save_dir, sizeof save_dir, "%ssaves", pdo_data_dir());
    SDL_CreateDirectory(save_dir);

    xbe = SDL_LoadFile(path, &size);
    if (!xbe) {
        SDL_Log("cannot read %s: %s", path, SDL_GetError());
        return 1;
    }
    if (pdo_guest_init(xbe, size, game_dir, save_dir)) {
        if (boot) {
            char list[PDO_PATH_MAX];
            snprintf(list, sizeof list, "%sopened_files.txt", pdo_data_dir());
            pdo_guest_log_opened_files(list);
            SDL_Log("the game files the title opens are listed in %s", list);
            pdo_guest_run(check->xbe.entry);
        } else {
            SDL_Log("game code is linked and the Xbox address space is set up; "
                    "run with --boot to start the title (not playable yet)");
        }
        rc = 0;
    }
    pdo_guest_shutdown();
    SDL_free(xbe);
    return rc;
}
#endif

int main(int argc, char **argv)
{
    options opt;
    pdo_config cfg;
    pdo_game_check check;
    char err[1024];
    bool have_game = false;

    if (!parse_args(argc, argv, &opt))
        return 2;
    SDL_SetAppMetadata(APP_NAME, "0.1.0", "io.github.ortarecomp");

    if (!SDL_Init(opt.check || opt.import_iso ? 0 : SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    pdo_config_load(&cfg);

    if (opt.import_iso) {
        char dst[PDO_PATH_MAX];
        pdo_assets_default_import_dir(dst, sizeof dst);
        if (!pdo_assets_import_iso(opt.import_iso, dst, cli_progress, NULL, &check, err, sizeof err)) {
            fprintf(stderr, "import failed: %s\n", err);
            SDL_Quit();
            return 1;
        }
        SDL_strlcpy(cfg.game_dir, dst, sizeof cfg.game_dir);
        pdo_config_save(&cfg);
        report(cfg.game_dir, &check);
        SDL_Quit();
        return 0;
    }

    if (opt.game_dir)
        SDL_strlcpy(cfg.game_dir, opt.game_dir, sizeof cfg.game_dir);

    if (!opt.setup && cfg.game_dir[0]) {
        have_game = pdo_assets_check_dir(cfg.game_dir, &check, err, sizeof err);
        if (!have_game)
            SDL_Log("game files at %s are not usable: %s", cfg.game_dir, err);
    }

    if (opt.check) {
        if (have_game)
            report(cfg.game_dir, &check);
        else
            fprintf(stderr, "no usable game files%s%s\n", cfg.game_dir[0] ? ": " : "",
                    cfg.game_dir[0] ? err : "");
        SDL_Quit();
        return have_game ? 0 : 1;
    }

    if (!have_game && !pdo_setup_run(&cfg, &check)) {
        SDL_Quit();
        return 0;
    }

#ifdef PDO_HAVE_RECOMP
    {
        int rc;
        rc = start_guest(cfg.game_dir, &check, opt.boot);
        SDL_Quit();
        return rc;
    }
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, APP_NAME,
        "Your game files are set up.\n\nThis build has no recompiled game code "
        "in it yet. See BUILDING.md to generate it from your default.xbe.", NULL);
    SDL_Quit();
    return 0;
#endif
}
