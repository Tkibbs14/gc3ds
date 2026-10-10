#pragma once
#include <3ds/types.h>

// Minimal Gekko (PowerPC 750CL) state for the benchmark. The JIT reads this layout by offset (see PPC_OFF_*).
// Each FPR is a paired single: ps0 is the scalar value, ps1 the second lane (single-precision ops write both).
typedef struct {
    u32 gpr[32];
    float ps0[32];
    float ps1[32];
    u32 cr, xer, pc;
    u32 gqr[8];
    u32 hostFpscr;  // JIT scratch: the host FPSCR saved on block entry
} PPCState;

#define PPC_OFF_PS0 128
#define PPC_OFF_PS1 256
#define PPC_OFF_HOSTFPSCR (384 + 12 + 32)

enum {
    OP_HALT, OP_ADDI, OP_ADDIS, OP_ORI, OP_ADD, OP_XOR, OP_RLWINM, OP_LWZ, OP_STW, OP_ADDIC_RC, OP_BC,
    OP_LFS, OP_STFS, OP_FADDS, OP_FMULS, OP_FMADDS,
    OP_PS_ADD, OP_PS_SUB, OP_PS_MUL, OP_PS_MADD, OP_PSQ_L, OP_PSQ_ST,
};

// One pre-decoded instruction. d/a/b/c are the raw PowerPC fields; each op knows which is its destination.
// For psq_l/psq_st, `c` holds the GQR index and `sh` the W (ps0-only) bit.
typedef struct {
    u8 op, d, a, b, c, sh, bo, bi;
    s32 imm;
    u32 mask;
    s32 target;
} Decoded;

// GameCube physical RAM is mirrored at 0x80000000 (cached) and 0xC0000000 (uncached); masking folds both mirrors.
#define GC_ADDR_MASK 0x01FFFFFFu

// GQR quantisation types.
enum { GQR_FLOAT = 0, GQR_U8 = 4, GQR_U16 = 5, GQR_S8 = 6, GQR_S16 = 7 };

int ppc_decode(const u32* code, int n, Decoded* out, char* err, int errlen);
// Guest RAM layouts. BE: bytes as on the console (32-bit loads need a byte swap). SWAPPED: each aligned 32-bit
// word stored as a native little-endian value, so word and float accesses need no swap and 8/16-bit accesses XOR
// the address (a ^ 3 for bytes, a ^ 2 for halfwords).
// BE_SETEND: RAM stays in console byte order, and JIT blocks run with the CPU in big-endian data mode (SETEND BE),
// so every guest load and store, VFP ones included, is a plain native access. Interpreter-wise it is RAM_BE.
enum { RAM_BE = 0, RAM_SWAPPED = 1, RAM_BE_SETEND = 2 };

void ppc_interp(PPCState* s, u8* ram, const Decoded* prog, int layout);
void ram_to_layout(u8* ram, u32 bytes, int layout);  // converts a BE image in place

// JIT modes for paired singles: one VFP op per lane, or one VFP short-vector op (FPSCR.LEN = 2) per pair.
enum { JIT_PS_SCALAR = 0, JIT_PS_VECTOR = 1 };

typedef void (*JitBlock)(PPCState* s, u8* ram);
// GQRs are treated as constant for the block (games set them once); `gqr` gives their values at compile time.
// pc is set to PC_BAILED if the block hit an access its fast path does not handle (unaligned paired access).
#define PC_BAILED 0xFFFFFFFFu
int jit_compile(const Decoded* prog, int n, u32* buf, u32 capWords, int psMode, int layout, const u32* gqr, char* err, int errlen);
void jit_flags(PPCState* s, u32 cpsr);
