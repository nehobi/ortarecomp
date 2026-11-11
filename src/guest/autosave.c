/*
 * Auto-save (the auto_save setting, off by default) and a save trace in the
 * log.
 *
 * The game keeps one save file, PDOSYSTEM.DAT: options, records, Pandora's
 * Box and the story save (docs/orta-facts.md, "Saves and Pandora's Box").
 * 0x00034340 (cdecl, one argument) copies the game's working state into
 * the block at 0x00424E48 and writes the file (tail jump to 0x0006A950,
 * skipped in kiosk mode, 0x0034277C): bit 0 of the argument takes the
 * options, records and Box, bit 1 the story save (the episode from
 * 0x00342758 to 0x00424EA0, the difficulty, the dragon). The title's
 * CONTINUE is there only with a story save (0x000340C0: 0x00424EA0 not 0)
 * and starts the saved episode.
 *
 * The game writes the story save in one place only: SAVE & END GAME on the
 * game-over menu, which comes to 0x000BE080 with 0x107 and calls
 * 0x00034340(2) (the only call with bit 1). Clearing an episode goes on to
 * the next one without it, so a player who never picks SAVE & END GAME
 * has no CONTINUE: the game's own behaviour, not a port bug.
 *
 * 0x000BE080 with 0x104 starts the stage of the episode at 0x00342758
 * (0x000BDD10); after a clear the game has moved that on to the next
 * episode by then. With auto_save on, when 0x104 comes with the episode
 * after the one just played, the hook below makes the call SAVE & END GAME
 * makes there, 0x00034340(2), before the next episode starts: the file is
 * the game's own, the same bytes SAVE & END GAME would write in that
 * episode, and CONTINUE starts that episode. Nothing else changes; with
 * the setting off the game runs as it always has.
 *
 * Scenes: the current scene is at 0x0034271C, the next at 0x00342718;
 * 0x000115B0 moves next to current every game frame. A scene's handler
 * gets the value at 0x00341E44 + scene * 16. Episode n's stage is scenes
 * 3n + 0xF (start), 3n + 0x10 (play), 3n + 0x11 (end); 0x5D handles how a
 * stage ended or what starts next (0x000BE080: 0x102 new game, 0x104 the
 * episode's stage, 0x105 game over, 0x106 retry, 0x107 save and end).
 *
 * Testing: PDO_SCENE_TRACE=1 logs every scene change; PDO_TEST_CLEAR=n,
 * once frame n is passed in a stage, moves the episode on by one and ends
 * the stage with 0x104 (what a clear leaves), to reach the next episode
 * without playing the boss.
 */

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "recomp_types.h"
#include "config.h"

#define SCENE_NEXT      0x00342718
#define SCENE_CUR       0x0034271C
#define SCENE_VALUE(s)  (0x00341E44 + (s) * 16)
#define EPISODE_NOW     0x00342758
#define KIOSK           0x0034277C
#define SAVED_EPISODE   0x00424EA0

#define STAGE_START(ep) ((ep) * 3 + 0xF)
#define START_EPISODE   0x104
#define LAST_EPISODE    10

uint64_t pdo_gfx_frame_count(void);
extern void sub_00034340(void);

static uint32_t played_episode;     /* the episode whose stage was last played, 0 none */

static bool trace_scenes(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = SDL_getenv("PDO_SCENE_TRACE");
        on = e && *e && *e != '0';
    }
    return on;
}

/* 0x00034340(2) from a hook, as SAVE & END GAME calls it; the guest's
 * registers are kept as they were. */
static void save_story(void)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx;
    uint32_t esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp;

    g_esp -= 4;
    MEM32(g_esp) = 2;           /* the story save only */
    g_esp -= 4;
    MEM32(g_esp) = 0;           /* return address; the callee pops it */
    sub_00034340();
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx;
    g_esi = esi; g_edi = edi; g_ebp = ebp; g_esp = esp;
}

static void test_clear(uint32_t cur, uint32_t next, uint32_t ep)
{
    static long long at = -2;
    if (at == -2) {
        const char *e = SDL_getenv("PDO_TEST_CLEAR");
        at = e && *e ? SDL_strtoll(e, NULL, 10) : -1;
    }
    if (at < 0 || pdo_gfx_frame_count() < (uint64_t)at || next != cur || cur != STAGE_START(ep) + 1)
        return;
    at = -1;
    MEM32(EPISODE_NOW) = ep + 1;
    MEM32(SCENE_VALUE(cur + 1)) = START_EPISODE;
    MEM32(SCENE_NEXT) = cur + 1;
    SDL_Log("test: episode %u ended as cleared", ep);
}

/* The game's frame: the next scene becomes the current one here. */
void sub_000115B0_enter(void)
{
    uint32_t next = MEM32(SCENE_NEXT), cur = MEM32(SCENE_CUR), ep;

    ep = MEM32(EPISODE_NOW);
    test_clear(cur, next, ep);
    if (next == cur)
        return;
    if (trace_scenes())
        SDL_Log("scene: 0x%X -> 0x%X (value 0x%X, episode %u, frame %llu)", cur, next,
                MEM32(SCENE_VALUE(next)), ep, (unsigned long long)pdo_gfx_frame_count());
    if (ep >= 1 && ep <= LAST_EPISODE && next == STAGE_START(ep) + 1)
        played_episode = ep;    /* its stage is being played */
    else if (next == 0x9 || next == 0xA || next == 0xB)
        played_episode = 0;     /* back to the title or the menu */
}

/* How a stage ended, or what starts next (cdecl: the scene's value at
 * esp+4) */
void sub_000BE080_enter(void)
{
    uint32_t how = MEM32(g_esp + 4), ep = MEM32(EPISODE_NOW);
    bool next_episode = how == START_EPISODE && played_episode && ep == played_episode + 1;

    if (!next_episode)
        return;
    played_episode = 0;
    SDL_Log("save: episode %u cleared, episode %u starts", ep - 1, ep);
    if (!pdo_settings_get()->auto_save || ep > LAST_EPISODE || MEM32(KIOSK))
        return;
    SDL_Log("auto-save: saving the story at episode %u, as SAVE & END GAME does", ep);
    save_story();
}

/* Copy the working state into the save block and write PDOSYSTEM.DAT
 * (cdecl: argument at esp+4) */
void sub_00034340_enter(void)
{
    uint32_t what = MEM32(g_esp + 4);

    SDL_Log("save: PDOSYSTEM.DAT %s(%s%s), episode %u, story save was episode %u",
            MEM32(KIOSK) ? "not written, kiosk " : "written ",
            what & 1 ? "options+records" : "", what & 2 ? (what & 1 ? "+story" : "story") : "",
            MEM32(EPISODE_NOW), MEM32(SAVED_EPISODE));
}

/* Copy the save block into the working state (cdecl: argument at esp+4;
 * bit 1: the story save too) */
void sub_00034120_enter(void)
{
    SDL_Log("save: PDOSYSTEM.DAT loaded (%s), story save: episode %u", MEM32(g_esp + 4) & 2 ? "story" : "options+records",
            MEM32(SAVED_EPISODE));
}
