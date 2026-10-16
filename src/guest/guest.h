#ifndef PDO_GUEST_H
#define PDO_GUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Map the Xbox address space from the XBE, set up the kernel layer and the
 * function dispatch table. Does not run any game code. */
bool pdo_guest_init(const void *xbe_data, size_t xbe_size,
                    const char *game_dir, const char *save_dir);

/* Run the title from its entry point. Returns when the title exits. */
void pdo_guest_run(uint32_t entry);

void pdo_guest_shutdown(void);

/* Write every game file the title opens, once each, to list_path. */
void pdo_guest_log_opened_files(const char *list_path);

/* Image hash of the XBE the code was generated from (from pdo_build_info.h),
 * as lower-case hex. */
const char *pdo_guest_built_from(void);

#endif
