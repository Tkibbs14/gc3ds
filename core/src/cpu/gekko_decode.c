// Table-driven decoder: the primary opcode (bits 0-5) indexes gk_prim; the groups 4, 19, 31, 59 and 63 dispatch again on
// their extended opcode. A-form groups use a 5-bit XO (bits 26-30) and the paired-single indexed loads/stores a 6-bit XO
// (bits 25-30); their entries are replicated over every 10-bit index whose low bits match, so one lookup on bits 21-30
// serves all forms. Unassigned entries raise an illegal-instruction program exception.
#include "gekko_internal.h"

GkOp gk_prim[64];
GkOp gk_op4[1024], gk_op19[1024], gk_op31[1024], gk_op59[32], gk_op63[1024];

static void op_illegal(Gekko* g, u32 inst) { gk_illegal(g, inst); }
static void op_4(Gekko* g, u32 inst) { gk_op4[(inst >> 1) & 0x3FF](g, inst); }
static void op_19(Gekko* g, u32 inst) { gk_op19[(inst >> 1) & 0x3FF](g, inst); }
static void op_31(Gekko* g, u32 inst) { gk_op31[(inst >> 1) & 0x3FF](g, inst); }
static void op_59(Gekko* g, u32 inst) { gk_op59[(inst >> 1) & 0x1F](g, inst); }
static void op_63(Gekko* g, u32 inst) { gk_op63[(inst >> 1) & 0x3FF](g, inst); }

void gk_decode_init(void) {
    static bool done;
    if (done) return;
    for (int i = 0; i < 64; i++) gk_prim[i] = op_illegal;
    for (int i = 0; i < 1024; i++) gk_op4[i] = gk_op19[i] = gk_op31[i] = gk_op63[i] = op_illegal;
    for (int i = 0; i < 32; i++) gk_op59[i] = op_illegal;
    gk_prim[4] = op_4;
    gk_prim[19] = op_19;
    gk_prim[31] = op_31;
    gk_prim[59] = op_59;
    gk_prim[63] = op_63;
    gk_register_int();
    gk_register_fpu();
    done = true;
}
