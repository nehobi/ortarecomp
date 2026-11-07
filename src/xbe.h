#ifndef PDO_XBE_H
#define PDO_XBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PDO_XBE_MAX_SECTIONS  64
#define PDO_XBE_MAX_LIBRARIES 64

typedef struct pdo_xbe_section {
    char     name[16];
    uint32_t vaddr, vsize;
    uint32_t raw_addr, raw_size;
} pdo_xbe_section;

typedef struct pdo_xbe_library {
    char     name[9];
    uint16_t build;             /* XDK build, e.g. 4134 */
} pdo_xbe_library;

typedef struct pdo_xbe_info {
    uint32_t base;
    uint32_t entry;             /* decoded (un-XORed) entry point VA */
    uint32_t kernel_thunk;      /* decoded kernel thunk table VA */
    uint32_t title_id;
    char     title[41];         /* ASCII rendering of the certificate title */
    uint32_t num_sections;
    pdo_xbe_section sections[PDO_XBE_MAX_SECTIONS];
    uint32_t num_libraries;
    pdo_xbe_library libraries[PDO_XBE_MAX_LIBRARIES];
    uint16_t xdk_build;         /* highest library build: the XDK version */
    uint8_t  file_sha256[32];   /* whole file */
    uint8_t  image_sha256[32];  /* section contents only; ignores the certificate */
} pdo_xbe_info;

typedef enum pdo_xbe_verdict {
    PDO_XBE_KNOWN,          /* a dump we have seen before */
    PDO_XBE_LAYOUT_MATCH,   /* Orta (NTSC-U) section layout, not a known dump */
    PDO_XBE_WRONG_GAME,     /* a valid XBE, but not the game we recompile */
    PDO_XBE_INVALID,        /* not an XBE */
} pdo_xbe_verdict;

/* Parse an in-memory XBE. On failure writes a reason to err and returns false. */
bool pdo_xbe_parse(const uint8_t *data, size_t size, pdo_xbe_info *out,
                   char *err, size_t err_len);

pdo_xbe_verdict pdo_xbe_identify(const pdo_xbe_info *info);

/* Human-readable explanation of a verdict, for dialogs and logs. */
const char *pdo_xbe_verdict_text(pdo_xbe_verdict v);

#endif
