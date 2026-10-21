#!/usr/bin/env python3
"""Supplementary synthetic ARM oracle checks; takes an external MEM1 dump, never copies it into the repo.
Run: python3 core/tests/region/run_check.py /absolute/external/mem.bin
The device captured-call checks, not these synthetic cases, gate X4 measurements.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
subprocess.run(["bash", str(root / "tools/arm_build.sh")], check=True)
objects = [str(p) for p in (root / "build-arm/obj").rglob("*.o") if p.name != "pc_main.o"]
with tempfile.TemporaryDirectory(prefix="gc3ds-x4-") as tmp:
    exe = str(Path(tmp) / "check")
    subprocess.run(["arm-linux-gnueabihf-gcc", "-O2", "-g", "-march=armv6k", "-mfpu=vfp",
                    "-mfloat-abi=hard", "-marm", str(Path(__file__).with_name("oracle_check.c")),
                    str(root / "host3ds/region_oracle.S"), *objects, "-lm", "-o", exe], check=True)
    for args in [("3",), ("2",), ("1",), ("1", "full"), ("1", "full", "parent"), ("1", "full", "parent", "children")]:
        subprocess.run(["qemu-arm", "-L", "/usr/arm-linux-gnueabihf", "-cpu", "cortex-a7", exe,
                        str(Path(sys.argv[1]).resolve()), *args], check=True)
