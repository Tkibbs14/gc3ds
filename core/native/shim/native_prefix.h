// Native mode (gc3ds): included first in every C++ game file.
#pragma once
// newlib's <ctype.h> defines one-letter class macros (_U _L _N _S _P _C _X _B) that collide with the game's field
// names (d_event_lib.h: "u16 _C;"). In C++ <cctype> provides the classification functions, so the macros can go.
#include <cctype>
#include <span>  // dusk/mods/svc/audio_res/wsys.hpp uses std::span without including it
#include <locale>  // libstdc++'s ctype_base.h uses the macros: parsed here, before they go
#undef _U
#undef _L
#undef _N
#undef _S
#undef _P
#undef _C
#undef _X
#undef _B
// Dusklight's game target uses include/dusk_pch.hpp as its precompiled header, so every game file sees it first
// (several rely on that for headers they never include). global.h comes with it; it needs the SDK types first.
#include "dolphin/types.h"
#include "global.h"
#include "dusk_pch.hpp"
