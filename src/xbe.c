/*
 * XBE header parsing and game identification.
 *
 * Retail dumps of one release can differ in a few certificate bytes (allowed
 * media), so besides the whole-file hash we also hash the section contents,
 * which is what the recompiled code actually depends on. tools/recomp.py
 * carries the same tables; keep them in step.
 *
 * Panzer Dragoon Orta (USA) is SE-007, built with XDK 4928.
 */

#include "xbe.h"

#include "sha256.h"

#include <stdio.h>
#include <string.h>

#define XBE_RETAIL_ENTRY_KEY  0xA8FC57ABu
#define XBE_DEBUG_ENTRY_KEY   0x94859D4Bu
#define XBE_RETAIL_THUNK_KEY  0x5B6D40B6u
#define XBE_DEBUG_THUNK_KEY   0xEFB1F152u

#define PDO_TITLE_ID 0x53450007u

/* Known dumps, NULL-terminated. */
static const char *const known_file_sha256[] = {
    "c1af4eafccc02e11105d606b177263216cb7aa71bf1fbe8486ddfc3b1b848d2c",
    NULL,
};

/* Sections whose placement the recompiled code depends on, NULL-terminated. */
static const struct { const char *name; uint32_t vaddr; } pdo_layout[] = {
    { ".text",   0x00011000 },
    { "D3D",     0x002AD9A0 },
    { "DSOUND",  0x002C1140 },
    { "XGRPH",   0x002FD740 },
    { "XPP",     0x003067A0 },
    { ".rdata",  0x0030C8A0 },
    { "APPDATA", 0x00340900 },
    { NULL, 0 },
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Bounds-checked translation of a header VA into a file offset. */
static bool va_to_off(uint32_t va, uint32_t base, size_t file_size, size_t need, size_t *off)
{
    if (va < base)
        return false;
    *off = (size_t)(va - base);
    return *off <= file_size && need <= file_size - *off;
}

static bool in_any_section(const pdo_xbe_info *x, uint32_t va)
{
    uint32_t i;
    for (i = 0; i < x->num_sections; i++)
        if (va >= x->sections[i].vaddr && va - x->sections[i].vaddr < x->sections[i].vsize)
            return true;
    return false;
}

bool pdo_xbe_parse(const uint8_t *data, size_t size, pdo_xbe_info *out,
                   char *err, size_t err_len)
{
    uint32_t cert_va, sect_va, n, i, raw_entry, raw_thunk, lib_count, lib_va;
    size_t off;
    pdo_sha256 h;

    memset(out, 0, sizeof *out);
    if (size < 0x178 || memcmp(data, "XBEH", 4) != 0) {
        snprintf(err, err_len, "not an Xbox executable (no XBEH header)");
        return false;
    }

    out->base = rd32(data + 0x104);
    cert_va   = rd32(data + 0x118);
    n         = rd32(data + 0x11C);
    sect_va   = rd32(data + 0x120);
    raw_entry = rd32(data + 0x128);
    raw_thunk = rd32(data + 0x158);
    lib_count = rd32(data + 0x160);
    lib_va    = rd32(data + 0x164);

    if (n == 0 || n > PDO_XBE_MAX_SECTIONS) {
        snprintf(err, err_len, "unexpected section count %u", n);
        return false;
    }
    out->num_sections = n;

    if (!va_to_off(cert_va, out->base, size, 0x0C + 80, &off)) {
        snprintf(err, err_len, "certificate outside the file");
        return false;
    }
    out->title_id = rd32(data + off + 0x08);
    for (i = 0; i < 40; i++) {
        uint16_t ch = (uint16_t)(data[off + 0x0C + i * 2] | data[off + 0x0D + i * 2] << 8);
        if (ch == 0)
            break;
        out->title[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }

    if (!va_to_off(sect_va, out->base, size, (size_t)n * 0x38, &off)) {
        snprintf(err, err_len, "section table outside the file");
        return false;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *s = data + off + (size_t)i * 0x38;
        pdo_xbe_section *sec = &out->sections[i];
        size_t name_off;

        sec->vaddr    = rd32(s + 0x04);
        sec->vsize    = rd32(s + 0x08);
        sec->raw_addr = rd32(s + 0x0C);
        sec->raw_size = rd32(s + 0x10);
        if (sec->raw_addr > size || sec->raw_size > size - sec->raw_addr) {
            snprintf(err, err_len, "section %u data outside the file", i);
            return false;
        }
        if (va_to_off(rd32(s + 0x14), out->base, size, 1, &name_off)) {
            size_t k;
            for (k = 0; k + 1 < sizeof sec->name && name_off + k < size && data[name_off + k]; k++)
                sec->name[k] = (char)data[name_off + k];
        }
    }

    /* XDK libraries linked in: 8-byte name, then major, minor, build, flags. */
    if (lib_count <= PDO_XBE_MAX_LIBRARIES &&
        va_to_off(lib_va, out->base, size, (size_t)lib_count * 16, &off)) {
        out->num_libraries = lib_count;
        for (i = 0; i < lib_count; i++) {
            const uint8_t *l = data + off + (size_t)i * 16;
            memcpy(out->libraries[i].name, l, 8);
            out->libraries[i].build = (uint16_t)(l[12] | l[13] << 8);
            if (out->libraries[i].build > out->xdk_build)
                out->xdk_build = out->libraries[i].build;
        }
    }

    /* Retail and debug builds scramble these with different keys; the right
     * one is whichever lands inside the image. */
    out->entry = raw_entry ^ XBE_RETAIL_ENTRY_KEY;
    out->kernel_thunk = raw_thunk ^ XBE_RETAIL_THUNK_KEY;
    if (!in_any_section(out, out->entry)) {
        out->entry = raw_entry ^ XBE_DEBUG_ENTRY_KEY;
        out->kernel_thunk = raw_thunk ^ XBE_DEBUG_THUNK_KEY;
    }

    pdo_sha256_init(&h);
    pdo_sha256_update(&h, data, size);
    pdo_sha256_final(&h, out->file_sha256);

    pdo_sha256_init(&h);
    for (i = 0; i < n; i++)
        pdo_sha256_update(&h, data + out->sections[i].raw_addr, out->sections[i].raw_size);
    pdo_sha256_final(&h, out->image_sha256);
    return true;
}

pdo_xbe_verdict pdo_xbe_identify(const pdo_xbe_info *info)
{
    char hex[65];
    size_t i;
    uint32_t j;

    if (info->title_id != PDO_TITLE_ID)
        return PDO_XBE_WRONG_GAME;

    pdo_sha256_hex(info->file_sha256, hex);
    for (i = 0; known_file_sha256[i]; i++)
        if (strcmp(hex, known_file_sha256[i]) == 0)
            return PDO_XBE_KNOWN;

    for (i = 0; pdo_layout[i].name; i++) {
        bool found = false;
        for (j = 0; j < info->num_sections; j++)
            if (strcmp(info->sections[j].name, pdo_layout[i].name) == 0 &&
                info->sections[j].vaddr == pdo_layout[i].vaddr)
                found = true;
        if (!found)
            return PDO_XBE_WRONG_GAME;
    }
    return PDO_XBE_LAYOUT_MATCH;
}

const char *pdo_xbe_verdict_text(pdo_xbe_verdict v)
{
    switch (v) {
    case PDO_XBE_KNOWN:
        return "Panzer Dragoon Orta (USA), known dump";
    case PDO_XBE_LAYOUT_MATCH:
        return "Panzer Dragoon Orta (USA), layout matches but this exact dump "
               "has not been seen before";
    case PDO_XBE_WRONG_GAME:
        return "a different game or region; only the USA release is supported";
    default:
        return "not an Xbox executable";
    }
}
