# Paired singles shaped like PSMTXConcat: pair loads, multiply-add chains, pair stores (GQR0 = float, unscaled).
    .text
    .globl _start
_start:
    lis   3, 0x8010
    lis   4, 0x3f80
    stw   4, 0(3)
    stw   4, 4(3)
    stw   4, 8(3)
    stw   4, 12(3)
loop:
    psq_l     1, 0(3), 0, 0
    psq_l     2, 8(3), 0, 0
    ps_mul    3, 1, 2
    ps_madds0 4, 3, 1, 2
    ps_madds1 5, 4, 2, 1
    ps_add    6, 5, 3
    psq_st    6, 16(3), 0, 0
    psq_st    4, 24(3), 0, 0
    b         loop
