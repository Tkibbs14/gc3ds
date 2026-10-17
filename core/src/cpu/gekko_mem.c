// Address translation (Gekko: 4 IBAT + 4 DBAT, no page-table walk) and the slow paths of data access.
//
// BAT format (PowerPC Programming Environments Manual, 7.4.2 "BAT Register Implementation"):
//   upper: BEPI[0:14] | BL[19:29] | Vs[30] | Vp[31]
//   lower: BRPN[0:14] | WIMG[25:28] | PP[30:31]
// An EA matches when EA[0:3] == BEPI[0:3] and EA[4:14] & ~BL == BEPI[4:14] & ~BL; the physical address is
// BRPN[0:3] || (BRPN[4:14] | (EA[4:14] & BL)) || EA[15:31]. Vs/Vp enable the BAT in supervisor/user state. PP: 00 no
// access, x1 read-only, 10 read/write. If two BATs match, the architecture leaves the result undefined; here the
// lowest-numbered BAT wins.
#include "gekko_internal.h"

#define TR_READ 8u

static void build_real(GkMap* m) {
    for (u32 b = 0; b < GK_NBLK; b++) m->trans[b] = (b << GK_BLK_SHIFT) | TR_VALID | TR_READ | TR_WRITE;
    memset(m->rhost, 0, sizeof m->rhost);
    memset(m->whost, 0, sizeof m->whost);
}

static void build_bat(Gekko* g, GkMap* m, u32 sprBase, bool data) {
    u32 pr = (g->msr & MSR_PR) ? 1 : 0;
    memset(m->trans, 0, sizeof m->trans);
    memset(m->rhost, 0, sizeof m->rhost);
    memset(m->whost, 0, sizeof m->whost);
    for (int i = 3; i >= 0; i--) {
        u32 up = g->spr[sprBase + 2 * (u32)i], lo = g->spr[sprBase + 2 * (u32)i + 1];
        u32 valid = pr ? (up & 1) : ((up >> 1) & 1);
        if (!valid) continue;
        u32 bepi = up >> 17, bl = (up >> 2) & 0x7FF, brpn = lo >> 17, pp = lo & 3;
        u32 flags = TR_VALID;
        if (pp != 0) flags |= TR_READ;
        if (pp == 2 && data) flags |= TR_WRITE;
        u32 base = bepi & ~bl & 0x7FFF;
        if (((bl + 1) & bl) == 0) {
            // Well-formed BL (right-justified ones): the matching blocks are one contiguous run.
            for (u32 x = 0; x <= bl; x++) {
                u32 b = base | x;
                m->trans[b] = ((((brpn & ~bl) | x) & 0x7FFF) << GK_BLK_SHIFT) | flags;
            }
        } else {
            for (u32 b = 0; b < GK_NBLK; b++)
                if ((b & ~bl) == base) m->trans[b] = ((((brpn & ~bl) | (b & bl)) & 0x7FFF) << GK_BLK_SHIFT) | flags;
        }
    }
}

void gk_map_invalidate(Gekko* g, bool ibat, bool dbat) {
    GkPriv* p = GKP(g);
    if (ibat) build_bat(g, &p->ibatMap, SPR_IBAT0U, false);
    if (dbat) build_bat(g, &p->dbatMap, SPR_DBAT0U, true);
    p->batPR = g->msr & MSR_PR;
    p->fetchBlk = 0xFFFFFFFFu;
}

void gk_update_maps(Gekko* g) {
    GkPriv* p = GKP(g);
    if ((g->msr & MSR_PR) != p->batPR) gk_map_invalidate(g, true, true);
    p->imap = (g->msr & MSR_IR) ? &p->ibatMap : &p->realMap;
    p->dmap = (g->msr & MSR_DR) ? &p->dbatMap : &p->realMap;
    p->fetchBlk = 0xFFFFFFFFu;
}

void gk_map_init(Gekko* g);
void gk_map_init(Gekko* g) {
    GkPriv* p = GKP(g);
    build_real(&p->realMap);
    gk_map_invalidate(g, true, true);
    gk_update_maps(g);
}

static void query_host(Gekko* g, GkMap* m, u32 blk) {
    u32 t = m->trans[blk];
    u8* h = NULL;
    if (g->bus->host_ptr) h = g->bus->host_ptr(g->bus->ctx, t & ~GK_BLK_MASK, GK_BLK_SIZE);
    m->rhost[blk] = (t & TR_READ) ? h : NULL;
    m->whost[blk] = (t & TR_WRITE) ? h : NULL;
    m->trans[blk] = t | TR_HOSTQ;
}

// Host pointer for a translated block on the instruction side (fetch), querying the system once per block.
const u8* gk_fetch_host(Gekko* g, u32 blk);
const u8* gk_fetch_host(Gekko* g, u32 blk) {
    GkMap* m = GKP(g)->imap;
    if (!(m->trans[blk] & TR_HOSTQ)) query_host(g, m, blk);
    return m->rhost[blk];
}

static void raise_dsi(Gekko* g, u32 ea, bool write, bool prot) {
    g->spr[SPR_DAR] = ea;
    g->spr[SPR_DSISR] = (prot ? 0x08000000u : 0x40000000u) | (write ? 0x02000000u : 0);
    emu_log(LOG_CPU, "DSI: %s ea=%08x pc=%08x msr=%08x (%s)\n", write ? "store" : "load", ea, GKP(g)->curPc, g->msr,
            prot ? "protection" : "no BAT match");
    gk_exception(g, EXC_DSI, GKP(g)->curPc, 0);
}

