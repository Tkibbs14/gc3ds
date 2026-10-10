#pragma once
// GameCube disc images: plain GCM/ISO and NKit (.nkit.iso). Reads by byte offset and FST (file system table) lookup.
//
// NKit GameCube images are GCMs whose junk padding was removed and whose files were packed closer together; the boot
// header (0x420 DOL, 0x424 FST) and the FST are rewritten to the new offsets, so everything the game reads through
// them is where they say. The image carries an "NKIT" block at 0x200 (unused by the console) recording the original
// size and CRC. Reads past the end of the (shorter) image return zeros: on a real disc that is junk nobody reads.
#include "../common.h"

typedef struct Disc {
    void* file;            // FILE*
    u64 size;              // bytes in the image file
    u8 header[0x440];      // boot.bin: disc ID, title, DOL/FST offsets
    u8 bi2[0x2000];        // bi2.bin at 0x440: debug monitor / simulated memory / region settings
    u32 dolOffset, fstOffset, fstSize;
    u8* fst;               // the whole FST (entries + string table)
    u32 fstEntries;
    bool nkit;
    u32 nkitSourceSize;    // original GCM size recorded by NKit (0 for plain images)
} Disc;

// Opens and validates an image (GameCube magic 0xC2339F3D at 0x1C). Returns false with a message on error.
bool disc_open(Disc* d, const char* path, char* err, size_t errLen);
void disc_close(Disc* d);
// Reads len bytes at a disc offset; bytes beyond the end of the image read as zero. False on an I/O error.
bool disc_read(Disc* d, u64 offset, void* dst, u32 len);
// Looks a path up in the FST ("/rel/foo.rel" or "rel/foo.rel", case-insensitive). False if not found.
bool disc_find(Disc* d, const char* path, u32* offset, u32* size);
// True if the file at path looks like a GameCube disc image (checks the magic, not the extension).
bool disc_probe(const char* path);
// Prints the header, NKit block and an FST consistency check (every file inside the image) to stderr.
void disc_print_info(Disc* d);
