# Tasks worth doing, with the evidence behind each

Real work, scoped, with the numbers that make it worth doing and the checks that prove it. Read `AGENTS.md` first (the
rules, the checks, how to measure) and `docs/HANDOFF.md`'s newest state. **Nothing here needs permission to start** -
open an issue saying which one you are taking so two people do not do the same thing.

Each task says whether a console is needed. Several do not need one, and one of them (the first) is the single most
useful thing a newcomer can do.

## No console needed

### 1. Re-measure a result that was taken on the scripted walk
**Why:** on 2026-10-30 we found the scripted Ordon walk cannot judge any change that shifts the guest's timeline: the
presses are applied when the guest polls the pad, so a moved clock sends Link down a different path and the scenes
differ by tens of percent in draws a picture (REDESIGN_CANDIDATES C41). Eight per-port figures turned out to be
artifacts that way, and two ports that were "gains" were in fact losses. **Many older results in
`docs/research/MEASUREMENTS.md` were taken the same way and nobody has re-read them.**
**What to do:** pick an entry, work out from its job numbers and options whether it used the walk (`--press`) or an
A/B that recompiles, and say in an issue whether it is still trustworthy. No hardware: this is reading and arithmetic.
**Verify:** quote the job numbers and the reasoning. If it needs a re-run, write the exact relay command.

### 2. Add a case to the JIT's ARM bench
**Why:** `core/tools/jitbench.py` runs hand-written PowerPC loops through the real JIT under qemu and reports hot ARM
words per guest instruction and modelled ARM11 stalls - the metric that decides most on this hardware, since
instruction-fetch stalls are 27% of core 0. It has four cases (integer list walking, a float chain, paired singles,
CodeWarrior calls). It needs more shapes: string and memory copies, switch tables, virtual calls, the
load-multiple/store-multiple prologues, denormal-heavy float.
**What to do:** write `core/tests/jit/bench/<name>.S`, label the hot loop `loop:`, run
`python3 core/tools/jitbench.py core/tests/jit/bench/<name>.S --cycles --disasm`, and open a pull request with the
numbers. Needs qemu-arm and arm-linux-gnueabihf-gcc, both in Debian/Ubuntu.
**Verify:** the numbers in the description; the tool's own `--selftest`-style output is the check.

### 3. Give the measurement tools a standard error
**Why:** defaults are decided from separate whole runs, and the project pools them by hand. `core/tools/runs.py`
prints one line a run; it should pool repeats of the same arm and print the mean with its standard error, so a 0.3%
claim can be told apart from a 0.3% spread (today's `OPT_FDIRECT` needed three pairs to settle exactly that).
**What to do:** extend `runs.py` (and `clock_split.py`) to group logs given as `--arm name file1 file2 ...`.
**Verify:** run it over the logs named in MEASUREMENTS' tables and show it reproduces the published means.

### 4. Model a candidate before anyone spends console time
**Why:** `core/tools/arm11_sched.py` models the ARM11's interlocks over the JIT's real output; several parked
candidates in `docs/research/REDESIGN_CANDIDATES.md` have never been modelled.
**What to do:** pick a parked candidate, model it, and add the result to its entry (never delete an entry - update it).

## A console helps (New 3DS / New 2DS XL, homebrew-enabled)

### 5. Bisect the native stand-ins the way the ports were bisected
**Why:** the ports' bisect found two of eight were dead weight, worth +0.71% and +0.55% once removed. **The stand-ins
(`core/src/cpu/hle_*.c`: the paired-single library, the J3D display-list writers, the THP decoder, the shadow-scan
family) have never had the same treatment** - and they are charged the same way, by instruction fetch rather than work.
**What to do:** separate whole runs with `--hle off`, `--ps off`, `--psflush off` and the `--ab` names in
`REMOTE_RUNS.md`, on a state that takes no input, scored on frames a second with `core/tools/runs.py`. Compare
against `core/tools/port_cost.py`-style cost (calls x microseconds a call) from the same log.
**Verify:** two runs an arm (they agree to ~0.1% on a static scene), and say which build you used.

### 6. Measure a game nobody has measured
**Why:** every number in the repository comes from four games (Twilight Princess, Wind Waker, Mario Party 4, and the
test DOLs). A fifth game's profile would tell us whether the conclusions generalise - `OPT_FDIRECT`, for instance, is
+2.6% in Wind Waker and flat in Twilight Princess because their hot code differs in how float-dense it is.
**What to do:** boot a game you own, find where it breaks or how fast it runs, and report the profile
(`--profile on`), the clock split (`core/tools/clock_split.py`) and what the GX thread was doing. **Never attach game
data**: no disc images, no save states of commercial games, no assets - numbers, logs and screenshots of your own
console only.

### 7. Hardware facts we still do not have
**Why:** designs here are decided by measured hardware behaviour (`docs/FINDINGS.md`,
`docs/research/TIMING_MODEL_PLAN.md`). Open questions include the write-buffer's behaviour on bursts to uncached
memory (it decides C40, worth ~1.6% of core 0 in every game), the cost of a cache maintenance operation by range, and
whether the micro-TLB can be made to matter.
**What to do:** add a probe to `bench/` or `core/host3ds/timingprobe.c` and report the numbers with the method.

## Larger, well-specified pieces

### 8. Let core 2 write the FIFO mirror (C40)
**Why:** `gp_burst` hands every 32-byte burst to the GX thread *and* memcpys the same bytes into guest RAM on core 0 -
duplicate work worth **Ordon 1.6% and the heavy area 0.5%, in every game** (measured with `--fifomirror off`). Core 2
is idle 12-13% in gameplay and already holds the bytes.
**What to do:** C40 in REDESIGN_CANDIDATES has the exact design, including what makes it safe (who reads FIFO memory,
and why the reported read pointer must advance only after the write) and the one hole plus how to close it.
**Verify:** a check mode comparing the deferred mirror against the eager one over a run, with a planted difference
that must be caught; then the usual A/B.

### 9. The give-up that blocks big ports
**Why:** a native port of a whole subtree is the only port shape that can pay under the fetch rule, and the thing
stopping one today is that a single denormal input makes a port give up and abandon the subtree: **2,108 of 2,134
give-ups are `plain()` refusing a denormal** (`docs/research/LIBRARY_PORTS.md`).
**What to do:** compute that one item through the interpreter's exact soft float (as `hle_ps.c`'s SOP / SMADD do)
instead of aborting, so the subtree continues.
**Verify:** `--port check` on a console (every call also run by the interpreter and compared, 0 differed) and
`--port plant` to prove the checker still reports a difference.

### 10. A port of a whole subtree
**Why:** the ports as a set are worth +10.6% while their parts sum to ~3.7% - the difference is the guest code the set
keeps out of the compiled code. A port must therefore **remove much more guest code than the native code it adds**; a
40-instruction leaf can never pay (one such port was a 0.5% loss). The named candidates are J3D's packet draw and the
remaining TEV block types.
**What to do:** read `docs/research/LIBRARY_PORTS.md` ("Entry", and how a context covers a subtree), start from the
profile of the scene you care about, and do task 9 first.
**Verify:** `--port check` with 0 differed, `--port plant` catching a planted difference, and separate whole runs on a
scene that takes no input.

## How a measurement gets accepted

`docs/TELEMETRY.md` says what to submit and how a number is checked - the short version: the baseline and the change
from **one session and one build**, two runs an arm, frames a second from a state that takes no input, with the log
attached. The checks exist because the device drifts ~2% between days and builds move a baseline ~0.8%, so numbers
from different sessions cannot be compared.
