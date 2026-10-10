// Cross-checks gekko_disasm against GNU objdump -d -M gekko,raw: reads objdump's listing on stdin and compares the
// mnemonic and the operand text of every instruction (whitespace-normalized).
//   disasm_check [-v] < listing
// Prints "DISASM checked=<n> mnemonic_mismatch=<a> operand_mismatch=<b>"; exit 1 on any mismatch.
#include "../../src/cpu/gekko.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 g_logMask;
void emu_log(LogCat cat, const char* fmt, ...) { (void)cat; (void)fmt; }
void gekko_on_fatal(Gekko* g, const char* what) { (void)g; (void)what; }

static void norm(const char* in, char* out, size_t n) {
    size_t k = 0;
    bool sp = false;
    for (; *in && k + 1 < n; in++) {
        if (*in == ' ' || *in == '\t') { sp = true; continue; }
        if (sp && k) out[k++] = ' ';
        sp = false;
        out[k++] = *in;
    }
    out[k] = 0;
}

int main(int argc, char** argv) {
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    char line[512];
    unsigned checked = 0, mnBad = 0, opBad = 0;
    while (fgets(line, sizeof line, stdin)) {
        unsigned addr, b0, b1, b2, b3;
        int pos = 0;
        if (sscanf(line, " %x: %2x %2x %2x %2x %n", &addr, &b0, &b1, &b2, &b3, &pos) != 5 || !pos) continue;
        char* text = line + pos;
        char* nl = strchr(text, '\n');
        if (nl) *nl = 0;
        char* hash = strstr(text, " <");  // drop symbol annotations
        if (hash) *hash = 0;
        u32 inst = (u32)(b0 << 24 | b1 << 16 | b2 << 8 | b3);
        char mine[128], a[256], b[256];
        gekko_disasm(addr, inst, mine, sizeof mine);
        norm(text, a, sizeof a);
        // objdump prints branch targets as bare hex; ours carry 0x.
        char* x;
        while ((x = strstr(mine, "0x")) && (x == mine || x[-1] == ',' || x[-1] == ' ')) memmove(x, x + 2, strlen(x + 2) + 1);
        norm(mine, b, sizeof b);
        // objdump -M raw prints optional operands of later ISA versions (EH, BH, L, mfocrf's FXM) that Gekko does
        // not have; drop a trailing ",0" / ",-1" / " 0" that ours lacks.
        size_t la = strlen(a), lb = strlen(b);
        if (la > lb && !strncmp(a, b, lb) &&
            (!strcmp(a + lb, ",0") || !strcmp(a + lb, ",-1") || !strcmp(a + lb, " 0")))
            a[lb] = 0;
        checked++;
        char ma[64], mb[64];
        sscanf(a, "%63s", ma);
        sscanf(b, "%63s", mb);
        if (strcmp(ma, mb)) {
            mnBad++;
            if (verbose || mnBad <= 20) printf("MNEMONIC %08x: objdump '%s' mine '%s'\n", inst, a, b);
        } else if (strcmp(a, b)) {
            opBad++;
            if (verbose || opBad <= 20) printf("OPERANDS %08x: objdump '%s' mine '%s'\n", inst, a, b);
        }
    }
    printf("DISASM checked=%u mnemonic_mismatch=%u operand_mismatch=%u\n", checked, mnBad, opBad);
    return (mnBad || opBad) ? 1 : 0;
}
