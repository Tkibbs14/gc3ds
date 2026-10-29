# CodeWarrior-shaped calls: a prologue that saves the link register and callee-saved words, a leaf, a return.
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
    stw   30, 24(1)
    mr    31, 3
    bl    leaf
    mr    3, 31
    bl    leaf
    lwz   31, 28(1)
    lwz   30, 24(1)
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
