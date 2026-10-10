#pragma once
#include "../sys/system.h"

// Loads a DOL executable into MEM1 and puts the machine in the state the IPL leaves for a game:
// BATs and MSR set up, low-memory globals filled, PC at the entry point. Returns false with a message on error.
bool boot_dol(System* s, const u8* dol, size_t size, char* err, size_t errLen);

// Boots a disc the way the IPL does (high level): OS globals, then the disc's apploader runs on the emulated CPU and
// loads the game; PC is left at the game's entry point. The disc stays attached to the drive (s->disc).
struct Disc;
bool boot_disc(System* s, struct Disc* d, char* err, size_t errLen);
