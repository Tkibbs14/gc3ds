#pragma once
// G2, translated display lists (docs/research/G2_PLAN.md). Stage a: a display list called from the FIFO is decoded
// once into ops (state writes and runs of draws, as run_list would execute them) and replayed from those ops while its
// bytes (page stamps) and the vertex formats its parse depends on stay the same. Replaying calls the decoder's own
// state functions, so the GX state comes out as the decoder leaves it; only parsing is skipped. --g2 on|check|plant
// (3DS), G2=1|check|plant (PC); off by default.
#include "gx.h"

enum { G2_BP, G2_CP, G2_XF, G2_IDX, G2_NOP, G2_DRAW, G2_RUN };
typedef struct {
    u8 kind, a, vsize, flags;  // a: BP / CP register, indexed-load array, draw opcode, run VAT; flags: run of lines or
                               // points (bit 0), what ended the run (bits 4-7: g_dlRun's index 6-10, 0 the list's end)
    u16 n, m;                  // XF / NOP: words / commands; run: draws, commands (draws and NOPs)
    u32 v, w, x;               // BP / CP / indexed load: v = value; XF: v = address, w = the values' offset in the list;
                               // draw: v = its vertices' offset, w = count; run: v = offset, w = length, x = vertices
    u16 pc[8];                 // run: draws by primitive (GXCore.primCount)
} G2Op;

#define G2_GUARDS 8
typedef struct {
    u32 addr, size, stamp, hash;  // the list, the stamp its decoding saw, its bytes' hash (sampled checks)
    u32 ops;                      // first op in the pool
    u16 nops;
    u8 state, tries;              // G2E_*; decodings made for this address
    u8 ng, side;                  // guards; the list writes BP registers with side effects (PE, copies, palettes)
    u8 gid[G2_GUARDS];            // CP registers the parse read before the list wrote them, and their values
    u32 gval[G2_GUARDS];
} G2Entry;
enum { G2E_FREE, G2E_SEEN, G2E_READY, G2E_NEVER };

// g_g2Stats: [0] calls looked up, [1] replayed, [2] decoded, [3] decoded again after a stamp, [4] after a guard
// differed, [5] not decodable (unknown or nested commands, a command cut by the list's end, too many guards), [6] given
// up after too many decodings, [7] pool flushes, [8] table full, [9] checked against the decoder, [10] check
// differences, [11] byte hashes checked, [12] byte hashes that differed (a change the stamps missed), [13] ops replayed.
extern u64 g_g2Stats[14];
extern bool g_g2;          // replay on
extern u8 g_g2Check;       // 1: every 8th replay also runs the decoder on a copy of the state and compares; 2: plant
extern void* (*g_g2Alloc)(u32 bytes);  // zeroed memory for the table and the op pool (calloc; the 3DS: linear memory)

G2Entry* g2_lookup(u32 addr, u32 size);  // NULL when the table cannot hold it
// Ready to replay: decoded, its pages unstamped since, its guards hold. Otherwise (re)decodes it when it should be;
// false: run the list normally.
bool g2_prepare(G2Entry* e, const GXCore* gx, const u8* p);
const G2Op* g2_ops(const G2Entry* e);
u32 g2_hash(const u8* p, u32 size);
void g2_reset(void);  // forget every list (a state load)
