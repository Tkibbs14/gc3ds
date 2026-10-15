// Native mode (gc3ds): only the event type is named (dusk/mouse.h, the UI's handle_event); no SDL events on the 3DS.
#pragma once
#include <stdint.h>
typedef uint32_t Uint32;
typedef uint64_t SDL_FingerID;
typedef union SDL_Event {
    uint32_t type;
    uint8_t padding[128];
} SDL_Event;
