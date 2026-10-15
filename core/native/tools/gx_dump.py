#!/usr/bin/env python3
"""Parses a native-mode GX stream dump (gx_3ds.c writes frames 120-121 to sdmc:/gc3ds/native/gx_dump.bin): the GameCube
FIFO commands plus Aurora's extension (GX_AURORA 0x50), with the lengths core/src/gx/fifo.c uses. Prints a command
histogram and the first commands, so a decoder that stops drawing can be checked against what Aurora sent.
  python3 gx_dump.py gx_dump.bin [--list N]"""
import struct
import sys
from collections import Counter

AURORA_LEN = {0x0001: 24, 0x0002: 16, 0x0003: 64, 0x0021: 0, 0x0030: 34, 0x0031: 23, 0x0032: 4, 0x0033: 4,
              0x0034: 8, 0x0035: 16, 0x0036: 13, 0x0037: 8, 0x0038: 0, 0x0039: 8, 0x003A: 0, 0x0060: 12, 0x1000: 20}
AURORA_NAME = {0x0001: "VIEWPORT_RENDER", 0x0002: "SCISSOR_RENDER", 0x0003: "PROJECTION_FULL", 0x0020: "DEBUG_PUSH",
               0x0021: "DEBUG_POP", 0x0022: "DEBUG_MARKER", 0x0030: "LOAD_TEXOBJ", 0x0031: "LOAD_TLUT",
               0x0032: "DESTROY_TEXOBJ", 0x0033: "DESTROY_TLUT", 0x0034: "DESTROY_COPY_TEX", 0x0035: "COPY_SRC",
               0x0036: "COPY_DST", 0x0037: "COPY_DEST", 0x0038: "DEPTH_SNAPSHOT", 0x0039: "BEGIN_OFFSCREEN",
               0x003A: "END_OFFSCREEN", 0x0040: "DRAW_SIZED", 0x0041: "DRAW_INDEXED", 0x0060: "CALL_DL_REF",
               0x1000: "POLYGON_OFFSET"}


