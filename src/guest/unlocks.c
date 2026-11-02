/*
 * Pandora's Box, all open (the launcher's "Pandora's Box" setting).
 *
 * Every Box item, menu entries included, is checked by one function
 * (0x000309B0, cdecl, the item's number): open when its entry in the table
 * at 0x003CE0A8 says always, when its bit is set in the unlocked items the
 * save keeps (0x00345D90), or when the total play time (0x003427F8, in
 * frames) reaches the hours the item needs. With the setting on it answers
 * open for every item. Nothing is written: the save, the play time and the
 * game's count of what is still to come (0x00030A10, behind "new pandra's
 * box added!") stay as they are, and turning the setting off puts the Box
 * back as earned. Items not looked at yet show NEW (0x00030B60) until seen.
 */

#include "recomp_types.h"
#include "config.h"

/* Pandora's Box: whether item n is open (cdecl, n; ret). */
extern void sub_000309B0_gen(void);
void sub_000309B0(void)
{
    sub_000309B0_gen();
    if (pdo_settings_get()->unlock_box)
        g_eax = 1;
}
