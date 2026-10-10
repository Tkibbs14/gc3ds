#!/usr/bin/env python3
"""Static instruction mix of compiled GameCube code (devkitPPC ELFs).

Static, not dynamic: it counts instructions as they appear in the binary, not as they execute, and libogc/newlib
library code dominates small programs. It answers "which instruction classes must the JIT handle well" and
"how long are straight-line runs between branches" (each branch ends a JIT block), not "what does a game spend
time on". A dynamic profile needs an executing core.

    python3 analyze_mix.py <dir-with-elfs>
"""
import collections
import re
import subprocess
import sys
from pathlib import Path

OBJDUMP = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump"

CLASSES = [
    ("branch", re.compile(r"^(b|bl|ba|bla|bc\w*|bd\w*|beq|bne|blt|bgt|ble|bge|bns|bso|bdnz\w*|bdz\w*|blr\w*|bctr\w*|b\w+lr\w*|b\w+ctr\w*)$")),
    ("psq ld/st", re.compile(r"^psq_")),
    ("paired arith", re.compile(r"^ps_")),
    ("fp load/store", re.compile(r"^(lf|stf)")),
    ("fp arith", re.compile(r"^f")),
    ("int load", re.compile(r"^(lbz|lhz|lha|lwz|lmw|lwbrx|lhbrx|lwarx|lswi|lswx)")),
    ("int store", re.compile(r"^(stb|sth|stw|stmw|stwbrx|sthbrx|stwcx|stswi|stswx)")),
    ("compare", re.compile(r"^cmp")),
    ("cr logic", re.compile(r"^(cr|mcrf|mcrxr|mfcr|mtcrf)")),
    ("spr/msr", re.compile(r"^(mf|mt)")),
    ("cache/sync", re.compile(r"^(dc|ic|sync|isync|eieio|tlb|sc|rfi)")),
    ("rotate/shift", re.compile(r"^(rlw|sl|sr|sra|ext|cntlz|clr|rotl|rotr)")),
    ("mul/div", re.compile(r"^(mul|div)")),
]


def classify(m):
    for name, rx in CLASSES:
        if rx.match(m):
            return name
    return "int alu"


def dol_text_blob(dol):
    """Concatenated text sections of a DOL executable (header: 7 text + 11 data offsets/addresses/sizes)."""
    raw = Path(dol).read_bytes()
    word = lambda i: int.from_bytes(raw[i:i + 4], "big")
    blob = b""
    for k in range(7):
        off, size = word(0x00 + 4 * k), word(0x90 + 4 * k)
        if off and size:
            blob += raw[off:off + size]
    return blob


def disasm(path):
    if str(path).endswith(".dol"):
        tmp = Path("/tmp") / (Path(path).stem + ".text.bin")
        tmp.write_bytes(dol_text_blob(path))
        cmd = [OBJDUMP, "-D", "-b", "binary", "-m", "powerpc:750", "-EB", "-M", "gekko", "--no-show-raw-insn", str(tmp)]
    else:
        cmd = [OBJDUMP, "-d", "-M", "gekko", "--no-show-raw-insn", str(path)]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or not parts[0].strip().endswith(":"):
            continue
        m = parts[1].split()[0] if parts[1].split() else ""
        if not m or m.startswith((".", "(")):
            continue
        yield m.rstrip(".+-")


def analyse(elfs):
    mn = collections.Counter()
    runs = []
    seen = set()
    for elf in elfs:
        run = 0
        for m in disasm(elf):
            mn[m] += 1
            run += 1
            if classify(m) == "branch":
                runs.append(run)
                run = 0
        seen.add(elf.name)
    total = sum(mn.values())
    cls = collections.Counter()
    for m, c in mn.items():
        cls[classify(m)] += c
    return total, mn, cls, runs


def main():
    d = Path(sys.argv[1])
    elfs = [d] if d.is_file() else sorted(d.glob("*.elf"))
    total, mn, cls, runs = analyse(elfs)
    print(f"{len(elfs)} ELFs, {total} instructions (static, library code repeated per program)\n")
    print("class            share")
    for name, c in cls.most_common():
        print(f"  {name:<14} {100 * c / total:5.1f}%")
    runs.sort()
    mean = sum(runs) / len(runs)
    print(f"\nstraight-line run between branches: mean {mean:.1f}, median {runs[len(runs) // 2]}, "
          f"p90 {runs[len(runs) * 9 // 10]} instructions")
    print("\ntop 40 mnemonics")
    for m, c in mn.most_common(40):
        print(f"  {m:<10} {100 * c / total:5.2f}%")
    ps = {m: c for m, c in mn.items() if m.startswith(("ps_", "psq_"))}
    print("\npaired-single mnemonics present:", ", ".join(f"{m} {c}" for m, c in sorted(ps.items(), key=lambda x: -x[1])) or "none")


if __name__ == "__main__":
    main()
