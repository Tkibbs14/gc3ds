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
**Two more ways a recorded number can be wrong, both found on 2026-10-30, both checkable by reading:**
- **A pooled A/B mean with its uncertainty as large as the effect.** The J3D display-list writers were adopted on
  "+5.17% (+- 5.08)". Pooled A/B means are the method C41 showed produces phantoms; `ab_phases.py` exists because of
  it. Any entry quoting a pooled mean, or a +- near its own effect, is a candidate.
- **An A/B whose two arms were the same code.** Until 2026-10-30 the perf.csv header named "traces" whatever `--ab`
  asked for, and an `--ab NAME` the build did not have ran **no A/B at all, silently**. So an A/B result from a build
  older than that is only as good as its option column: check that the column actually changes between phases, and
  that the feature's own counter (`# hle 10s`, `# ps library 10s`, `# port 10s`) is non-zero in the "on" arm.

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

### 5. Finish the stand-in bisect (started 2026-10-30; the THP decoder and the shadow-scan family are left)
**Why:** the ports' bisect found two of eight were dead weight. The stand-ins are charged the same way - by
instruction fetch, not work - and the bisect begun on 2026-10-30 has already found one loss and one withdrawn
credit: **`--psflush` is off by default** (a -0.06 to -0.33% loss in all four games, against a recorded +0.8% /
+1.7%), and the **J3D display-list writers (`--hlegd`) run in none of the benchmark states at all**, so their
+2-3% cutscene credit is withdrawn (MEASUREMENTS, jobs 264-265 and the correction under it). The paired-single
library came out intact (+3.8% Ordon, +1.1% heavy area), and the whole stand-in group costs **1.14% of core 0**
(job 1569).
**What is left:** `--thp off` on the title's movie section, and the `hle_tp.c` shadow-scan family one piece at a
time (`--shdwmemo`, `--shdwtri`, `--shdwcb`, `--trifast`) on the title's later stretch, which is where they pay.
**What to do:** separate whole runs, two an arm, on a state that takes no input, scored on frames a second with
`core/tools/runs.py --skip 80`; `core/tools/title_sections.py` for tp850. Read the cost side from
`# stand-in cost 10s` in the same log.
**Verify, and this is the part that matters:** **check the arms actually differed before believing a null.** Two
runs an arm agreeing to 0.01% is what you get when both arms ran the same code - that is how the hlegd result was
caught (`# hle 10s` reported `J3DGD runs 0` with the group *on*). Every stand-in has a counter in `# hle 10s` or
`# ps library 10s`; quote it for both arms, and say which build you used.

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
**Why this is now the *first* direction and not one of several** (2026-10-30, job 1569): frames a second is the
emulated clock rate, and a port delivers clock **3.3x as cheaply** as running the guest code it replaces (ports
9.25% of the clock for 9.65% of core 0; JIT block code 23.4% for 80.9%). Core 0's codegen cannot be the answer -
half of core 0 is memory stalls, no layout change fixes fetch (50% of time in compiled code is 54.8 KB against a
16 KB L1), and no peephole reaches the -15 to -20% of hot words that +10% would need. See MEASUREMENTS' job-1569
sections.

### 11. Give one of the remaining ports a one-copy commit
**Why:** `port_commit` is **2.61% of core 0** in TP's heavy area and it is pure overhead (unlike `port_run`, which
for a subtree port is the work). The J3D light port's commit was a store per byte, 8.7 us a call; **one copy turned
that port from a measured loss into +2.7% in the heavy area** (2026-10-24). The technique has not been applied to the
other four. Per call today (job 1569): `J3DSys::reinitGX` 39.78 us, the pane walk 40.40, `J3DMaterial::diff` 7.03,
`J2DMaterial::setGX` 9.61.
**What to do:** read `port_commit` in `core/src/cpu/port_j2d.c` - the first two loops write scattered single words
(`ram_wr32`) for the GX register shadow and the store log. Coalesce runs of consecutive words the way `ram_from_be`
already does for display-list bytes. **Do not touch the pipe handoff**: commit time tracks pipe bytes, and those
bytes are copied twice in `gp_burst`, which is C40's to fix, not the port's.
**Verify:** `--port check` with 0 differed and `--port plant` catching a planted difference, then separate whole runs
on a scene that takes no input, with the `us per call` line from `# port 10s` before and after.

## How a measurement gets accepted

`docs/TELEMETRY.md` says what to submit and how a number is checked - the short version: the baseline and the change
from **one session and one build**, two runs an arm, frames a second from a state that takes no input, with the log
attached. The checks exist because the device drifts ~2% between days and builds move a baseline ~0.8%, so numbers
from different sessions cannot be compared.
