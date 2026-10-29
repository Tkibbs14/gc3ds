# Integer loop shaped like J3D/J2D list walking: load a field, test it, update two words, step a pointer.
# (core/tools/jitbench.py wraps this into a DOL; r1 is not used as a stack, nothing calls out.)
    .text
    .globl _start
_start:
    lis   3, 0x8010            # scratch base
    li    4, 256               # entries
    li    7, 0
fill:                          # make the list: each entry {next count flag pad}
    slwi  5, 7, 4
    add   6, 3, 5
    addi  8, 7, 1
    slwi  8, 8, 4
    cmpwi 8, 4096
    bne   nowrap
    li    8, 0
nowrap:
    add   8, 3, 8
    stw   8, 0(6)              # next
    stw   7, 4(6)              # count
    li    9, 1
    stw   9, 8(6)              # flag
    addi  7, 7, 1
    cmpw  7, 4
    blt   fill
    mr    10, 3
loop:
    lwz   11, 0(10)            # next
    lwz   12, 4(10)            # count
    lwz   0,  8(10)            # flag
    cmpwi 0, 0
    beq   skip
    addi  12, 12, 1
    stw   12, 4(10)
    andi. 0, 12, 7
    stw   0, 12(10)
skip:
    mr    10, 11
    b     loop
