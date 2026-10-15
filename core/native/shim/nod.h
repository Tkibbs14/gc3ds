/* Native mode (gc3ds): the part of the nod disc library Aurora's DVD layer (lib/dolphin/dvd) uses, implemented by
 * core/native/platform/disc_3ds.cpp for GameCube images (plain GCM / ISO and NKit, as gc3ds reads them): one data
 * partition whose files are the FST's. Function and type names as nod's C API. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NodHandle NodHandle;
typedef enum NodResult { NOD_RESULT_OK = 0, NOD_RESULT_ERROR = 1 } NodResult;
typedef enum NodPartitionKind { NOD_PARTITION_KIND_DATA = 0, NOD_PARTITION_KIND_UPDATE = 1, NOD_PARTITION_KIND_CHANNEL = 2 } NodPartitionKind;
typedef enum NodNodeKind { NOD_NODE_KIND_FILE = 0, NOD_NODE_KIND_DIRECTORY = 1 } NodNodeKind;
typedef struct NodDiscStream {
    void* user_data;
    int64_t (*read_at)(void* user_data, uint64_t offset, void* out, size_t len);
    int64_t (*stream_len)(void* user_data);
    void (*close)(void* user_data);
} NodDiscStream;
typedef struct NodDiscOptions {
    uint32_t preloader_threads;
} NodDiscOptions;
typedef struct NodDiscHeader {
    char game_id[6];
    uint8_t disc_num;
    uint8_t disc_version;
    uint8_t audio_streaming;
    uint8_t audio_stream_buf_size;
} NodDiscHeader;
typedef struct NodData {
    const uint8_t* data;
    uint32_t size;
} NodData;
typedef struct NodPartitionMeta {
    NodData raw_dol;
} NodPartitionMeta;
/* Called for each FST entry from index 1; returns the next index to visit. A directory's size is the index past its
 * last entry. */
typedef uint32_t (*NodFstCallback)(uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user_data);

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out);
NodResult nod_disc_open_partition_kind(NodHandle* disc, NodPartitionKind kind, const void* options, NodHandle** out);
NodResult nod_disc_header(NodHandle* disc, NodDiscHeader* out);
NodResult nod_partition_meta(NodHandle* partition, NodPartitionMeta* out);
void nod_partition_iterate_fst(NodHandle* partition, NodFstCallback callback, void* user_data);
NodResult nod_partition_open_file(NodHandle* partition, uint32_t index, NodHandle** out);
int64_t nod_read(NodHandle* handle, void* out, size_t len);
int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence);
void nod_free(NodHandle* handle);
#ifdef __cplusplus
}
#endif
