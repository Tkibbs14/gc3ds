// Minimal PNG writer (RGB8, uncompressed deflate blocks): no dependencies, good enough for frame dumps.
#include <stdio.h>
#include "png.h"

static u32 g_crcTable[256];

static void crc_init(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[n] = c;
    }
}

static u32 crc_update(u32 crc, const u8* p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = g_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void put32(FILE* f, u32 v) { u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v }; fwrite(b, 1, 4, f); }

static void chunk(FILE* f, const char* type, const u8* data, u32 len) {
    put32(f, len);
    u32 crc = crc_update(0xFFFFFFFFu, (const u8*)type, 4);
    fwrite(type, 1, 4, f);
    if (len) { fwrite(data, 1, len, f); crc = crc_update(crc, data, len); }
    put32(f, crc ^ 0xFFFFFFFFu);
}

bool png_write_rgb(const char* path, const u8* rgb, u32 w, u32 h) {
    if (!g_crcTable[1]) crc_init();
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, f);
    u8 ihdr[13] = { (u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h,
                    8, 2, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    // zlib stream: header, stored blocks of up to 65535 bytes, Adler-32.
    size_t raw = (size_t)h * (1 + 3 * (size_t)w);
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    u8* z = malloc(zlen);
    if (!z) { fclose(f); return false; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    u32 a = 1, b = 0;
    size_t done = 0, row = 0, col = 0;  // position within the filtered image (filter byte 0 per row)
    while (done < raw) {
        u32 n = (u32)(raw - done > 65535 ? 65535 : raw - done);
        z[o++] = done + n == raw ? 1 : 0;
        z[o++] = (u8)n; z[o++] = (u8)(n >> 8); z[o++] = (u8)~n; z[o++] = (u8)(~n >> 8);
        for (u32 i = 0; i < n; i++) {
            u8 byte = col == 0 ? 0 : rgb[row * 3 * w + col - 1];
            if (++col == 1 + 3 * w) { col = 0; row++; }
            z[o++] = byte;
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        done += n;
    }
    u32 adler = (b << 16) | a;
    z[o++] = (u8)(adler >> 24); z[o++] = (u8)(adler >> 16); z[o++] = (u8)(adler >> 8); z[o++] = (u8)adler;
    chunk(f, "IDAT", z, (u32)o);
    chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) == 0;
}
