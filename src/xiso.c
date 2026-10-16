/*
 * XDVDFS reader.
 *
 * The volume descriptor sits 32 sectors into the game partition and starts
 * and ends with "MICROSOFT*XBOX*MEDIA". Directories are binary trees of
 * 4-byte-aligned entries:
 *
 *   u16 left, u16 right   child offsets in dwords from the directory start
 *   u32 sector, u32 size  file data, or the child directory's table
 *   u8  attributes        0x10 = directory
 *   u8  name length, then the name (no terminator)
 *
 * A left offset of 0xFFFF marks padding to the end of a sector. The image is
 * untrusted input: every offset is bounds-checked and names are filtered.
 */

#include "xiso.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define SECTOR        2048u
#define MAGIC         "MICROSOFT*XBOX*MEDIA"
#define MAGIC_LEN     20
#define MAX_DEPTH     32
#define MAX_ENTRIES   200000
#define MAX_DIR_SIZE  (16u * 1024u * 1024u)

/* Partition offsets: trimmed xiso, then XGD1/XGD2/XGD3 full dumps. */
static const uint64_t partition_offsets[] = {
    0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull,
};

struct pdo_xiso {
    SDL_IOStream *io;
    uint64_t      partition;
    uint32_t      root_sector, root_size;
    int           entries_seen;
};

static bool read_at(pdo_xiso *iso, uint64_t offset, void *buf, size_t len)
{
    if (SDL_SeekIO(iso->io, (Sint64)offset, SDL_IO_SEEK_SET) < 0)
        return false;
    return SDL_ReadIO(iso->io, buf, len) == len;
}

pdo_xiso *pdo_xiso_open(const char *path, char *err, size_t err_len)
{
    pdo_xiso *iso = SDL_calloc(1, sizeof *iso);
    size_t i;

    if (!iso) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    iso->io = SDL_IOFromFile(path, "rb");
    if (!iso->io) {
        snprintf(err, err_len, "cannot open %s: %s", path, SDL_GetError());
        SDL_free(iso);
        return NULL;
    }

    for (i = 0; i < SDL_arraysize(partition_offsets); i++) {
        uint8_t vd[0x800];
        uint64_t at = partition_offsets[i] + 32ull * SECTOR;

        if (!read_at(iso, at, vd, sizeof vd))
            continue;
        if (memcmp(vd, MAGIC, MAGIC_LEN) != 0 || memcmp(vd + 0x7EC, MAGIC, MAGIC_LEN) != 0)
            continue;
        iso->partition   = partition_offsets[i];
        iso->root_sector = (uint32_t)vd[20] | vd[21] << 8 | vd[22] << 16 | (uint32_t)vd[23] << 24;
        iso->root_size   = (uint32_t)vd[24] | vd[25] << 8 | vd[26] << 16 | (uint32_t)vd[27] << 24;
        return iso;
    }

    snprintf(err, err_len, "not an Xbox disc image (no XDVDFS volume found)");
    SDL_CloseIO(iso->io);
    SDL_free(iso);
    return NULL;
}

void pdo_xiso_close(pdo_xiso *iso)
{
    if (!iso)
        return;
    SDL_CloseIO(iso->io);
    SDL_free(iso);
}

static bool name_is_safe(const char *name)
{
    const char *p;

    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 0x20)
            return false;
    return true;
}

typedef struct walk_ctx {
    pdo_xiso         *iso;
    pdo_xiso_visit_fn visit;
    void             *userdata;
    char             *err;
    size_t            err_len;
    bool              stopped;
} walk_ctx;

static bool walk_dir(walk_ctx *w, uint32_t sector, uint32_t size, const char *prefix, int depth);

/* Visit every entry of one directory table. Sibling order does not matter to
 * callers, only that a directory is visited before its contents, so the tree
 * is walked with an explicit stack: no recursion depth to blow up, and a
 * visited bitmap rejects malformed images whose links loop. */