class GX:
    def __init__(self):
        self.cp = [0] * 256

    def vsize(self, vat):
        lo, hi = self.cp[0x50], self.cp[0x60]
        a, b, c = self.cp[0x70 + vat], self.cp[0x80 + vat], self.cp[0x90 + vat]
        comp = lambda f: 1 if f <= 1 else 2 if f <= 3 else 4
        color = lambda f: [2, 3, 4, 2, 3, 4, 4, 4][f & 7]
        size = lo & 1
        for i in range(8):
            size += (lo >> (1 + i)) & 1
        pos, nrm, c0, c1 = (lo >> 9) & 3, (lo >> 11) & 3, (lo >> 13) & 3, (lo >> 15) & 3
        if pos == 1:
            size += (3 if a & 1 else 2) * comp((a >> 1) & 7)
        elif pos:
            size += pos - 1
        if nrm:
            nbt, index3 = (a >> 9) & 1, (a >> 31) & 1
            if nrm == 1:
                size += (9 if nbt else 3) * comp((a >> 10) & 7)
            else:
                size += (nrm - 1) * (3 if nbt and index3 else 1)
        for v, sh in ((c0, 14), (c1, 18)):
            if v == 1:
                size += color((a >> sh) & 7)
            elif v:
                size += v - 1
        tcnt = [(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1]
        tfmt = [(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7]
        for i in range(8):
            t = (hi >> (2 * i)) & 3
            if t == 1:
                size += (2 if tcnt[i] else 1) * comp(tfmt[i])
            elif t:
                size += t - 1
        return size


def parse(data, gx, hist, out, limit, depth=0):
    p = 0
    while p < len(data):
        op = data[p]
        start = p
        if 0x80 <= op < 0xC0:
            count = struct.unpack_from(">H", data, p + 1)[0]
            vs = gx.vsize(op & 7)
            verts = data[p + 3:p + 3 + count * vs]
            p += 3 + count * vs
            name = f"DRAW prim {(op >> 3) & 7} vat {op & 7} count {count} vsize {vs}"
            if "--values" in sys.argv:
                name += " " + " | ".join(verts[i * vs:(i + 1) * vs].hex() for i in range(min(count, 8)))
            hist["draw"] += 1
        elif op in (0x00, 0x48, 0x44):
            p += 1
            name = f"op {op:02x}"
            hist[name] += 1
        elif op == 0x08:
            r, v = data[p + 1], struct.unpack_from(">I", data, p + 2)[0]
            gx.cp[r] = v
            p += 6
            name = f"CP {r:02x} = {v:08x}"
            hist["cp"] += 1
        elif op == 0x10:
            h = struct.unpack_from(">I", data, p + 1)[0]
            n = (h >> 16) + 1
            vals = struct.unpack_from(f">{n}f", data, p + 5)
            p += 5 + 4 * n
            name = f"XF {h & 0xFFFF:04x} x{n}" + (" " + " ".join(f"{v:.4g}" for v in vals) if "--values" in sys.argv else "")
            hist["xf"] += 1
        elif op in (0x20, 0x28, 0x30, 0x38):
            p += 5
            name = f"LOAD_INDX {op:02x}"
            hist["indx"] += 1
        elif op == 0x40:
            p += 9
            name = "CALL_DL"
            hist["call_dl"] += 1
        elif op == 0x61:
            v = struct.unpack_from(">I", data, p + 1)[0]
            p += 5
            name = f"BP {v >> 24:02x} = {v & 0xFFFFFF:06x}"
            hist["bp"] += 1
        elif op == 0x50:
            sub = struct.unpack_from(">H", data, p + 1)[0]
            a = p + 3
            if 0x10 <= sub <= 0x1F:
                ptr, size, le = struct.unpack_from(">QIB", data, a)
                n = 13
                name = f"ARRAYBASE {sub & 15} {ptr:08x} size {size} le {le}"
                gx.cp[0xA0 + (sub & 15)] = ptr & 0xFFFFFFFF
            elif sub in (0x20, 0x22):
                n = 2 + struct.unpack_from(">H", data, a)[0]
                name = AURORA_NAME[sub] + " " + data[a + 2:a + n].decode("utf-8", "replace")
            elif sub == 0x40:
                cmd, nb = struct.unpack_from(">BI", data, a)
                n = 5 + nb
                name = f"DRAW_SIZED op {cmd:02x} bytes {nb} vsize {gx.vsize(cmd & 7)}"
            elif sub == 0x41:
                cmd, nv, ni = struct.unpack_from(">BHI", data, a)
                n = 7 + ni * 2 + nv * gx.vsize(cmd & 7)
                name = f"DRAW_INDEXED op {cmd:02x} verts {nv} indices {ni}"
            elif sub in AURORA_LEN:
                n = AURORA_LEN[sub]
                name = AURORA_NAME[sub]
                if sub == 0x30:
                    m, ptr, w, h, fmt = struct.unpack_from(">BQIII", data, a)
                    name += f" map {m} {ptr:08x} {w}x{h} fmt {fmt}"
                if sub == 0x60:
                    ptr, size = struct.unpack_from(">QI", data, a)
                    name += f" {ptr:08x} size {size}"
            else:
                n = 0
                name = f"UNKNOWN AURORA {sub:04x}"
            p = a + n
            hist["aurora " + name.split()[0]] += 1
        else:
            name = f"UNKNOWN OPCODE {op:02x}"
            hist[name] += 1
            p += 1
        if len(out) < limit:
            out.append(("  " * depth) + f"{start:6d} {name}")


def main():
    data = open(sys.argv[1], "rb").read()
    limit = int(sys.argv[sys.argv.index("--list") + 1]) if "--list" in sys.argv else 80
    hist, out = Counter(), []
    parse(data, GX(), hist, out, limit)
    print(f"{len(data)} bytes")
    for k, v in hist.most_common():
        print(f"{v:7d} {k}")
    print("\n".join(out))


if __name__ == "__main__":
    main()
