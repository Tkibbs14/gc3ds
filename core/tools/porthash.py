"""porthash.py DOL SYMBOLS ADDR[:SIZE]...: the port's guest-code hash table (port_j2d_hashes.inc)."""
import re, struct, sys
dol, symf = sys.argv[1], sys.argv[2]
d = open(dol, 'rb').read()
offs = struct.unpack('>18I', d[0:72]); addrs = struct.unpack('>18I', d[72:144]); sizes = struct.unpack('>18I', d[144:216])
def read(addr, size):
    for o, a, s in zip(offs, addrs, sizes):
        if s and a <= addr and addr + size <= a + s: return d[o + addr - a: o + addr - a + size]
    raise SystemExit('not in DOL: %x' % addr)
syms = {}
for line in open(symf):
    m = re.match(r'(\S+) = \.\w+:0x([0-9A-F]+); // type:(\w+) size:0x([0-9A-F]+)', line)
    if m: syms.setdefault(int(m.group(2), 16), (m.group(1), int(m.group(4), 16)))
for arg in sys.argv[3:]:
    a, _, sz = arg.partition(':')
    a = int(a, 16)
    name, size = syms[a] if not sz else (syms.get(a, ('data',))[0], int(sz, 16))
    b = read(a, size)
    h = 0xCBF29CE484222325
    for i in range(0, size, 4):
        h = ((h ^ struct.unpack('>I', b[i:i+4])[0]) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    print('    { 0x%08X, 0x%X, 0x%016Xull },  // %s' % (a, size, h, name))