static bool walk_table(walk_ctx *w, const uint8_t *table, uint32_t size,
                       const char *prefix, int depth)
{
    uint32_t  slots = size / 4 + 1;
    uint32_t *stack = SDL_malloc(slots * sizeof *stack);
    uint8_t  *seen = SDL_calloc(slots / 8 + 1, 1);
    uint32_t  top = 0;
    char      name[256], path[1024];
    bool      ok = false;

    if (!stack || !seen) {
        snprintf(w->err, w->err_len, "out of memory");
        goto out;
    }
    stack[top++] = 0;

    while (top) {
        uint32_t at = stack[--top], sec, len, slot = at / 4;
        uint16_t left, right;
        uint8_t  attr, name_len;

        if (at + 14 > size)
            continue;
        left = (uint16_t)(table[at] | table[at + 1] << 8);
        if (left == 0xFFFF)
            continue;   /* sector padding, no entry here */
        if (seen[slot / 8] & (1u << (slot % 8))) {
            snprintf(w->err, w->err_len, "disc image directory is malformed");
            goto out;
        }
        seen[slot / 8] |= (uint8_t)(1u << (slot % 8));
        if (++w->iso->entries_seen > MAX_ENTRIES) {
            snprintf(w->err, w->err_len, "disc image has too many files");
            goto out;
        }

        right = (uint16_t)(table[at + 2] | table[at + 3] << 8);
        sec  = (uint32_t)table[at + 4] | table[at + 5] << 8 | table[at + 6] << 16 | (uint32_t)table[at + 7] << 24;
        len  = (uint32_t)table[at + 8] | table[at + 9] << 8 | table[at + 10] << 16 | (uint32_t)table[at + 11] << 24;
        attr = table[at + 12];
        name_len = table[at + 13];
        if (at + 14 + name_len > size) {
            snprintf(w->err, w->err_len, "disc image directory is truncated");
            goto out;
        }
        memcpy(name, table + at + 14, name_len);
        name[name_len] = '\0';
        if (!name_is_safe(name)) {
            snprintf(w->err, w->err_len, "disc image contains an unsafe file name");
            goto out;
        }

        /* Each entry is pushed at most once (seen bitmap), so two children
         * per popped entry can never exceed the slot count. */
        if (left && top < slots)
            stack[top++] = (uint32_t)left * 4;
        if (right && top < slots)
            stack[top++] = (uint32_t)right * 4;

        snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", name);
        if (!w->visit(w->userdata, path, (attr & 0x10) != 0, sec, len)) {
            w->stopped = true;
            goto out;
        }
        if ((attr & 0x10) && len && !walk_dir(w, sec, len, path, depth + 1))
            goto out;
    }
    ok = true;

out:
    SDL_free(stack);
    SDL_free(seen);
    return ok;
}

static bool walk_dir(walk_ctx *w, uint32_t sector, uint32_t size, const char *prefix, int depth)
{
    uint8_t *table;
    bool ok;

    if (depth > MAX_DEPTH || size > MAX_DIR_SIZE) {
        snprintf(w->err, w->err_len, "disc image directory is malformed");
        return false;
    }
    table = SDL_malloc(size);
    if (!table) {
        snprintf(w->err, w->err_len, "out of memory");
        return false;
    }
    if (!read_at(w->iso, w->iso->partition + (uint64_t)sector * SECTOR, table, size)) {
        snprintf(w->err, w->err_len, "cannot read disc image: %s", SDL_GetError());
        SDL_free(table);
        return false;
    }
    ok = walk_table(w, table, size, prefix, depth);
    SDL_free(table);
    return ok;
}

bool pdo_xiso_walk(pdo_xiso *iso, pdo_xiso_visit_fn visit, void *userdata,
                   char *err, size_t err_len)
{
    walk_ctx w = { iso, visit, userdata, err, err_len, false };

    iso->entries_seen = 0;
    if (!iso->root_size)
        return true;
    return walk_dir(&w, iso->root_sector, iso->root_size, "", 0) || w.stopped;
}

bool pdo_xiso_extract(pdo_xiso *iso, uint32_t sector, uint32_t size, const char *dst_path,
                      void (*progress)(void *userdata, uint64_t bytes), void *userdata,
                      char *err, size_t err_len)
{
    static uint8_t buf[1024 * 1024];
    SDL_IOStream *out;
    uint32_t left = size;

    if (SDL_SeekIO(iso->io, (Sint64)(iso->partition + (uint64_t)sector * SECTOR), SDL_IO_SEEK_SET) < 0) {
        snprintf(err, err_len, "cannot seek in disc image: %s", SDL_GetError());
        return false;
    }
    out = SDL_IOFromFile(dst_path, "wb");
    if (!out) {
        snprintf(err, err_len, "cannot create %s: %s", dst_path, SDL_GetError());
        return false;
    }
    while (left) {
        size_t n = left < sizeof buf ? left : sizeof buf;
        if (SDL_ReadIO(iso->io, buf, n) != n) {
            snprintf(err, err_len, "disc image ended early: %s", SDL_GetError());
            SDL_CloseIO(out);
            return false;
        }
        if (SDL_WriteIO(out, buf, n) != n) {
            snprintf(err, err_len, "cannot write %s: %s", dst_path, SDL_GetError());
            SDL_CloseIO(out);
            return false;
        }
        left -= (uint32_t)n;
        if (progress)
            progress(userdata, n);
    }
    if (!SDL_CloseIO(out)) {
        snprintf(err, err_len, "cannot finish writing %s: %s", dst_path, SDL_GetError());
        return false;
    }
    return true;
}
