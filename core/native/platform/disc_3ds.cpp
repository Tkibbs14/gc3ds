// gc3ds native mode: the disc for Aurora's DVD layer (lib/dolphin/dvd, compiled unchanged). Aurora opens the image
// with SDL's file stream (sdl_fs_3ds.cpp) and reads it through nod's C API, implemented here for GameCube images,
// plain GCM / ISO and NKit, the way gc3ds's own reader (core/src/disc) treats them: the boot header gives the FST, the
// FST gives every file, and NKit's packed images need nothing more since their header and FST point at the packed
// offsets. Reads past the end of the image return zeros (NKit drops the junk padding the game never reads).
#include <nod.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>

// ---- nod over a GameCube image ----
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

enum class Kind { Disc, Partition, File };
struct NodHandle {
    Kind kind;
    NodHandle* disc = nullptr;  // partition, file: the disc
    // disc
    NodDiscStream stream{};
    uint64_t length = 0;
    uint8_t header[0x440]{};
    std::vector<uint8_t> fst;
    uint32_t fstEntries = 0;
    std::vector<uint8_t> dol;
    // file
    uint64_t start = 0, size = 0, pos = 0;
};

// Reads at an image offset; bytes past the end of the image are zero.
static bool read_at(NodHandle* disc, uint64_t offset, void* out, size_t len) {
    std::memset(out, 0, len);
    if (offset >= disc->length) return true;
    size_t want = (size_t)std::min<uint64_t>(len, disc->length - offset);
    int64_t got = disc->stream.read_at(disc->stream.user_data, offset, out, want);
    return got >= 0;
}

extern "C" NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions*, NodHandle** out) {
    *out = nullptr;
    auto* d = new NodHandle{Kind::Disc};
    d->stream = *stream;
    int64_t len = stream->stream_len(stream->user_data);
    d->length = len > 0 ? (uint64_t)len : 0;
    if (!read_at(d, 0, d->header, sizeof d->header) || be32(d->header + 0x1C) != 0xC2339F3Du) {  // GameCube magic
        nod_free(d);
        return NOD_RESULT_ERROR;
    }
    uint32_t fstOff = be32(d->header + 0x424), fstSize = be32(d->header + 0x428);
    if (fstSize < 12 || fstSize > 16u << 20) {
        nod_free(d);
        return NOD_RESULT_ERROR;
    }
    d->fst.resize(fstSize);
    if (!read_at(d, fstOff, d->fst.data(), fstSize)) {
        nod_free(d);
        return NOD_RESULT_ERROR;
    }
    d->fstEntries = be32(d->fst.data() + 8);
    if (d->fstEntries == 0 || (uint64_t)d->fstEntries * 12 > fstSize) {
        nod_free(d);
        return NOD_RESULT_ERROR;
    }
    *out = d;
    return NOD_RESULT_OK;
}

extern "C" NodResult nod_disc_open_partition_kind(NodHandle* disc, NodPartitionKind kind, const void*, NodHandle** out) {
    *out = nullptr;
    if (!disc || disc->kind != Kind::Disc || kind != NOD_PARTITION_KIND_DATA) return NOD_RESULT_ERROR;  // GameCube: one
    *out = new NodHandle{Kind::Partition, disc};
    return NOD_RESULT_OK;
}

extern "C" NodResult nod_disc_header(NodHandle* disc, NodDiscHeader* out) {
    if (!disc || disc->kind != Kind::Disc) return NOD_RESULT_ERROR;
    std::memcpy(out->game_id, disc->header, 6);
    out->disc_num = disc->header[6];
    out->disc_version = disc->header[7];
    out->audio_streaming = disc->header[8];
    out->audio_stream_buf_size = disc->header[9];
    return NOD_RESULT_OK;
}

// The DOL as stored: its size is the end of its furthest section (7 text and 11 data sections; offsets at 0x00,
// sizes at 0x90).
extern "C" NodResult nod_partition_meta(NodHandle* part, NodPartitionMeta* out) {
    if (!part || part->kind != Kind::Partition) return NOD_RESULT_ERROR;
    NodHandle* d = part->disc;
    if (d->dol.empty()) {
        uint32_t dolOff = be32(d->header + 0x420);
        uint8_t hdr[0x100];
        if (!read_at(d, dolOff, hdr, sizeof hdr)) return NOD_RESULT_ERROR;
        uint32_t end = 0x100;
        for (int i = 0; i < 18; i++) {
            uint32_t off = be32(hdr + 4 * i), size = be32(hdr + 0x90 + 4 * i);
            if (off && size && off + size > end) end = off + size;
        }
        if (end > 16u << 20) return NOD_RESULT_ERROR;
        d->dol.resize(end);
        if (!read_at(d, dolOff, d->dol.data(), end)) return NOD_RESULT_ERROR;
    }
    out->raw_dol.data = d->dol.data();
    out->raw_dol.size = (uint32_t)d->dol.size();
    return NOD_RESULT_OK;
}

// FST entry i: byte 0 is 1 for a directory, bytes 1-3 the name's offset in the string table after the entries; then
// the file's offset (a directory's parent) and the file's size (a directory's end index).
extern "C" void nod_partition_iterate_fst(NodHandle* part, NodFstCallback callback, void* user) {
    if (!part || part->kind != Kind::Partition) return;
    NodHandle* d = part->disc;
    const uint8_t* fst = d->fst.data();
    const uint32_t strBase = d->fstEntries * 12;
    for (uint32_t i = 1; i < d->fstEntries;) {
        const uint8_t* e = fst + 12 * i;
        uint32_t nameOff = strBase + (be32(e) & 0xFFFFFF);
        const char* name = nameOff < d->fst.size() ? (const char*)fst + nameOff : "";
        NodNodeKind kind = e[0] ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
        uint32_t next = callback(i, kind, name, be32(e + 8), user);
        i = next > i ? next : i + 1;
    }
}

extern "C" NodResult nod_partition_open_file(NodHandle* part, uint32_t index, NodHandle** out) {
    *out = nullptr;
    if (!part || part->kind != Kind::Partition) return NOD_RESULT_ERROR;
    NodHandle* d = part->disc;
    if (index == 0 || index >= d->fstEntries) return NOD_RESULT_ERROR;
    const uint8_t* e = d->fst.data() + 12 * index;
    if (e[0]) return NOD_RESULT_ERROR;  // a directory
    auto* f = new NodHandle{Kind::File, d};
    f->start = be32(e + 4);
    f->size = be32(e + 8);
    *out = f;
    return NOD_RESULT_OK;
}

extern "C" int64_t nod_read(NodHandle* h, void* out, size_t len) {
    if (!h) return -1;
    if (h->kind == Kind::Disc) return -1;
    if (h->kind == Kind::Partition) return -1;
    if (h->pos >= h->size) return 0;
    size_t n = (size_t)std::min<uint64_t>(len, h->size - h->pos);
    if (!read_at(h->disc, h->start + h->pos, out, n)) return -1;
    h->pos += n;
    return (int64_t)n;
}

extern "C" int64_t nod_seek(NodHandle* h, int64_t offset, int32_t whence) {
    if (!h || h->kind != Kind::File) return -1;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)h->pos : (int64_t)h->size;
    int64_t at = base + offset;
    if (at < 0) return -1;
    h->pos = (uint64_t)at;
    return at;
}

extern "C" void nod_free(NodHandle* h) {
    if (!h) return;
    if (h->kind == Kind::Disc && h->stream.close) h->stream.close(h->stream.user_data);
    delete h;
}
