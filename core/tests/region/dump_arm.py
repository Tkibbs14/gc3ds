#!/usr/bin/env python3
"""Disassemble an X4 SD dump outside the repository.

Dump records are little-endian {guest_pc, host_pc, words}, then that block's emitted ARM hot code and exits.
The normal JIT's shared stubs and separate cold arena are not part of these records.
Run after copying the SD file to a private directory: dump_arm.py /external/x4_jit_N.bin /external/arm-view
"""
from pathlib import Path
import struct
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
source, output = map(lambda x: Path(x).resolve(), sys.argv[1:])
if output.is_relative_to(repo):
    raise SystemExit("keep captured/generated guest code outside the repository")
output.mkdir(parents=True, exist_ok=True)
data = source.read_bytes()
at = 0
count = 0
while at < len(data):
    if len(data) - at < 12:
        raise SystemExit("truncated record header")
    guest, host, words = struct.unpack_from("<III", data, at)
    at += 12
    if words > 65536 or words * 4 > len(data) - at:
        raise SystemExit("invalid/truncated block record")
    raw = output / f"{count:04d}-{guest:08x}-{host:08x}.bin"
    raw.write_bytes(data[at:at + words * 4])
    at += words * 4
    asm = subprocess.check_output(["/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump", "-D", "-b", "binary",
                                   "-m", "arm", "-EL", f"--adjust-vma={host}", str(raw)], text=True)
    raw.with_suffix(".s").write_text(asm)
    count += 1
print(f"{count} emitted blocks decoded in {output}")
