#pragma once
#include "../sys/system.h"

// Loads a DOL executable into MEM1 and puts the machine in the state the IPL leaves for a game:
// BATs and MSR set up, low-memory globals filled, PC at the entry point. Returns false with a message on error.
bool boot_dol(System* s, const u8* dol, size_t size, char* err, size_t errLen);