bool gk_translate(Gekko* g, u32 ea, bool write, bool data, u32* pa, bool raise) {
    GkPriv* p = GKP(g);
    GkMap* m = data ? p->dmap : p->imap;
    u32 t = m->trans[ea >> GK_BLK_SHIFT];
    bool ok = (t & TR_VALID) && (t & TR_READ) && (!write || (t & TR_WRITE));
    if (!ok) {
        if (raise) {
            if (data) {
                raise_dsi(g, ea, write, (t & TR_VALID) != 0);
            } else {
                emu_log(LOG_CPU, "ISI: ea=%08x msr=%08x\n", ea, g->msr);
                gk_exception(g, EXC_ISI, ea, (t & TR_VALID) ? 0x08000000u : 0x40000000u);
            }
        }
        return false;
    }
    *pa = (t & ~GK_BLK_MASK) | (ea & GK_BLK_MASK);
    return true;
}

static bool read_bytes(Gekko* g, u32 ea, u32 size, u64* out) {
    u64 v = 0;
    for (u32 i = 0; i < size; i++) {
        u32 pa;
        if (!gk_translate(g, ea + i, false, true, &pa, true)) return false;
        GkMap* m = GKP(g)->dmap;
        u32 blk = (ea + i) >> GK_BLK_SHIFT;
        if (!(m->trans[blk] & TR_HOSTQ)) query_host(g, m, blk);
        u8 b = m->rhost[blk] ? (u8)ram_rd8(m->rhost[blk], (ea + i) & GK_BLK_MASK) : g->bus->read8(g->bus->ctx, pa);
        v = (v << 8) | b;
    }
    *out = v;
    return true;
}

bool gk_read_slow(Gekko* g, u32 ea, u32 size, u64* out) {
    u32 o = ea & GK_BLK_MASK;
    if (o + size > GK_BLK_SIZE) {
        // Straddles two translation blocks: check both ends first so a fault leaves nothing half done.
        u32 pa;
        if (!gk_translate(g, ea, false, true, &pa, true)) return false;
        if (!gk_translate(g, ea + size - 1, false, true, &pa, true)) return false;
        return read_bytes(g, ea, size, out);
    }
    u32 pa;
    if (!gk_translate(g, ea, false, true, &pa, true)) return false;
    GkMap* m = GKP(g)->dmap;
    u32 blk = ea >> GK_BLK_SHIFT;
    if (!(m->trans[blk] & TR_HOSTQ)) query_host(g, m, blk);
    const u8* h = m->rhost[blk];
    if (h) {
        switch (size) {
        case 1: *out = ram_rd8(h, o); break;
        case 2: *out = ram_rd16(h, o); break;
        case 4: *out = ram_rd32(h, o); break;
        default: *out = ram_rd64(h, o); break;
        }
        return true;
    }
    const GekkoBus* bus = g->bus;
    if ((pa & (size - 1)) != 0 && size > 1) return read_bytes(g, ea, size, out);
    switch (size) {
    case 1: *out = bus->read8(bus->ctx, pa); break;
    case 2: *out = bus->read16(bus->ctx, pa); break;
    case 4: *out = bus->read32(bus->ctx, pa); break;
    default: *out = bus->read64(bus->ctx, pa); break;
    }
    return true;
}

static bool write_bytes(Gekko* g, u32 ea, u32 size, u64 v) {
    for (u32 i = 0; i < size; i++) {
        u32 pa;
        if (!gk_translate(g, ea + i, true, true, &pa, true)) return false;
        u8 b = (u8)(v >> (8 * (size - 1 - i)));
        GkMap* m = GKP(g)->dmap;
        u32 blk = (ea + i) >> GK_BLK_SHIFT;
        if (!(m->trans[blk] & TR_HOSTQ)) query_host(g, m, blk);
        if (m->whost[blk]) ram_wr8(m->whost[blk], (ea + i) & GK_BLK_MASK, b);
        else g->bus->write8(g->bus->ctx, pa, b);
    }
    return true;
}

bool gk_write_slow(Gekko* g, u32 ea, u32 size, u64 v) {
    u32 o = ea & GK_BLK_MASK;
    u32 pa;
    if (o + size > GK_BLK_SIZE) {
        if (!gk_translate(g, ea, true, true, &pa, true)) return false;
        if (!gk_translate(g, ea + size - 1, true, true, &pa, true)) return false;
        return write_bytes(g, ea, size, v);
    }
    if (!gk_translate(g, ea, true, true, &pa, true)) return false;
    GkMap* m = GKP(g)->dmap;
    u32 blk = ea >> GK_BLK_SHIFT;
    if (!(m->trans[blk] & TR_HOSTQ)) query_host(g, m, blk);
    u8* h = m->whost[blk];
    if (h) {
        switch (size) {
        case 1: ram_wr8(h, o, (u32)v); break;
        case 2: ram_wr16(h, o, (u32)v); break;
        case 4: ram_wr32(h, o, (u32)v); break;
        default: ram_wr64(h, o, v); break;
        }
        return true;
    }
    const GekkoBus* bus = g->bus;
    if ((pa & (size - 1)) != 0 && size > 1) return write_bytes(g, ea, size, v);
    switch (size) {
    case 1: bus->write8(bus->ctx, pa, (u8)v); break;
    case 2: bus->write16(bus->ctx, pa, (u16)v); break;
    case 4: bus->write32(bus->ctx, pa, (u32)v); break;
    default: bus->write64(bus->ctx, pa, v); break;
    }
    return true;
}
