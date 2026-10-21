#pragma once
// A tiny synthetic ABI callee, compiled only into the fuzzer; no commercial code or hashes.
#define X3_TEST_ID 0x20000u
#define X3_TEST_PC 0x80009000u
static const u32 x3_native_words[] = {
    0x93e40000u, // stw r31,0(r4)
    0x3be30001u, // addi r31,r3,1
    0xc0240004u, // lfs f1,4(r4)
    0xec21082au, // fadds f1,f1,f1
    0x80640004u, // lwz r3,4(r4)
    0x83e40000u, // lwz r31,0(r4)
    0x4e800020u
};
