#ifndef PDO_XISO_H
#define PDO_XISO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reader for Xbox disc images (XDVDFS). Accepts both trimmed "xiso" images
 * and full Redump-style dumps, where the game partition sits further in. */

typedef struct pdo_xiso pdo_xiso;

pdo_xiso *pdo_xiso_open(const char *path, char *err, size_t err_len);
void pdo_xiso_close(pdo_xiso *iso);

/* Called once per entry, parents before children. relpath uses '/' and is
 * already checked to be safe (no "..", no separators inside a name).
 * Return false to stop the walk. */
typedef bool (*pdo_xiso_visit_fn)(void *userdata, const char *relpath, bool is_dir,
                                  uint32_t sector, uint32_t size);

bool pdo_xiso_walk(pdo_xiso *iso, pdo_xiso_visit_fn visit, void *userdata,
                   char *err, size_t err_len);

/* Copy one file's contents out of the image. progress (may be NULL) is
 * called with the number of bytes just written. */
bool pdo_xiso_extract(pdo_xiso *iso, uint32_t sector, uint32_t size, const char *dst_path,
                      void (*progress)(void *userdata, uint64_t bytes), void *userdata,
                      char *err, size_t err_len);

#endif
