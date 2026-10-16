#ifndef PDO_SETUP_UI_H
#define PDO_SETUP_UI_H

#include "assets.h"
#include "config.h"

#include <stdbool.h>

/* First-run setup: ask the player for their disc image or extracted game
 * folder, check it, import it if needed and save the choice to cfg.
 * Returns false if the player quits. */
bool pdo_setup_run(pdo_config *cfg, pdo_game_check *check);

#endif
