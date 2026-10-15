// Native mode (gc3ds): the window types m_Do_graphic.cpp's safe-area code names. There is no SDL window on the 3DS
// (aurora::window::get_sdl_window returns null there), so the function is never reached.
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
#ifdef __cplusplus
extern "C" {
#endif
bool SDL_GetWindowSafeArea(SDL_Window* window, SDL_Rect* rect);
#ifdef __cplusplus
}
#endif
