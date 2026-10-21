// X2 probe transaction, also compiled by the host fault-injection test. No hardware access without callbacks.
#ifndef GC3DS_L2LOCK_H
#define GC3DS_L2LOCK_H
#include <stdbool.h>
#include <stdint.h>
#define L2LOCK_BASE 0x97E10000u
#define L2LOCK_AUX 0x104u
#define L2LOCK_FIRST 0x900u
#define L2LOCK_WORDS 16

typedef struct {
    void* ctx;
    bool (*read)(void* ctx, unsigned offset, uint32_t* value);
    bool (*write)(void* ctx, unsigned offset, uint32_t value);
} L2LockIo;
typedef struct {
    uint32_t id, control, aux, saved[L2LOCK_WORDS], wanted[L2LOCK_WORDS];
    unsigned attempted, badOffset;
    bool snapshot;
} L2Lock;
bool l2lock_snapshot(L2Lock* s, const L2LockIo* io);
bool l2lock_apply(L2Lock* s, const L2LockIo* io);
bool l2lock_restore(L2Lock* s, const L2LockIo* io);
#endif
