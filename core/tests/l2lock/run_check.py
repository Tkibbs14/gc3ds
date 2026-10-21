#!/usr/bin/env python3
"""X2 transaction checks and planted source mutations. No console access."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[3]
source = (root / "core/host3ds/l2lock.c").read_text()
plants = {
    "wrong polarity": ("0x5555u : 0x55u", "0xaaaau : 0xaau"),
    "skip readback": ("value != s->wanted[i]", "false"),
    "lost failed-store cleanup": ("s->attempted |= 1u << i;", "s->attempted |= 0;"),
    "skip restore write": ("if ((s->attempted & (1u << i))", "if (false"),
}
with tempfile.TemporaryDirectory(prefix="x2-check-") as tmp:
    tmp = Path(tmp)
    for label, mutation in [("normal", None), *plants.items()]:
        code = source
        if mutation:
            old, new = mutation
            assert old in code, label
            code = code.replace(old, new)
        (tmp / "transaction.c").write_text(code)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(root / "core/host3ds"),
                        str(tmp / "transaction.c"), str(root / "core/tests/l2lock/transaction.c"),
                        "-o", str(tmp / "check")], check=True)
        result = subprocess.run([str(tmp / "check")], capture_output=True, text=True)
        if label == "normal":
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode == -signal.SIGABRT and "Assertion" in result.stderr, (label, result.stderr)
            print(f"X2 plant caught: {label}")
