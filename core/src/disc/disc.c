// Disc image access. Layout (YAGCD 13, "Disc"): boot.bin 0x000-0x43F, bi2.bin 0x440-0x243F, apploader at 0x2440,
// then the DOL, FST and files at the offsets in boot.bin (0x420 DOL, 0x424 FST, 0x428 FST size).
#include "disc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define GC_MAGIC 0xC2339F3Du
#define GC_DISC_SIZE 0x57058000u  // full single-layer GameCube disc (1,459,978,240 bytes)

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

bool disc_read(Disc* d, u64 offset, void* dst, u32 len) {
    u8* out = dst;
    if (offset >= d->size) { memset(out, 0, len); return true; }
    u32 have = offset + len > d->size ? (u32)(d->size - offset) : len;
    FILE* f = d->file;
    if (fseek(f, (long)offset, SEEK_SET) != 0 || fread(out, 1, have, f) != have) return false;
    if (have < len) memset(out + have, 0, len - have);
    return true;
}

bool disc_probe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    u8 h[0x20];
    bool ok = fread(h, 1, sizeof h, f) == sizeof h && be32(h + 0x1C) == GC_MAGIC;
    fclose(f);
    return ok;
}

bool disc_open(Disc* d, const char* path, char* err, size_t errLen) {
    memset(d, 0, sizeof *d);
    FILE* f = fopen(path, "rb");
    if (!f) { snprintf(err, errLen, "cannot open %s", path); return false; }
    d->file = f;
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    d->size = end > 0 ? (u64)end : 0;
    if (!disc_read(d, 0, d->header, sizeof d->header) || !disc_read(d, 0x440, d->bi2, sizeof d->bi2)) {
        snprintf(err, errLen, "read error");
        disc_close(d);
        return false;
    }
    if (be32(d->header + 0x1C) != GC_MAGIC) {
        snprintf(err, errLen, "not a GameCube disc image (magic %08x)", be32(d->header + 0x1C));
        disc_close(d);
        return false;
    }
    d->nkit = !memcmp(d->header + 0x200, "NKIT", 4);
    if (d->nkit) d->nkitSourceSize = be32(d->header + 0x210);
    d->dolOffset = be32(d->header + 0x420);
    d->fstOffset = be32(d->header + 0x424);
    d->fstSize = be32(d->header + 0x428);
    if (d->fstSize < 12 || d->fstSize > 16u * 1024 * 1024 || d->fstOffset + (u64)d->fstSize > d->size) {
        snprintf(err, errLen, "bad FST (offset %08x size %08x)", d->fstOffset, d->fstSize);
        disc_close(d);
        return false;
    }
    d->fst = malloc(d->fstSize);
    if (!d->fst || !disc_read(d, d->fstOffset, d->fst, d->fstSize)) {
        snprintf(err, errLen, "cannot read FST");
        disc_close(d);
        return false;
    }
    d->fstEntries = be32(d->fst + 8);
    if (d->fstEntries == 0 || (u64)d->fstEntries * 12 > d->fstSize) {
        snprintf(err, errLen, "bad FST entry count %u", d->fstEntries);
        disc_close(d);
        return false;
    }
    return true;
}

void disc_close(Disc* d) {
    if (d->file) fclose(d->file);
    free(d->fst);
    d->file = NULL;
    d->fst = NULL;
}

// FST entry i: byte 0 = 1 for a directory; bytes 1-3 = name offset in the string table; then file offset / parent
// index, and file length / index of the first entry after the directory.
static const char* entry_name(Disc* d, u32 i) {
    u32 strOff = d->fstEntries * 12 + (be32(d->fst + 12 * i) & 0xFFFFFF);
    return strOff < d->fstSize ? (const char*)d->fst + strOff : "";
}

static bool name_eq(const char* a, size_t alen, const char* b, u32 bmax) {
    u32 i = 0;
    for (; i < alen && i < bmax && b[i]; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return i == alen && (i >= bmax || !b[i]);
}

bool disc_find(Disc* d, const char* path, u32* offset, u32* size) {
    u32 dirStart = 1, dirEnd = d->fstEntries;
    const char* p = path;
    while (*p == '/') p++;
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        u32 found = 0;
        for (u32 i = dirStart; i < dirEnd; ) {
            const u8* e = d->fst + 12 * i;
            const char* nm = entry_name(d, i);
            u32 room = d->fstSize - (u32)(nm - (const char*)d->fst);
            if (name_eq(p, len, nm, room)) { found = i; break; }
            i = e[0] ? be32(e + 8) : i + 1;  // skip a whole subdirectory
            if (i <= dirStart - 1) break;    // malformed
        }
        if (!found) return false;
        const u8* e = d->fst + 12 * found;
        if (!slash) {
            if (e[0]) return false;
            *offset = be32(e + 4);
            *size = be32(e + 8);
            return true;
        }
        if (!e[0]) return false;
        dirStart = found + 1;
        dirEnd = be32(e + 8);
        if (dirEnd > d->fstEntries) dirEnd = d->fstEntries;
        p = slash;
        while (*p == '/') p++;
    }
    return false;
}

void disc_print_info(Disc* d) {
    char id[7], title[65];
    memcpy(id, d->header, 6); id[6] = 0;
    memcpy(title, d->header + 0x20, 64); title[64] = 0;
    fprintf(stderr, "disc %s rev %u \"%s\"  image %llu bytes\n", id, d->header[7], title, (unsigned long long)d->size);
    u8 al[0x20];
    if (!disc_read(d, 0x2440, al, sizeof al)) memset(al, 0, sizeof al);
    fprintf(stderr, "  DOL at %08x, FST at %08x size %08x (%u entries); apploader %.10s entry %08x size %08x+%08x\n",
            d->dolOffset, d->fstOffset, d->fstSize, d->fstEntries, (const char*)al, be32(al + 0x10), be32(al + 0x14),
            be32(al + 0x18));
    if (d->nkit)
        fprintf(stderr, "  NKit %.4s: source CRC %08x, source size %08x (%u bytes, %.1f%% kept)\n", d->header + 0x204,
                be32(d->header + 0x208), d->nkitSourceSize, d->nkitSourceSize,
                d->nkitSourceSize ? 100.0 * (double)d->size / d->nkitSourceSize : 0.0);
    u32 files = 0, outside = 0, maxEnd = 0;
    for (u32 i = 1; i < d->fstEntries; i++) {
        const u8* e = d->fst + 12 * i;
        if (e[0]) continue;
        u32 off = be32(e + 4), len = be32(e + 8);
        files++;
        if ((u64)off + len > d->size) {
            if (outside++ < 5) fprintf(stderr, "  file %s at %08x+%x lies outside the image\n", entry_name(d, i), off, len);
        }
        if (off + len > maxEnd) maxEnd = off + len;
    }
    fprintf(stderr, "  %u files, %u outside the image; last file byte at %08x (image ends %08llx; full disc %08x)\n",
            files, outside, maxEnd, (unsigned long long)d->size, GC_DISC_SIZE);
}
