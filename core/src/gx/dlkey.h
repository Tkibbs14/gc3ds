#pragma once
// G2's first measurement (EMULATOR_CEILING.md G2, MEASUREMENTS.md "G2-0"): per display list called from the FIFO, the
// share whose key repeats, by how much incoming state the key includes. Measurement only: --dlkey on (3DS), DLKEY=1
// (PC); off, fifo.c pays one flag test per list command.
#include "gx.h"

// Read-before-write words by class. Structural (BP, CP, CP arrays, XF registers): what a translated list would check.
// Parametric: values a replay could apply as they are without changing what it does (TEV colour registers and konsts,
// projection, viewport, ambient and material colours, fog), then lights, matrices, and matrix words loaded from RAM by
// indexed loads inside the list: what it would re-upload.
enum { DK_BP, DK_CP, DK_ARR, DK_XFR, DK_PAR, DK_LIT, DK_MTX, DK_RAM, DK_CLASSES };
// Keys, each the one before plus more state: bytes + array bases, + structural, + parametric, + lights, + matrices.
enum { DKK_BYTES, DKK_STRUCT, DKK_PARAM, DKK_LIGHT, DKK_FULL, DKK_N };
// g_dlKeyStats indices.
enum {
    DS_CALLS, DS_NEW,
    DS_LAST,                       // + key: repeats against the same address's last use
    DS_PIC = DS_LAST + DKK_N,      // + key: seen in the previous picture (keys include the address)
    DS_WORDS = DS_PIC + DKK_N,     // + class: words read
    DS_SETCHG = DS_WORDS + DK_CLASSES,  // read sets that changed with the same bytes
    DS_TIMED, DS_TICKS,
    DS_TICKREP,                    // + key: ticks of timed calls whose key repeated its last use (census included)
    DS_NOTKEPT = DS_TICKREP + DKK_N, DS_PICTURES, DS_DRAWS,
    DS_DRAWPIC,                    // + key: draws in calls whose key was seen in the last picture (clean weights)
    DS_DRAWLAST = DS_DRAWPIC + DKK_N,  // + key: draws in calls whose key repeated its last use
    DS_DISTINCT = DS_DRAWLAST + DKK_N, // structural keys a picture, summed over pictures
    DS_SIDE,                       // calls with a side effect a cache could not replay (PE signals, copies, palette
                                   // loads, texture cache invalidation, unknown or nested commands)
    DS_XFLOAD,                     // calls that load matrices or lights themselves (XF 0x000-0x0FFF)
    // Superset keys (bytes + array bases + every register a draw could read, read or not, without matrices, lights and
    // copy / token / palette-load registers: what a signature kept up to date on register writes would see), the
    // structural one without the parametric words: last use, last picture, draws in calls seen in the last picture.
    DS_ZS_LAST, DS_ZS_PIC, DS_ZS_DRAWPIC, DS_ZP_LAST, DS_ZP_PIC, DS_ZP_DRAWPIC,
    // Misses attributed: calls with the same bytes and read set as the last use but another parametric key; the guard
    // words that differed in them; calls with more guard words than kept (attributed in part).
    DS_MISS, DS_MISSWORDS, DS_CUT,
    DKS_N
};
extern u64 g_dlKeyStats[DKS_N];
// Per state word (BP 0-255, CP 256-511, XF 512 + address, TEV banks, BP mask): attributed misses it took part in.
#define DLK_IDS (512 + 0x1058 + 17)
extern u32 g_dlKeyMiss[DLK_IDS];
void dlkey_name(u32 id, char* out, u32 n);
extern bool g_dlKey;        // census on
extern bool g_dlKeyPlant;   // self-test: every key made unique (every repeat share must read 0)
extern bool g_dlKeyActive;  // inside a traced call (fifo.c's run_list tests it per command)
// Zeroed memory for the census's tables (calloc by default; the 3DS takes them from linear memory: its heap has
// ~0.2 MB free in TP, jobs 1127-1129).
extern void* (*g_dlKeyAlloc)(u32 bytes);

void dlkey_begin(System* s, const GXCore* gx, u32 addr, const u8* p, u32 size);
void dlkey_cmd(System* s, const GXCore* gx, const u8* p, u32 avail);  // before each command inside the call
void dlkey_end(u64 ticks);   // ticks: the call's GX-thread time (0 when not timed)
void dlkey_picture(void);    // an EFB -> XFB copy: the picture window turns over
