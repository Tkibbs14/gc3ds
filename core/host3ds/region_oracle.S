// X4 fixed ARM oracles. These are deliberately handwritten functions, not code generators.
// ABI: r0 Gekko*, r1 big-endian MEM1. Guest volatile registers and dead stack slots are not live at return
// (the same boundary contract as port_check); nonvolatile GPR/FPR/CR fields and caller memory are checked.
.syntax unified
.arm
.fpu vfp
.text
.align 2
.macro ENTER
    push {r4-r11,lr}
    sub sp, sp, #4
    vpush {d8-d15}
    mov r4, r0
    mov r5, r1
.endm
.macro LEAVE
    ldr r0, [r4, #648]
    str r0, [r4, #664]
    vpop {d8-d15}
    add sp, sp, #4
    pop {r4-r11,pc}
.endm
// No guest-memory range/alignment tests. Inputs have the captured call's standard MEM1 mappings.
.macro PTR dst, ea
    bic \dst, \ea, #0xc0000000
    add \dst, \dst, r5
.endm
.macro LD dst, base, off=0
    PTR lr, \base
    ldr \dst, [lr, #\off]
    rev \dst, \dst
.endm
.macro ST val, base, off=0
    PTR lr, \base
    rev r12, \val
    str r12, [lr, #\off]
.endm
.macro PIPE val, size
    mov r1, \val
    mov r0, r4
    mov r2, #\size
    bl x4_pipe_word
.endm
.global x4_empty
.type x4_empty, %function
x4_empty:
    ENTER
    LEAVE
.size x4_empty, .-x4_empty

.global x4_pos
.type x4_pos, %function
x4_pos:
    ENTER
    ldr r6, [r4, #12]       // guest r3 matrix
    ldr r7, [r4, #16]       // guest r4 matrix index
    mov r0, #16
    PIPE r0, 1
    mov r0, r7, lsl #2
    orr r0, r0, #0xb0000
    PIPE r0, 4
    // The six plain-float psq loads/stores are inlined, with exact-single lanes. No helper/round-trip check.
    // Keep the guest's load-before-store ordering (the pipe cannot alias the matrix).
    PTR r6, r6
    ldr r0,=0x3f800000
    vmov s31,r0
    .irp off,0,4,8,12,16,20,24,28,32,36,40,44
    ldr r1, [r6, #\off]
    rev r1, r1
    vmov s0,r1
    vmul.f32 s0,s0,s31    // canonical NaN / FTZ on a float load, preserving the sign of zero
    vmov r1,s0
    mov r0, r4
    mov r2, #4
    bl x4_pipe_word
    .endr
    LEAVE
.size x4_pos, .-x4_pos

.global x4_tex
.type x4_tex, %function
x4_tex:
    ENTER
    ldr r6, [r4, #12]       // r30: object EA
    ldr r7, [r4, #16]       // region EA; dead after its two words are sent
    ldr r8, [r4, #20]       // r31: texture unit, live across optional TLUT callback
    ldr r9, [r4, #52]       // guest small data r13
    ldr r0, [r4, #4]
    ldr r1, [r4, #648]
    ST r1, r0, 4           // caller's LR save word is observable above entry SP
    sub r1, r0, #64
    ST r0, r1
    str r1, [r4, #4]
    // Each ID table supplies only the top byte. Values are reloaded after stores exactly as in the guest.
    sub r10, r9, #0x7a00
    sub r10, r10, #0xf0    // r13 -31472
    PTR r10, r10
    ldrb r0, [r10, r8]
    LD r1, r6
    bic r1, r1, #0xff000000
    orr r1, r1, r0, lsl #24
    ST r1, r6
    add r10, r10, r8
    ldrb r0, [r10, #8]
    LD r1, r6, 4
    bic r1, r1, #0xff000000
    orr r1, r1, r0, lsl #24
    ST r1, r6, 4
    ldrb r0, [r10, #16]
    LD r1, r6, 8
    bic r1, r1, #0xff000000
    orr r1, r1, r0, lsl #24
    ST r1, r6, 8
    ldrb r0, [r10, #24]
    LD r1, r7
    bic r1, r1, #0xff000000
    orr r1, r1, r0, lsl #24
    ST r1, r7
    ldrb r0, [r10, #32]
    LD r1, r7, 4
    bic r1, r1, #0xff000000
    orr r1, r1, r0, lsl #24
    ST r1, r7, 4
    ldrb r0, [r10, #40]
    LD r1, r6, 12
    bic r1, r1, #0xff000000
    orr r1, r1, r0, lsl #24
    ST r1, r6, 12
    mov r0, #97
    PIPE r0, 1
    LD r0, r6
    PIPE r0, 4
    mov r0, #97
    PIPE r0, 1
    LD r0, r6, 4
    PIPE r0, 4
    mov r0, #97
    PIPE r0, 1
    LD r0, r6, 8
    PIPE r0, 4
    mov r0, #97
    PIPE r0, 1
    LD r0, r7
    PIPE r0, 4
    mov r0, #97
    PIPE r0, 1
    LD r0, r7, 4
    PIPE r0, 4
    mov r0, #97
    PIPE r0, 1
    LD r0, r6, 12
    PIPE r0, 4
    PTR r0, r6
    ldrb r0, [r0, #31]
    tst r0, #2             // CR0 is dead after this branch; keep it only in ARM flags
    bne .Ltex_no_tlut
    ldr r0, [r4, #8]
    sub r0, r0, #0x3400
    sub r0, r0, #0x80
    LD r10, r0            // __GXData
    LD r1, r6, 24
    str r1, [r4, #12]
    str r10, [r4, #16]
    LD r1, r10, 1228
    mov r0, r4
    ldr r2, =0x8035e3a4
    bl x4_call
    ldr r7, [r4, #12]      // TLUT callback result
    sub r0, r9, #0x7a00
    sub r0, r0, #0xc0      // r13 -31424
    PTR r0, r0
    ldrb r0, [r0, r8]
    LD r1, r7, 4
    bic r1, r1, #0xff000000
    orr r1, r1, r0, lsl #24
    ST r1, r7, 4
    mov r0, #97
    PIPE r0, 1
    LD r0, r7, 4
    PIPE r0, 4
.Ltex_no_tlut:
    ldr r0, [r4, #8]
    sub r0, r0, #0x3400
    sub r0, r0, #0x80
    LD r10, r0
    add r7, r10, r8, lsl #2
    LD r0, r6, 8
    ST r0, r7, 1300
    LD r0, r6
    ST r0, r7, 1332
    LD r0, r10, 1452
    orr r0, r0, #1
    ST r0, r10, 1452
    PTR r10, r10
    mov r0, #0
    strh r0, [r10, #2]
    ldr r0, [r4, #4]
    add r0, r0, #64
    str r0, [r4, #4]
    LD r1, r0, 4
    str r1, [r4, #648]
    LEAVE
.size x4_tex, .-x4_tex
.ltorg

// J2DPane::draw: function-scope allocation. g27..31 stay in r6..r10; g1 in r11.
// f0..f2 in d0..d2, f30/f31 in d14/d15. Comparisons feed ARM branches directly.
// External (including recursive) calls use the existing JIT through a common, deliberately conservative C bridge.
.macro FL fd, base, off
    LD r0, \base, \off
    vmov s12, r0
    vcvt.f64.f32 \fd, s12
.endm
.macro FS fd, base, off
    vcvt.f32.f64 s12, \fd
    vmov r0, s12
    ST r0, \base, \off
.endm
// The destination FPR is dead after the immediately following store: keep the single bits, not a double round-trip.
.macro FADD_STORE addend, base, off
    vadd.f64 d0,d0,\addend
    vcvt.f32.f64 s12,d0
    vmov r0,s12
    ST r0,\base,\off
.endm
.macro FADD fd, fa, fb
    vadd.f64 \fd, \fa, \fb
    vcvt.f32.f64 s12, \fd
    vcvt.f64.f32 \fd, s12
.endm
.macro FSUB fd, fa, fb
    vsub.f64 \fd, \fa, \fb
    vcvt.f32.f64 s12, \fd
    vcvt.f64.f32 \fd, s12
.endm
.macro FCMP fa, fb
    vcmp.f64 \fa, \fb
    vmrs APSR_nzcv, fpscr
.endm
.macro PSAVE
    str r0, [r4]
    str r1, [r4, #12]
    str r2, [r4, #16]
    str r3, [r4, #20]
    str r11, [r4, #4]
    add r12, r4, #108
    stmia r12, {r6-r10}
    vstr d0, [r4, #128]
    vstr d1, [r4, #144]
    vstr d2, [r4, #160]
    vstr d14, [r4, #608]
    vstr d15, [r4, #624]
.endm
.macro PRELOAD
    ldr r0, [r4]
    ldr r1, [r4, #12]
    ldr r2, [r4, #16]
    ldr r3, [r4, #20]
    ldr r11, [r4, #4]
    add r12, r4, #108
    ldmia r12, {r6-r10}
    vldr d0, [r4, #128]
    vldr d1, [r4, #144]
    vldr d2, [r4, #160]
    vldr d14, [r4, #608]
    vldr d15, [r4, #624]
.endm
.macro PCALL target, ret
    PSAVE
    mov r0, r4
    ldr r1, =\target
    ldr r2, =\ret
    bl x4_call
    PRELOAD
.endm
.macro PICALL off, ret
    ldr r0,[r4,#48]
    LD r12,r0
    ldr lr,=0x4e800020
    cmp r12,lr
    beq .Lpic_empty\@
    PSAVE
    ldr r1, [r4, #48]      // guest r12 target, not host r12 scratch
    mov r0, r4
    ldr r2, =\ret
    bl x4_call
    PRELOAD
.Lpic_empty\@:
.endm
.macro VTARGET obj, off
    LD r0, \obj
    LD r0, r0, \off
    str r0, [r4, #48]
    str r0, [r4, #652]    // mtctr survives the call
.endm
.macro COPYWORD src, so, dst, do
    LD r0, \src, \so
    ST r0, \dst, \do
.endm

// PSMTXConcat leaf, expanded without a guest call/exit. All 24 input singles stay in s0-s23.
// Liveness frees f30/f31's host d14/d15 temporarily (two spills); d12/d13 are the output pair, d14/d15 scratch.
// This retains the guest's load-before-store alias safety and PS rounding, rather than scalarizing through a stack.
.macro MLDS sd, base, off
    LD r0,\base,\off
    vmov \sd,r0
    vmul.f32 \sd,\sd,s31
.endm
.macro MROUND dd, ss
    vcvt.f32.f64 \ss,\dd
    vcvt.f64.f32 \dd,\ss
.endm
.macro MPROUND
    vcvt.f32.f64 s24,d12
    vcvt.f32.f64 s26,d13
    vcvt.f64.f32 d12,s24
    vcvt.f64.f32 d13,s26
.endm
.macro MPAIR a0,a1,a2,a3,b0,b1,b2,b3,b4,b5,off,side=0,tail=0
    vcvt.f64.f32 d14,\a0
    vcvt.f64.f32 d15,\b0
    vmul.f64 d12,d14,d15
    vcvt.f64.f32 d15,\b1
    vmul.f64 d13,d14,d15
    MPROUND
    vcvt.f64.f32 d14,\a1
    vcvt.f64.f32 d15,\b2
    vmla.f64 d12,d14,d15
    vcvt.f64.f32 d15,\b3
    vmla.f64 d13,d14,d15
    MPROUND
    vcvt.f64.f32 d14,\a2
    vcvt.f64.f32 d15,\b4
    vmla.f64 d12,d14,d15
    vcvt.f64.f32 d15,\b5
    vmla.f64 d13,d14,d15
    MPROUND
    .if \tail
    vcvt.f64.f32 d14,\a3
    ldr r0,=0x4509d0
    ldr r0,[r5,r0]
    rev r0,r0
    vmov s30,r0
    vcvt.f64.f32 d15,s30
    vmla.f64 d12,d14,d15
    ldr r0,=0x4509d4
    ldr r0,[r5,r0]
    rev r0,r0
    vmov s30,r0
    vcvt.f64.f32 d15,s30
    vmla.f64 d13,d14,d15
    MPROUND
    .endif
    .if \side
    vstr d13,[r4,#\side]   // f14.ps1 / f15.ps1 survive the guest's lfd restores
    .endif
    vcvt.f32.f64 s24,d12
    vmov r0,s24
    rev r0,r0
    str r0,[r3,#\off]
    vcvt.f32.f64 s26,d13
    vmov r0,s26
    rev r0,r0
    str r0,[r3,#(\off+4)]
.endm
.macro MCONCAT
    vpush {d14-d15}
    ldr r0,=0x3f800000
    vmov s31,r0
    MLDS s0,r2,0
    MLDS s1,r2,4
    MLDS s2,r2,8
    MLDS s3,r2,12
    MLDS s4,r2,16
    MLDS s5,r2,20
    MLDS s6,r2,24
    MLDS s7,r2,28
    MLDS s8,r2,32
    MLDS s9,r2,36
    MLDS s10,r2,40
    MLDS s11,r2,44
    MLDS s12,r1,0
    MLDS s13,r1,4
    MLDS s14,r1,8
    MLDS s15,r1,12
    MLDS s16,r1,16
    MLDS s17,r1,20
    MLDS s18,r1,24
    MLDS s19,r1,28
    MLDS s20,r1,32
    MLDS s21,r1,36
    MLDS s22,r1,40
    MLDS s23,r1,44
    PTR r3,r3
    MPAIR s12,s13,s14,s15,s0,s1,s4,s5,s8,s9,0
    MPAIR s12,s13,s14,s15,s2,s3,s6,s7,s10,s11,8,0,1
    MPAIR s16,s17,s18,s19,s0,s1,s4,s5,s8,s9,16,360
    MPAIR s16,s17,s18,s19,s2,s3,s6,s7,s10,s11,24,376,1
    MPAIR s20,s21,s22,s23,s0,s1,s4,s5,s8,s9,32
    MPAIR s20,s21,s22,s23,s2,s3,s6,s7,s10,s11,40,0,1
    ldr r0,=0x4509d4
    ldr r0,[r5,r0]
    rev r0,r0
    vmov s30,r0
    vcvt.f64.f32 d15,s30
    vstr d15,[r4,#632]
    vpop {d14-d15}
.endm
.global x4_pane
.type x4_pane, %function
x4_pane:
    ENTER
    sub sp, sp, #64
    add r12, r4, #108
    ldmia r12, {r6-r10}
    stmia sp, {r6-r10}    // original guest nonvolatile registers, separate from the emulated stack
    vldr d0, [r4, #608]
    vldr d1, [r4, #616]
    vldr d2, [r4, #624]
    vldr d3, [r4, #632]
    vstr d0, [sp, #24]
    vstr d1, [sp, #32]
    vstr d2, [sp, #40]
    vstr d3, [sp, #48]
    ldr r11, [r4, #4]
    ldr r0, [r4, #648]
    ST r0, r11, 4
    mov r0, r11
    sub r11, r11, #272
    ST r0, r11
    ldr r6, [r4, #12]
    ldr r7, [r4, #16]
    ldr r8, [r4, #20]
    vldr d14, [r4, #144]
    vldr d15, [r4, #160]
    mov r10, #0
    ldr r0, [r4, #24]
    tst r0, #255
    beq .Lp6834
    PTR r0, r6
    ldrb r0, [r0, #176]
    cmp r0, #0
    movne r10, #1
.Lp6834:
    mov r1, r7
    VTARGET r7, 32
    ldr r0,[r4,#48]
    LD r12,r0
    ldr lr,=0x38600001
    cmp r12,lr
    bne .Lp_type_call
    LD r12,r0,4
    ldr lr,=0x4e800020
    cmp r12,lr
    bne .Lp_type_call
    mov r1,#1
    b .Lp_type_done
.Lp_type_call:
    PICALL 32, 0x802f6848
.Lp_type_done:
    cmp r1, #1
    movne r8, #0
    LD r1, r6, 236
    mov r9, #0
    cmp r1, #0
    beq .Lp6868
    LD r9, r1, 12
.Lp6868:
    FL d1, r6, 40
    FL d0, r6, 32
    FCMP d1, d0
    blt .Lp_exit
    bvs .Lp_exit
    FL d1, r6, 44
    FL d0, r6, 36
    FCMP d1, d0
    blt .Lp_exit
    bvs .Lp_exit
    COPYWORD r6,32,r6,48
    COPYWORD r6,36,r6,52
    COPYWORD r6,40,r6,56
    COPYWORD r6,44,r6,60
    FL d1, r6, 212
    FL d2, r6, 216
    FL d0, r6, 48
    FADD_STORE d1,r6,48
    FL d0,r6,52
    FADD_STORE d2,r6,52
    FL d0,r6,56
    FADD_STORE d1,r6,56
    FL d0,r6,60
    FADD_STORE d2,r6,60
    cmp r10,#0
    beq .Lp6934
    COPYWORD r6,32,r6,64
    COPYWORD r6,36,r6,68
    COPYWORD r6,40,r6,72
    COPYWORD r6,44,r6,76
    mov r1,r6
    VTARGET r6,88
    PICALL 88,0x802f6934
.Lp6934:
    cmp r9,#0
    beq .Lp6a54
    FL d1,r9,48
    FL d0,r9,32
    FSUB d2,d1,d0
    FL d1,r9,52
    FL d0,r9,36
    FSUB d1,d1,d0
    FL d0,r6,48
    FADD_STORE d2,r6,48
    FL d0,r6,52
    FADD_STORE d1,r6,52
    FL d0,r6,56
    FADD_STORE d2,r6,56
    FL d0,r6,60
    FADD_STORE d1,r6,60
    add r1,r9,#128
    add r2,r6,#80
    add r3,r6,#128
    MCONCAT
    cmp r10,#0
    beq .Lp6adc
    tst r8,#255
    beq .Lp6a24
    COPYWORD r6,48,r6,64
    COPYWORD r6,52,r6,68
    COPYWORD r6,56,r6,72
    COPYWORD r6,60,r6,76
    FL d0,r6,64
    FL d1,r9,64
    FCMP d0,d1
    bhi .Lp69dc
    FS d1,r6,64
.Lp69dc:
    FL d0,r6,68
    FL d1,r9,68
    FCMP d0,d1
    bhi .Lp69f4
    FS d1,r6,68
.Lp69f4:
    FL d0,r6,72
    FL d1,r9,72
    FCMP d0,d1
    blt .Lp6a0c
    bvs .Lp6a0c
    FS d1,r6,72
.Lp6a0c:
    FL d0,r6,76
    FL d1,r9,76
    FCMP d0,d1
    blt .Lp6a24
    bvs .Lp6a24
    FS d1,r6,76
.Lp6a24:
    PTR r12,r6
    ldrb r0,[r12,#178]
    strb r0,[r12,#179]
    ldrb r1,[r12,#180]
    cmp r1,#0
    beq .Lp6adc
    PTR lr,r9
    ldrb r1,[lr,#179]
    mul r2,r0,r1
    // divw by 255, inlined exact for the byte*byte range 0..65025.
    add r2,r2,#1
    add r2,r2,r2,lsr #8
    mov r2,r2,lsr #8
    strb r2,[r12,#179]
    b .Lp6adc
.ltorg
.Lp6a54:
    FL d0,r6,48
    FADD_STORE d14,r6,48
    FL d0,r6,52
    FADD_STORE d15,r6,52
    FL d0,r6,56
    FADD_STORE d14,r6,56
    FL d0,r6,60
    FADD_STORE d15,r6,60
    FL d0,r6,212
    FADD d1,d0,d14
    FL d0,r6,216
    FADD d2,d0,d15
    mov r1,r6
    VTARGET r6,68
    PICALL 68,0x802f6aa8
    add r1,r6,#80
    add r2,r6,#128
    .irp off,0,4,8,12,16,20,24,28,32,36,40,44
    FL d0,r1,\off
    FS d0,r2,\off
    .endr
    COPYWORD r6,48,r6,64
    COPYWORD r6,52,r6,68
    COPYWORD r6,56,r6,72
    COPYWORD r6,60,r6,76
    PTR r12,r6
    ldrb r0,[r12,#178]
    strb r0,[r12,#179]
.Lp6adc:
    ldr r0,[r4,#8]
    sub r0,r0,#0x3800
    sub r0,r0,#0x10
    FL d0,r0,0
    FS d0,r11,8
    FS d0,r11,12
    FS d0,r11,16
    FS d0,r11,20
    cmp r10,#0
    beq .Lp6b10
    tst r8,#255
    beq .Lp6b10
    mov r1,r7
    add r2,r11,#8
    add r3,r6,#64
    PCALL 0x802e987c,0x802f6b10
.Lp6b10:
    cmp r10,#0
    beq .Lp6ca8
    FL d1,r6,72
    FL d0,r6,64
    FCMP d1,d0
    blt .Lp6b50
    bvs .Lp6b50
    FL d1,r6,76
    FL d0,r6,68
    FCMP d1,d0
    bge .Lp6b58
.Lp6b50:
    tst r8,#255
    bne .Lp6ca8
.Lp6b58:
    ldr r0,=0x803cc9b8
    ST r0,r11,24
    // The context copy retains both guest loops, rather than specializing them to the captured path.
    .irp off,4,8,12,16,20,24,28,32
    FL d0,r7,\off
    FS d0,r11,(\off+24)
    .endr
    .irp off,36,40,44,48
    COPYWORD r7,\off,r11,(\off+24)
    .endr
    PTR r12,r7
    ldrb r0,[r12,#52]
    PTR lr,r11
    strb r0,[lr,#76]
    FL d0,r7,56
    FS d0,r11,80
    FL d0,r7,60
    FS d0,r11,84
    add r3,r11,#84
    add r2,r7,#60
    str r10,[sp,#60]
    mov r10,#8
.Lp6bec:
    LD r1,r2,4
    LD r12,r2,8
    add r2,r2,#8
    ST r12,r3,8
    ST r1,r3,4
    add r3,r3,#8
    subs r10,r10,#1
    bne .Lp6bec
    add r9,r11,#152
    sub r3,r9,#4
    add r2,r7,#124
    mov r10,#6
.Lp6c14:
    LD r1,r2,4
    LD r12,r2,8
    add r2,r2,#8
    ST r12,r3,8
    ST r1,r3,4
    add r3,r3,#8
    subs r10,r10,#1
    bne .Lp6c14
    ldr r10,[sp,#60]
    // Final mixed-size context bytes. Byte copies preserve the unaligned lhz/sth pairs exactly.
    PTR r1,r7
    PTR r2,r11
    .irp off,176,177,178,179,180,181,182,183,184
    ldrb r0,[r1,#\off]
    strb r0,[r2,#(\off+24)]
    .endr
    tst r8,#255
    beq .Lp6c74
    add r1,r11,#24
    add r2,r11,#8
    PCALL 0x802e90c0,0x802f6c6c
    add r1,r11,#24
    PCALL 0x802e8e20,0x802f6c74
.Lp6c74:
    PTR r0,r6
    ldrb r1,[r0,#177]
    ldr r0,[r4,#8]
    sub r0,r0,#0x3400
    sub r0,r0,#0x80
    LD r2,r0
    and r3,r1,#1
    mov r3,r3,lsl #1
    and r0,r1,#2
    orr r3,r3,r0,lsr #1
    and r3,r3,#3
    LD r0,r2,516
    bic r0,r0,#0xc000
    orr r0,r0,r3,lsl #14
    ST r0,r2,516
    LD r0,r2,1452
    orr r0,r0,#4
    ST r0,r2,1452
    mov r1,r6
    vmov.f64 d1,d14
    vmov.f64 d2,d15
    mov r2,r9
    VTARGET r6,56
    PICALL 56,0x802f6c9c
    ldr r0,=0x803cc9b8
    ST r0,r11,24
.Lp6ca8:
    LD r6,r6,220
    cmp r6,#0
    subne r6,r6,#12
    b .Lp6ce8
.Lp6cbc:
    LD r1,r6,12
    ldr r0,[r4,#8]
    sub r0,r0,#0x3800
    sub r0,r0,#0x10
    FL d1,r0,0
    vmov.f64 d2,d1
    mov r2,r7
    mov r3,r8
    str r10,[r4,#24]
    PCALL 0x802f67e0,0x802f6cd8
    LD r6,r6,24
    cmp r6,#0
    subne r6,r6,#12
.Lp6ce8:
    cmp r6,#0
    bne .Lp6cbc
.Lp_exit:
    ldmia sp,{r6-r10}
    add r12,r4,#108
    stmia r12,{r6-r10}
    vldr d0,[sp,#24]
    vldr d1,[sp,#32]
    vldr d2,[sp,#40]
    vldr d3,[sp,#48]
    vstr d0,[r4,#608]
    vstr d1,[r4,#616]
    vstr d2,[r4,#624]
    vstr d3,[r4,#632]
    add r11,r11,#272
    str r11,[r4,#4]
    LD r0,r11,4
    str r0,[r4,#648]
    add sp,sp,#64
    LEAVE
.size x4_pane,.-x4_pane
.ltorg

.section .note.GNU-stack,"",%progbits
