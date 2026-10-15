/* Native mode (gc3ds): the SDL filesystem, error and stdinc calls Aurora's file helpers (lib/io.cpp) and memory-card
 * code (lib/card, lib/dolphin/card.cpp) use, over stdio / POSIX on the SD card (core/native/platform/sdl_fs_3ds.cpp).
 * The preferences path is sdmc:/gc3ds/native/. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL_iostream.h>
typedef int64_t SDL_Time;
typedef enum SDL_PathType { SDL_PATHTYPE_NONE, SDL_PATHTYPE_FILE, SDL_PATHTYPE_DIRECTORY, SDL_PATHTYPE_OTHER } SDL_PathType;
typedef struct SDL_PathInfo {
    SDL_PathType type;
    uint64_t size;
    SDL_Time create_time;
    SDL_Time modify_time;
    SDL_Time access_time;
} SDL_PathInfo;
#ifdef __cplusplus
extern "C" {
#endif
bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info);
bool SDL_CreateDirectory(const char* path);
bool SDL_RemovePath(const char* path);
bool SDL_RenamePath(const char* oldpath, const char* newpath);
char* SDL_GetPrefPath(const char* org, const char* app);
void SDL_free(void* mem);
const char* SDL_GetError(void);
bool SDL_SetError(const char* fmt, ...);
uint32_t SDL_rand_bits(void);
#ifdef __cplusplus
}
#endif
#define SDL_InvalidParamError(param) SDL_SetError("Parameter '%s' is invalid", (param))
