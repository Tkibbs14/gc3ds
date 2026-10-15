/* Native mode (gc3ds): the SDL file stream Aurora's DVD layer opens the disc image with, over stdio
 * (core/native/platform/disc_3ds.cpp). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int64_t Sint64;
typedef struct SDL_IOStream SDL_IOStream;
typedef enum SDL_IOWhence { SDL_IO_SEEK_SET = 0, SDL_IO_SEEK_CUR = 1, SDL_IO_SEEK_END = 2 } SDL_IOWhence;
#ifdef __cplusplus
extern "C" {
#endif
SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode);
Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence);
size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size);
Sint64 SDL_GetIOSize(SDL_IOStream* context);
bool SDL_CloseIO(SDL_IOStream* context);
#ifdef __cplusplus
}
#endif
/* Writing and stream status (Aurora's lib/io.cpp). */
typedef enum SDL_IOStatus { SDL_IO_STATUS_READY, SDL_IO_STATUS_ERROR, SDL_IO_STATUS_EOF, SDL_IO_STATUS_NOT_READY,
                            SDL_IO_STATUS_READONLY, SDL_IO_STATUS_WRITEONLY } SDL_IOStatus;
#ifdef __cplusplus
extern "C" {
#endif
size_t SDL_WriteIO(SDL_IOStream* context, const void* ptr, size_t size);
bool SDL_FlushIO(SDL_IOStream* context);
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* context);
#ifdef __cplusplus
}
#endif
