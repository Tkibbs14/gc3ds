// ARM DDI 0246H r3p3, 2.3.6 and 3.3.11 tables 3-20..35: one means NO allocation.
// The master selector is AyUSER[7:5], not a CPU ID. X2 tests the core=selector hypothesis explicitly.
#include "l2lock.h"
#include <string.h>

bool l2lock_snapshot(L2Lock* s, const L2LockIo* io) {
    memset(s, 0, sizeof *s);
    static const unsigned head[] = {0, 0x100, L2LOCK_AUX};
    uint32_t* values[] = {&s->id, &s->control, &s->aux};
    for (unsigned i = 0; i < 3; i++) {
        s->badOffset = head[i];
        if (!io->read(io->ctx, head[i], values[i])) return false;
    }
    for (unsigned i = 0; i < L2LOCK_WORDS; i++) {
        s->badOffset = L2LOCK_FIRST + i * 4;
        if (!io->read(io->ctx, s->badOffset, &s->saved[i])) return false;
    }
    memcpy(s->wanted, s->saved, sizeof s->wanted);
    s->snapshot = true;
    return true;
}

bool l2lock_apply(L2Lock* s, const L2LockIo* io) {
    if (!s->snapshot || s->attempted || s->id != 0x410000c9u || !(s->control & 1)) return false;
    // Do not combine an experiment with an existing system partition. Never write auxiliary control (cache enabled).
    for (unsigned i = 0; i < L2LOCK_WORDS; i++) if (s->saved[i]) return false;
    uint32_t half = (s->aux & (1u << 16)) ? 0x5555u : 0x55u;  // interleaved half, 16/8-way geometry
    for (unsigned i = 2; i < 8; i++) s->wanted[i] = half;  // D/I pairs 1, 2, 3; pair 0 stays unrestricted
    for (unsigned i = 2; i < 8; i++) {
        s->badOffset = L2LOCK_FIRST + i * 4;
        s->attempted |= 1u << i;  // mark BEFORE the store: a failed store may already have changed hardware
        if (!io->write(io->ctx, s->badOffset, s->wanted[i])) return false;
        uint32_t value;
        if (!io->read(io->ctx, s->badOffset, &value) || value != s->wanted[i]) return false;
    }
    // Detect aliases/ignored registers as well as the six direct readbacks.
    for (unsigned i = 0; i < L2LOCK_WORDS; i++) {
        uint32_t value;
        s->badOffset = L2LOCK_FIRST + i * 4;
        if (!io->read(io->ctx, s->badOffset, &value) || value != s->wanted[i]) return false;
    }
    return true;
}

bool l2lock_restore(L2Lock* s, const L2LockIo* io) {
    if (!s->snapshot) return s->attempted == 0;
    bool ok = true;
    for (unsigned i = L2LOCK_WORDS; i-- > 0;) {
        if ((s->attempted & (1u << i)) && !io->write(io->ctx, L2LOCK_FIRST + i * 4, s->saved[i])) ok = false;
    }
    // Check EVERY saved register, including the untouched auxiliary/control words; never stop cleanup at an error.
    for (unsigned i = 0; i < L2LOCK_WORDS; i++) {
        uint32_t value;
        if (!io->read(io->ctx, L2LOCK_FIRST + i * 4, &value) || value != s->saved[i]) ok = false;
    }
    uint32_t value;
    if (!io->read(io->ctx, L2LOCK_AUX, &value) || value != s->aux) ok = false;
    if (!io->read(io->ctx, 0x100, &value) || value != s->control) ok = false;
    if (ok) s->attempted = 0;
    return ok;
}
