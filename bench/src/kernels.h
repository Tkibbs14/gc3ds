#pragma once
// Hand-emitted ARM micro-kernels: the code shapes an emulator's JIT would produce, isolated so each cost can be timed.
// Pure code generation (no 3DS services), so the qemu check can run them too.
#include <3ds/types.h>

// Block chaining. Blocks are 4 guest-equivalent ALU ops each; K must be a power of two; next(i) visits every block.
static inline u32 kern_next(u32 i, u32 K) { return (i * 5 + 1) & (K - 1); }
static inline u32 kern_block_pc(u32 i) { return 0x80000000u + i * 16; }

// Linked: block i branches straight to block next(i) while a downcount in r12 lasts. Entry is buf[0]:
// u32 f(u32 steps) returns 4 * steps.
int kern_linked(u32* buf, u32 capWords, u32 K, u32 slotWords);

// Dispatched: each block is a function returning the guest PC of its successor; a C loop looks the PC up in a
// direct-mapped table. blockWord[i] receives each block's word offset.
int kern_dispatch(u32* buf, u32 capWords, u32 K, u32 slotWords, u32* blockWord);
typedef struct { u32 pc; u32 (*fn)(void); } KernEntry;
u32 kern_run_dispatch(const KernEntry* tab, u32 K, u32 steps);  // returns the final guest PC, 0xDEAD on a miss

// Memory access paths over guest addresses 0x80000000 + 4k. u32 f(u32 count, u8* ram) returns the sum of the
// byte-swapped words read: identical for all three variants.
enum { KMEM_FASTMEM, KMEM_CHECKED, KMEM_CALL, KMEM_COUNT };
extern const char* const kern_mem_names[KMEM_COUNT];
int kern_mem(u32* buf, u32 capWords, int variant);
void kern_set_ram(u8* ram);

// VFP operation cost: void f(u32 iters) runs iters * KERN_FP_UNROLL copies of one operation, as one dependent chain
// (latency) or rotating over independent registers (throughput). Restores FPSCR and s16-s31.
#define KERN_FP_UNROLL 32
enum {
    KFP_ADD32, KFP_MUL32, KFP_MLA32, KFP_DIV32, KFP_SQRT32,
    KFP_ADD64, KFP_MUL64, KFP_MLA64, KFP_DIV64,
    KFP_VADD2, KFP_VMUL2, KFP_VMLA2,
    KFP_CVT_F_I, KFP_CVT_I_F,
    KFP_VMOV_SR, KFP_VMOV_RS, KFP_VMOV_SSRR, KFP_VMOV_RRSS, KFP_VMOV_RT, KFP_SETEND,
    KFP_COUNT
};
extern const char* const kern_fp_names[KFP_COUNT];
int kern_fp(u32* buf, u32 capWords, int which, int dependent);
