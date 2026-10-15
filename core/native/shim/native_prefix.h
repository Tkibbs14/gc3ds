// Native mode (gc3ds): included first in every C++ game file.
#pragma once
// newlib's <ctype.h> defines one-letter class macros (_U _L _N _S _P _C _X _B) that collide with the game's field
// names (d_event_lib.h: "u16 _C;"). In C++ <cctype> provides the classification functions, so the macros can go.
#include <cctype>
#undef _U
#undef _L
#undef _N
#undef _S
#undef _P
#undef _C
#undef _X
#undef _B
// Dusklight's build force-includes global.h (DUSK_GAME_DATA, DUSK_GAME_EXTERN, ...); it needs the SDK types first.
#include "dolphin/types.h"
#include "global.h"
