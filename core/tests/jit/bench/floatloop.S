# Scalar float chain shaped like J3D vector maths: loads, a dependent multiply-add chain, a store.
    .text
    .globl _start
_start:
    lis   3, 0x8010            # scratch
    lis   4, 0x3f80            # 1.0f
    stw   4, 0(3)
    addis 4, 4, 1
    stw   4, 4(3)
    addis 4, 4, 1
    stw   4, 8(3)
    addis 4, 4, 1
    stw   4, 12(3)
loop:
    lfs   1, 0(3)
    lfs   2, 4(3)
    lfs   3, 8(3)
    fmuls 4, 1, 2              # the chain J3D's transforms are made of
    fmuls 5, 2, 3
    fadds 6, 4, 5
    fmuls 7, 6, 1
    fadds 8, 7, 3
    stfs  8, 16(3)
    lfs   9, 12(3)
    fmuls 10, 8, 9
    stfs  10, 20(3)
    b     loop
