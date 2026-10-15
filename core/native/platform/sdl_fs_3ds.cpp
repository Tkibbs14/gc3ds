// gc3ds native mode: the SDL file-stream, filesystem and error calls Aurora uses (the disc image in lib/dolphin/dvd,
// file helpers in lib/io.cpp, the memory card in lib/card), over stdio and POSIX on the SD card. Declared by the
// stand-in headers core/native/shim/SDL3/SDL_iostream.h and SDL_filesystem.h.
#include <3ds.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

// ---- errors ----
static char s_error[256];
extern "C" const char* SDL_GetError(void) { return s_error; }
extern "C" bool SDL_SetError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof s_error, fmt, ap);
    va_end(ap);
    return false;
}

// ---- file streams ----
struct SDL_IOStream {
    FILE* f;
    bool error;
};
extern "C" SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
    FILE* f = fopen(file, mode);
    if (!f) {
        SDL_SetError("cannot open %s: %s", file, strerror(errno));
        return nullptr;
    }
    if (!strchr(mode, 'w') && !strchr(mode, 'a') && !strchr(mode, '+'))
        setvbuf(f, nullptr, _IONBF, 0);  // reads: as gc3ds's disc reader, each read is one host read
    return new SDL_IOStream{f, false};
}
extern "C" Sint64 SDL_SeekIO(SDL_IOStream* io, Sint64 offset, SDL_IOWhence whence) {
    if (!io) return -1;
    int w = whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (fseeko(io->f, (off_t)offset, w)) return -1;
    return (Sint64)ftello(io->f);
}
extern "C" size_t SDL_ReadIO(SDL_IOStream* io, void* ptr, size_t size) {
    if (!io) return 0;
    size_t n = fread(ptr, 1, size, io->f);
    if (n < size && ferror(io->f)) io->error = true;
    return n;
}
extern "C" size_t SDL_WriteIO(SDL_IOStream* io, const void* ptr, size_t size) {
    if (!io) return 0;
    size_t n = fwrite(ptr, 1, size, io->f);
    if (n < size) io->error = true;
    return n;
}
extern "C" bool SDL_FlushIO(SDL_IOStream* io) { return io && fflush(io->f) == 0; }
extern "C" SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* io) {
    if (!io || io->error) return SDL_IO_STATUS_ERROR;
    return feof(io->f) ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_READY;
}
extern "C" Sint64 SDL_GetIOSize(SDL_IOStream* io) {
    if (!io) return -1;
    off_t at = ftello(io->f);
    fseeko(io->f, 0, SEEK_END);
    off_t size = ftello(io->f);
    fseeko(io->f, at, SEEK_SET);
    return (Sint64)size;
}
extern "C" bool SDL_CloseIO(SDL_IOStream* io) {
    if (!io) return false;
    bool ok = fclose(io->f) == 0 && !io->error;
    delete io;
    return ok;
}

// ---- paths ----
extern "C" bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (info) *info = SDL_PathInfo{SDL_PATHTYPE_NONE, 0, 0, 0, 0};
        return SDL_SetError("cannot stat %s", path);
    }
    if (info) {
        info->type = S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : S_ISREG(st.st_mode) ? SDL_PATHTYPE_FILE : SDL_PATHTYPE_OTHER;
        info->size = (uint64_t)st.st_size;
        info->create_time = info->modify_time = info->access_time = (SDL_Time)st.st_mtime * 1000000000ll;
    }
    return true;
}
extern "C" bool SDL_CreateDirectory(const char* path) {
    // As SDL: creates missing parents too.
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (strchr(buf, ':') == nullptr || buf[strlen(buf) - 1] != ':') mkdir(buf, 0777);  // not the "sdmc:" root
        *p = '/';
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) return SDL_SetError("cannot create %s", path);
    return true;
}
extern "C" bool SDL_RemovePath(const char* path) {
    if (remove(path) == 0 || rmdir(path) == 0) return true;
    return SDL_SetError("cannot remove %s", path);
}
extern "C" bool SDL_RenamePath(const char* oldpath, const char* newpath) {
    remove(newpath);  // as SDL: replaces the destination
    if (rename(oldpath, newpath) == 0) return true;
    return SDL_SetError("cannot rename %s", oldpath);
}
extern "C" char* SDL_GetPrefPath(const char*, const char*) {
    SDL_CreateDirectory("sdmc:/gc3ds/native");
    return strdup("sdmc:/gc3ds/native/");
}
extern "C" void SDL_free(void* mem) { free(mem); }
extern "C" uint32_t SDL_rand_bits(void) {
    static uint32_t s = 0;
    if (!s) s = (uint32_t)svcGetSystemTick() | 1;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
