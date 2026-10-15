// Native mode (gc3ds): the SDL timer calls Dusklight's frame limiter uses (dusk/time.h), on the 3DS clock
// (core/native/platform). Nanoseconds since start.
#pragma once
#include <stdint.h>
typedef uint64_t Uint64;
#ifdef __cplusplus
extern "C" {
#endif
Uint64 SDL_GetTicksNS(void);
void SDL_DelayPrecise(Uint64 ns);
#ifdef __cplusplus
}
#endif
