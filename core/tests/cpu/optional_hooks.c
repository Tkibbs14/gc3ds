// The legacy CPU test runner links only the interpreter. Its optional memo/AOT/HLE
// hooks need inert implementations; this is test linkage, never emulator code.
#include "../../src/cpu/gekko.h"
#include "../../src/cpu/aot.h"
#include "../../src/cpu/hle.h"
void* g_memoRec;
void memo_access(Gekko* g, u32 ea, u32 size, bool w) { (void)g; (void)ea; (void)size; (void)w; }
bool aot_enabled(void) { return false; }
bool aot_state_ok(const Gekko* g) { (void)g; return false; }
bool aot_is_entry(u32 pc) { (void)pc; return false; }
u32 aot_run(Gekko* g, s32 budget) { (void)g; (void)budget; return 0; }
bool hle_is_hook(Gekko* g, u32 pc) { (void)g; (void)pc; return false; }
u32 hle_run(Gekko* g, u32 budget) { (void)g; (void)budget; return 0; }
