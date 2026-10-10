#pragma once
#include "../sys/system.h"

// Attaches the JIT to the system's CPU; false where no JIT exists for this host (non-ARM) or on allocation failure.
bool host_jit_enable(System* s);
void host_jit_report(void);  // JIT statistics to stderr (no-op without a JIT)
