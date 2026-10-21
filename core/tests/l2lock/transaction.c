// Synthetic registers only: exercises the same transaction the device probe calls, including every I/O failure.
#include "l2lock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    uint32_t regs[0x940 / 4];
    unsigned op, fail, writes, restoreWrites;
    bool ignore, alias, restoring;
} Mock;
static bool rd(void* ctx, unsigned offset, uint32_t* value) {
    Mock* m = ctx;
    if (++m->op == m->fail) return false;
    *value = m->regs[offset / 4];
    return true;
}
static bool wr(void* ctx, unsigned offset, uint32_t value) {
    Mock* m = ctx;
    assert(offset >= 0x908 && offset <= 0x91c && !(offset & 3));  // no aux/control/unrelated writes
    m->writes++;
    if (m->restoring) m->restoreWrites++;
    // Model a store which reaches hardware but whose completion reports failure.
    if (!m->ignore) m->regs[(m->alias ? 0x900 : offset) / 4] = value;
    return ++m->op != m->fail;
}
static void init(Mock* m, L2Lock* s, L2LockIo* io, bool sixteen) {
    memset(m, 0, sizeof *m);
    m->regs[0] = 0x410000c9;
    m->regs[0x100 / 4] = 1;
    m->regs[0x104 / 4] = sixteen ? 0x32890000 : 0x32880000;
    *io = (L2LockIo){m, rd, wr};
    assert(l2lock_snapshot(s, io));
}
static void clean(Mock* m) {
    for (unsigned i = 0; i < 16; i++) assert(m->regs[(0x900 + i * 4) / 4] == 0);
    assert(m->regs[0x100 / 4] == 1);
    assert((m->regs[0x104 / 4] & ~0x10000u) == 0x32880000);
}
int main(void) {
    Mock m; L2Lock s; L2LockIo io;
    unsigned cases = 0;
    for (unsigned geometry = 0; geometry < 2; geometry++) {
        init(&m, &s, &io, geometry);
        unsigned before = m.op;
        assert(l2lock_apply(&s, &io));
        unsigned applyOps = m.op - before;
        for (unsigned i = 0; i < 16; i++)
            assert(m.regs[(0x900 + i * 4) / 4] == ((i >= 2 && i < 8) ? (geometry ? 0x5555 : 0x55) : 0));
        m.restoring = true; before = m.op;
        assert(l2lock_restore(&s, &io)); clean(&m); cases++;
        unsigned restoreOps = m.op - before;
        // Snapshot failures must not write; all partial-apply failures must still restore all saved words.
        for (unsigned fail = 1; fail <= 19; fail++) {
            init(&m, &s, &io, geometry); m.op = 0; m.fail = fail;
            assert(!l2lock_snapshot(&s, &io)); assert(l2lock_restore(&s, &io)); assert(!m.writes); cases++;
        }
        for (unsigned fail = 1; fail <= applyOps; fail++) {
            init(&m, &s, &io, geometry); m.fail = m.op + fail;
            assert(!l2lock_apply(&s, &io)); m.restoring = true;
            assert(l2lock_restore(&s, &io)); clean(&m); cases++;
        }
        for (unsigned fail = 1; fail <= restoreOps; fail++) {
            init(&m, &s, &io, geometry); assert(l2lock_apply(&s, &io));
            m.restoring = true; m.fail = m.op + fail;
            assert(!l2lock_restore(&s, &io)); assert(m.restoreWrites == 6);
            assert(l2lock_restore(&s, &io)); clean(&m); cases++;
        }
        init(&m, &s, &io, geometry); m.ignore = true;
        assert(!l2lock_apply(&s, &io)); assert(l2lock_restore(&s, &io)); clean(&m); cases++;
        init(&m, &s, &io, geometry); m.alias = true;
        assert(!l2lock_apply(&s, &io));
        // This alias clears when the attempted word is restored; verify every original word nonetheless.
        assert(l2lock_restore(&s, &io)); clean(&m); cases++;
        init(&m, &s, &io, geometry); s.saved[10] = m.regs[0x928 / 4] = 2;
        assert(!l2lock_apply(&s, &io)); assert(!m.writes); cases++;
    }
    printf("X2 transaction: %u synthetic cases passed\n", cases);
}
