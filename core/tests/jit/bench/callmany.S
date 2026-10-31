# One leaf called from eight different sites, which is what the game's SDK code looks like (PSMTXConcat,
# J3DGDSetLightAttn and the GX setters are each called from everywhere). The point of this case is the *return*: with
# a single call site the JIT can link the leaf's blr straight to the one caller and the indirect table is never
# probed (callloop.S: 128 probes in 486 M guest instructions). With eight sites the return target keeps changing, so
# the link cannot stay valid and the blr has to go through ST_INDIRECT - which is where --indstat counts (C48).
# Run both with --indstat to see the difference:
#   GC3DS_OPT=42276727 GC3DS_NOFAULTMEM=1 qemu-arm ... build-arm/gcemu CASE.dol --seconds 1 --jit
    .text
    .globl _start
_start:
    lis   1, 0x8011            # a stack
    li    3, 0
outer:
    mflr  0
    stw   0, 4(1)
    stwu  1, -32(1)
    stw   31, 28(1)
    mr    31, 3
    bl    leaf
    bl    leaf
    bl    leaf
    bl    leaf
    bl    leaf
    bl    leaf
    bl    leaf
    bl    leaf
    mr    3, 31
    lwz   31, 28(1)
    addi  1, 1, 32
    lwz   0, 4(1)
    mtlr  0
    addi  3, 3, 1
    b     outer
leaf:
    addi  3, 3, 1
    mulli 4, 3, 7
    add   3, 3, 4
    blr
