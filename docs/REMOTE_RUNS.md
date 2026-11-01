# Remote runs: testing on the console from anywhere

The console and the PC never need to be on the same network. Builds, jobs, logs, live commands and screenshots go
through a small HTTP relay that you run yourself (bring your own server):

```
PC  core/tools/relay.sh ──HTTP(S)──►  relay (builds, job queue, logs, frames)  ◄──HTTP(S)──  console: gc3ds "Remote runs" (core/host3ds/relay.c)
```

The console polls the relay, downloads a build it does not have, installs it over its own `.3dsx`, restarts into it by
itself, runs the job and streams its `perf.csv` back. A full measurement loop (edit, build, queue, read the A/B result)
needs no hands on the console.

## Running a relay

A reference server with no dependencies is in `tools/relay-server/server.js` (Node 18+; state is a directory):

```bash
export RELAY_TOKEN=$(openssl rand -hex 32)           # keep it: the PC and the console use the same token
node tools/relay-server/server.js --port 8787 --data ./relay-data

**Commit before you push a build.** `build.sh` embeds the commit and marks it `-dirty` when the tree is not clean,
`runs.py` prints that in red letters, and such a run **cannot be quoted as a measurement** - it is not reproducible,
because nothing records what was in the tree. An A/B on one dirty build is still internally valid (both arms are the
same binary), but the figure has to be taken again on a clean build before it goes in the ledger, and the binary
cannot be checked after the fact once it is overwritten. Cost of the lapse on 2026-11-01: four device runs
(1793-1796) thrown away and queued again as 1797-1800 - and then **four more** (1828-1831, the C49 test), because a
note is not a check. **`relay.sh push` now refuses a binary that was built from a dirty tree** (`RELAY_ALLOW_DIRTY=1`
overrides), reading `host3ds/version.inc`, which `build.sh` wrote at build time and which therefore describes the
*binary* rather than the tree. That distinction is the trap: `push` takes its note from the tree as it is now, so a
build made before a commit and pushed after it is recorded as clean while the binary says `-dirty`.
```

Put it behind HTTPS (for example Caddy as a reverse proxy) if it is reachable from the internet: the token travels in a
header. The console checks the server's certificate against `core/host3ds/certs/relay_ca.pem`, which holds Let's
Encrypt's roots (ISRG Root X1 and X2, what Caddy uses by default); add your CA's root there for another one. Plain
`http://` works too on a network you trust. Any server that implements the protocol below works the same.

## Client settings

`core/host3ds/relay.txt` on the PC and `sdmc:/gc3ds/relay.txt` on the console (both outside git):

```
url https://your-host
token <RELAY_TOKEN>
auto 1          # console only: enter Remote runs 3 s after launch (B for the menu; menu key L enters it by hand)
```

The routes below are appended to `url`.

## Everyday use (PC)

```bash
bash core/host3ds/build.sh                         # build core/host3ds/gc3ds.3dsx
bash core/tools/relay.sh run --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 300 --ab fcache
bash core/tools/relay.sh queue <options>           # queue without following (prints the job id)
bash core/tools/relay.sh log <job>                 # follow a job into core/out/relay_<job>.csv
bash core/tools/relay.sh jobs                      # the last 20 jobs: id, status, build, the build's commit, created, args
bash core/tools/relay.sh cancel <job>              # a job still queued
bash core/tools/relay.sh cmd <job> "set opt 1"     # a live command for the running job (see host3ds/main.c apply_cmd)
bash core/tools/relay.sh frames <job>              # the job's top-screen frames (BMP, PNG when ImageMagick is there)
```

`RELAY_BUILD_ID=<id>` queues against a build already on the relay (the id `push` printed) instead of uploading one, so
a machine without devkitPro — a cloud session, say — can run jobs for a build another machine built.

`run` / `queue` upload the current `gc3ds.3dsx` if the relay does not have it (builds are named by the first 16 hex
digits of their sha256) and tie the job to it; `queue` refuses to queue when the upload failed.

Live commands (`cmd`, taken within ~2 s): `shot` (a top-screen frame, then `frames`), `stop`, `set NAME VALUE` (opt,
sched, merge, vtrim, dlcache, shapes, ...), and for steering a game from the PC: `press BUTTON N` (A, B, X, Y, Z, L, R,
START, UP, DOWN, LEFT, RIGHT, held N fields, default 6), `stick X Y N` (the main stick, 0-255 with 128 the centre, held
N fields) and `save sdmc:/gc3ds/states/NAME.state` (a save state at the next field). Menus answer the stick, not the
d-pad. That is how gameplay benchmark states are made: queue a long run, then press / shot / look until the game is
where you want it, and save. `--press` takes the same buttons plus `SLEFT` / `SRIGHT` / `SUP` / `SDOWN` (the stick to
an edge) for scripted runs; its fields count from the run's start.

**Resolving a build id to source.** Every figure in `docs/research/MEASUREMENTS.md` names a build id (a content hash
of the 3dsx), and the project's method is "one build, one session" - so being able to get from an id back to the code
is the difference between a reproducible measurement and a number. Two places carry it now (both added 2026-10-31):

- **In the log itself.** `core/host3ds/build.sh` writes the commit into `version.inc`, and every run's perf.csv
  header opens with `# build: gc3ds <commit>`. `runs.py` prints it beside the score, so a number you read is never
  separated from the source that produced it. `unknown (not built by build.sh)` means a bare `make`; a **`-dirty`**
  suffix means uncommitted changes, and `runs.py` says loudly that such a run cannot be tied to source - do not
  quote it as a measurement.
- **On the relay.** `relay.sh push` has always sent the same commit as the build's `note`, but the relay **wrote it
  to disk and never served it**, so nobody reading the repository could make the mapping for any run on record.
  `relay.sh jobs` now prints it as a column (id, status, build, **commit**, created, args). An older relay leaves
  the column out, so anything parsing that output must find `args` by content rather than by position - the build is
  column 3 either way.

## On the console

- It polls every 10 s and runs jobs one after another.
- A job for a build the console does not have: it downloads it (gzip, with progress, stall detection and retries) over
  its own `.3dsx`, keeps the job in `sdmc:/gc3ds/relay_pending.txt`, and restarts into the new build through Luma3DS's
  homebrew loader (the `hb:ldr` port, opened with `svcConnectToPort` as the Homebrew Launcher does). The job log
  records how long the restart took. If the loader does not answer, gc3ds asks for a restart by hand.
- A restarted job numbers its log chunks from 100000, so they never replace the chunks the first instance sent.
- The console needs internet (phone hotspot or home Wi-Fi; many work networks isolate devices). Keep the lid open:
  sleep drops Wi-Fi, and a job interrupted that way stays "running" on the relay (cancel it).

## Job options (also 3dslink arguments: `3dslink -a <ip> gc3ds.3dsx -- <options>`)

| Option | Meaning |
|---|---|
| `--game SUBSTR` | the first program whose path contains SUBSTR (case-insensitive) |
| `--state PATH` / `--save-state FIELD:PATH` | load a save state after boot / save one when the VI reaches FIELD |
| `--seconds N` | end after N real seconds (relay jobs default to 600) |
| `--ab NAME` | alternate one setting every 20 s within the run (its column in perf.csv says which half each row is in). The names are the `abXxx` fields in `host3ds/main.c`'s `RunOpts` (parsed at `--ab`); among them `opt`, `kstore`, `merge`, `hle`, `ps`, `psgx`, `psflush`, `rawdraw`, `rawstate`, `mips`, `smalltex`, `tevver`, `deflit`, `shlight`, `vscache`, `blacklit`, `vramtex`, `cmdbuf`, `qbig`, `splitverts`, `l2pf`, `gxdiag`, `profclock`, `gpudiag`, `texdiv`, `gfskip`, `port`, `pichook`, `j3dhook`, `flushhook`, `lighthook`, `gpbulk`, `ringbatch`, `dblframe`, `portcharge` |
| `--opt`, `--kstore`, `--merge`, `--hle`, `--mips`, `--deflit`, `--shlight`, `--vramtex`, ... `on\|off`; `--smalltex N`, `--splitverts N`, `--l2pf N`, `--gpudiag BITS`, `--texdiv N` | start values (see `host3ds/main.c` `opts_default` for the defaults) |
| `--gfskip ADDR`, `--gfskip-n N`, `--gfskip-ok PTR+OFF=VAL,..` | guest frame skip: the game's renderer at ADDR every Nth call, only while the guards hold (TP: `--gfskip 8000A604 --gfskip-ok 80450BC8+4=1,80450BC8+20=ffffffff`) |
| **the card filling up** | every run's header now carries `# card: N MB free`, with a `*** LOW ***` marker under 32 MB that `runs.py` repeats on the run's line. `sdmc:/gc3ds/perf.csv` is appended by every run. It used to grow without bound (~1.4 MB a 240 s job, ~96 MB in one day of measuring) and on 2026-10-30 left the card with under 6 MB free, so the 2DS could not download a build and six queued jobs failed with curl's **"Failure writing output to destination"** - a job whose build is already cached keeps working, which is the tell. It now rotates at 24 MB to `perf.prev.csv`. If you meet that error on an older build, free space on the card (perf.csv, perf.prev.csv, `shots/`); every relay job's log is on the relay and in `core/out/` already |
| `--shots N` | a screenshot every N perf rows (default 0: only the two in the warm-up). **Leave it off while scoring**: writing the BMP and shipping the frame to the relay steals core 0 for 4-86% of that second, which is what made runs look like they drifted 2.5% on 2026-10-30. Turn it on to watch where a run is, with `relay.sh frames JOB` |
| `--gfskip-snap on\|off` | (on) draw one picture in a **whole number** of game frames. A picture lands only on a frame boundary and a frame costs `cd` drawn or `cs` skipped, so the achievable gaps are `cd + n*cs` and nothing between; a fractional rate alternates between two of them and reads as jitter (32% of the mean gap at target 50, job 1715). Snapping gives up hitting the target exactly for gaps that are all the same length. **Note the sharp edge:** a floor just above an achievable point collapses to drawing everything (asking for 8 pictures when the points are 10.8 and 7.9 switches the feature off), so read `wants to draw` and `skipped` before believing a paced run did anything |
| `--gfskip-minfps P` | with `--gfskip-target`: **never show fewer than P pictures a second** (default 5), letting the *game speed* give way instead of the display - the owner's choice, 2026-10-31. Without it a heavy scene shows 1.4 pictures a second while holding its speed target; with it at 5, TP's heavy area runs 35% of full speed at 5 pictures a second and static Ordon 67% at 5. It is a floor and not a cap: a scene that can hold its target above P is left alone. 0 leaves only the ratio backstop (one picture in ten). `# gfskip target 10s` says which of the two decided each window |
| `--gfskip-target PCT`, `--gfskip-hz HZ` | guest frame skip **paced to a game speed** instead of a fixed ratio: hold the game at PCT of its own rate (`--gfskip-hz`: TP and Wind Waker 30, Mario Party 4 60) and spend what is left on pictures, drawn as evenly as the ratio allows. Use it instead of `--gfskip-n`, with the same `--gfskip` and `--gfskip-ok`. `# gfskip target 10s` reports the frame costs it learned, the fraction it wants to draw, and whether it is pinned at the picture-rate floor (1 in 10) or drawing everything. Why a target: what a player feels is the game's speed *changing* between scenes (static Ordon 35%, the heavy area 24%), and a ratio keeps that; a target moves the variation into the picture rate instead |
| `--memo ADDR` | function memoization probe on the function at ADDR (measurement only; `# memo 10s`) |
| `--tribench N`, `--panestat on` | measurement: time the ShdwDraw triangle path's pieces on its Nth run (`# tri bench`, core-0 cycles); count J2DPane::draw's pane classes by vtable (`# pane types 10s`). Every run with the shadow stand-in active also reports `# shdw where 10s` (where in the stand-in core 0's samples fall) |
| `--port off\|on\|check\|plant`, `--port-hooks MASK`, `--port-parts on` | the native library ports (on; MASK: bit 0 J2DMaterial::setGX, 1 J2DPictureEx::drawSelf, 2 the J3D concat-view matrices, 3 __GXSendFlushPrim, 4 J3D's light loads, 5 the J2DPane::draw walk, 6 J3DMaterial::diff, 7 J3DSys::reinitGX, 8 the minimap's renderingDecoration, 9 J3DShapeMtxMultiConcatView::load, 10 J2DTextBoxEx::drawSelf; default **243** = bits 0, 1, 4-7; bit 2 and bit 8 came off on 2026-10-30, +0.71% Ordon and +0.55% heavy - apportioned on 2026-10-31, three runs an arm: **bit 8 is all of it (-0.26% heavy, 3 of 3 runs below all 3 baseline runs), bit 2 is neutral (-0.03%) despite 1.53 M calls, bit 3 neutral (+0.08%)**); `--pmu-port-hooks MASK`: `--pmu-split port\|portcommit` around these hooks only; check: every call compared with the interpreter on the console (`# port 10s`, its `differed` count), plant: a planted difference the check must report; parts: per-part timing. **`# port give-ups 10s` names the line of `port_j2d.c` that refused**, per reason, which is what says what to port next: the reason column is coarse (reason 5, printed as `unrecognised`, has **21 sites** - it was labelled "tev block" until 2026-10-31, and in TP's heavy area the pane walk's 4,720 refusals a 10 s window under it need not have been a TEV block at all). The line numbers are of the build in the run's header |
| `--hlegd on\|off` | the J3D display-list writer stand-ins (on). Added 2026-10-30 so the group can be decided by separate whole runs: its `--ab hlegd` read -0.03 ± 0.12 points in static Ordon and -0.34 ± 0.34 in the heavy area, which settles nothing |
| `--fdirect on\|off` | float arithmetic writes frD's lane register instead of a scratch one and a vmov (OPT_FDIRECT, on since 2026-10-30: Wind Waker +2.6%, Mario Party 4 +0.7%, TP flat); `--ab fdirect` |
| `--port-picture off` | the pane walk's acceptance of **J2DPicture** panes (**on** since 2026-11-01). Measured with `--port-walktext` as one change (C53): static Ordon **+3.46%** (jobs 1760-1767) and **+3.17%** (jobs 1785-1788, a second build, arms adjacent), TP's heavy area **+2.31%**; every on-run above every off-run in both rounds, all arms inside one launch memory state (C55). Its value is *scan acceptance* and not drawing - a subtree is all-or-nothing, so a hidden J2DPicture still has to pass `walk_scan` for its subtree to be taken. `--port-picture-draw` is the drawing path and stays off
| `--port-softfloat on` | compute **PSMTXConcat through the interpreter's exact soft float** when an input is a denormal, a NaN or an infinity, instead of giving up. **Exact and effective as of 2026-11-01** (jobs 1791-1792, TP's heavy area, after `d600311` let an exact NaN through `psq_copy`): `--port check` **differed 0** with the exact path actually taken - **every** float give-up now reads 0, both `plain nan/inf` and the `psq_copy` slot it used to relocate to - and the walk's float refusals fall from **60 a window to 0**, leaving only its 50 root refusals. `--port plant` differed 33,616. **But the prize is small and was over-estimated from refusal counts:** the 60 refused entries do not become taken calls, they *vanish* from the hook, because each accepted walk now absorbs them - `checked` stays at 50 while instructions a call rise 104,292 -> **105,766** (+1.4%) and pipe bytes 14,984 -> 15,210. So ~1.4% more covered work, not the 2.2x a refusal count suggested. Off until a speed run justifies it (jobs 1793-1796) |
| `--j3dstat on` | **measurement only (C56):** count the calls at each entry point of the `J3DDrawBuffer::drawHead` subtree and let the guest code run, reported as `# j3d subtree 10s`. That subtree is **9.88%** of core 0 in TP's heavy area (job 1821) and the largest unexplored one; a port of it must reproduce two **closed** class sets - four packet classes (`J3DMatPacket`, `J3DShapePacket`, `J3DDrawPacket`, `J3DPacket` whose body is a bare `blr`) and six `J3DShapeMtx::load` implementations - and **a slot that stays 0 is code that need not be written at all**. Counting entry points needs no struct layout and cannot perturb what it measures, unlike walking the buffer. Two of the six matrix loads are already written as port bits 2 and 9, which measured -0.03% and -0.46% as standalone *entry hooks*: absorbing them into a subtree context is C56's whole argument, after the same thing turned port bit 10 from -4.3% alone into part of C53's +3.4% |
| `--port-size-refused MASK` | **measurement only, and only with `--port check`:** for refusals whose *reason* is in MASK (a bitmask over the eight reason codes, so 4 = reason 2 = floats), also run the call through the interpreter and add up its guest instructions, reported as `guest instructions a refused call, same order:` on that hook's `# port 10s` line. It answers what the give-up histogram cannot: a reason that refuses many small calls and one that abandons a few large subtrees are **identical in a count**, and the project has read one as the other more than once - the J2DPicture half accepts 412 panes a window and draws none. **It is a mask and not an on/off switch because the measurement perturbs itself:** sizing a refusal means executing the call, and `gekko_run_call` is a bare interpreter loop that never consults this hook, so the refused call's children never reach the walk. With every reason sized at once that is fatal - a peer's PC run on 2026-11-01 saw hook entries at state 4290 fall from 72,000 to 1,476 and the walk look switched off, because *root* refusals (reason 1) were swallowing whole pane trees. Size one reason at a time. **And even then the number is an upper bound on the prize, not the prize**, because part of what a refusal abandons is already covered by walks accepted further down the same tree. Not a speed measurement |
| `--port-picture-draw on` | draw a **visible** J2DPicture pane with the port's own code instead of refusing the subtree at it. **Off, and nothing has ever exercised this path:** `drew picture` is 0 in every checked state on both machines (PC 900/4290/5000/6590 and 2DS jobs 1757-1759, static Ordon with the HUD and minimap live), because once the class test stops refusing, the walk reaches these panes and finds every one of them hidden. So `--port-picture`'s value is **scan acceptance** - a hidden pane still has to pass `walk_scan` for its subtree to be taken - and this switch is the part with no evidence behind it. Turning it on needs a state where `drew picture` is non-zero and `--port check` reads 0 differed there; if TP has no such state, the code should be deleted rather than kept |
| `--port-walktext off` | the pane walk's acceptance of **J2DTextBoxEx** panes, drawn with port bit 10's own `text_draw_self` (**on** since 2026-11-01; measured with `--port-picture` as one change, see the row above). This is the half that unblocked C53: with it the walk's real guest instructions a call went from 13,377 to **104,292** and its pane-class test refuses **nothing** - an empty `# port walk-scan 10s` where job 1755 had 1,410 refusals a window. Still a separate switch from `--port-hooks` bit 10, which is the standalone *entry* hook and lost 4.3% in Ordon: keeping them apart is what made the fold measurable. Exact: device `--port check` differed 0 with text panes drawn (1757-1759), `--port plant` 29,442; a peer's PC run differed 0 over four title states with 3,150 text panes drawn at 6590
| `--indstat on` | **measurement only (C48):** count indirect-branch table probes in the shared stub, reported as `# jit indirect 10s` - probes per 1,000 guest instructions, the split between the first slot, the second (OPT_IND2) and misses, and how many came from `blr` against `bctr`. The table is 2 MB (`FAST_BITS` 16, four tables), the size of the whole shared L2, probed at a pc-derived index, and the rate had never been measured: the two available guesses differed by **eight times**. **Never a default** - the counting costs four instructions a probe, so the rate is reported per guest instruction and not per second, and a run with it on is not a speed measurement. Under qemu it prints once at exit (`JIT indirect:`, with `GC3DS_OPT` including 33554432) |
| `--port-charge PCT` | the ports' charge on the emulated clock, in percent of the calibrated estimate (100; `port_j2d.c` kInsScale). Measurement only, for C41: with `--ab portcharge` the run alternates 100 and PCT every 20 s, recompiling nothing, so the charge's effect on speed is readable apart from the work a port removes |
| `--gpbulk on\|off` | pipe bursts copied whole (on) |
| `--ringbatch on\|off` | the pipe's bursts batched into GX-thread ring records of up to 512 bytes (on) |
| `--dblframe on\|off` | overlapped GPU frames: citro3d's frame stays open and a frame waits only for the GPU to finish the one before last (on) |
| `--shdwmemo on\|off` | the ShdwDraw walk memo: repeated shadow walks replayed from a record (on; `# shdw memo 10s`, `# shdw memo misses 10s`) |
| `--shdwtri on\|off`, `--trifast on\|off`, `--shdwcb on\|off` | ShdwDraw's triangle path as a native stand-in (on; `# shdw tri 10s`), in its fast form (on; off: the PSVEC-at-a-time form), and the shadow callback natively too (on; `# shdw callback 10s`) |
| `--psflush on\|off`, `--psxyz on\|off`, `--psgx on\|off`, `--psoff PREFIX,..` | groups of the translated SDK stand-ins (hle_ps.c): __GXSendFlushPrim (on), the cXyz operators (off, a loss), the GX register setters (off, a loss); --psoff keeps the named ones off |
| `--fcache`, `--ind2`, `--vin`, `--rotmask` `on\|off` | JIT and vertex-record options, all on since 10-19 (separate runs; MEASUREMENTS.md) |
| `--pmu-split on\|hle\|fast\|empty\|port\|portcommit\|cb\|path` | performance counters around compiled code, the ShdwDraw stand-in, its fast cells, an empty bracket, the J2D port's two halves, the native shadow callback, or each triangle path run (`# pmu split 10s`, per-run counts; a bracket costs ~1,100 cycles) |
| `--probe-faults on`, `--probe-mirror on` | the fault-delivery probe; Luma's physical-memory mirror and the L2C-310's registers (read only) |
| `--trace-follow on\|off`, `--trace-side on\|off\|MASK`, `--trace-len N`, `--link on\|off` | JIT block shape |
| `--profile on` | hot guest code, guest stacks, core 0's samples by guest thread, the GX thread's position and the decoder's command classes (`# hot guest code`, `# guest stacks`, `# guest threads`, `# gx thread where`, `# gx decode`). Every run also reports `# joint 10s` (core 0 / GX thread / vertex worker states sampled together: who waits on whom) and `# new frames` / `# new frames hist` (real frames a second; real gaps and VI fields between XFB copies) |
| `--fuzz CASES [SEED]` | the JIT differential fuzzer (tests/jit/jit_fuzz.c) on the console's own CPU |

`python3 core/tools/port_cost.py core/out/relay_<job>.csv` says what each native port costs core 0 (calls a second,
microseconds a call, share of a core), and `python3 core/tools/clock_split.py ...` where the run's emulated clock came
from (instructions really run, stand-in and port charges, idle and polling fills) with each field's slack.
`python3 core/tools/jitbench.py core/tests/jit/bench/*.S --cycles --disasm` needs no console at all: it runs
hand-written guest loops through the real JIT under qemu and reports hot ARM words a guest instruction.

Score separate whole runs (how defaults are decided now) with `python3 core/tools/runs.py core/out/relay_<job>.csv ...`
(gameplay: settled speed, fields and real frames a second, the VI fields per picture) and
`python3 core/tools/title_sections.py core/out/relay_<job>.csv ...` (tp850: cutscene, later stretch, movie). Fetch a
finished job's log first with `bash core/tools/relay.sh log <job>`. The scripted Ordon walk's `--press` list:
`SUP@40-159,SRIGHT@160-279,SDOWN@280-399,SLEFT@400-519` repeated every 480 fields to 2919 (the jobs in MEASUREMENTS.md
since 10-19 use exactly that). Score an A/B with `python3 core/tools/ab_phases.py core/out/relay_<job>.csv <column> 60 <on value>` (each phase
against its neighbours, from 60 s; pooled means showed phantom gains), or `core/tools/ab_split.py --col <column>` for
the tp850 state. `core/tools/perf_heavy.py --by <column>` gives means and standard errors over the heavy rows.
ab_phases.py drops each phase's first 12 s (a switch that flushes compiled code stalls); A/Bs that flush compiled code
still read ~2% low for the on arm in the heavy area, so compare those builds with separate whole runs instead: in the
heavy area (no `--press`) two runs of one build agree to ~0.1% (jobs 548-555). The scripted Ordon walk repeats
exactly for one setting, so its runs compare too — **but not across a change that shifts the guest's timeline** (a
port's or stand-in's charge, event timing): the presses are applied when the guest polls the pad, so a moved clock
diverts the walk and the arms render different scenes (C41). For those, use the heavy area or verify the scene
(`draws_s / (fields_s / 2)` at matched `emu_s`).

## Save states

`System` and `GXCore` have the same layout in the qemu ARM build and the 3DS build (same ARM EABI), so a state written
under qemu (`gcemu <disc> --jit --save-state 850:<path>`) loads on the console. A build that changes either size
refuses old states.

## Protocol

Every request carries the header `x-relay-token: <token>` (401 otherwise). Replies to the console are plain
`key value` lines, so its C client needs no JSON parser.

| Method and path | Who | Request | Reply |
|---|---|---|---|
| `PUT /build?note=TEXT` | PC | the `.3dsx` | `build <id>` and `bytes <n>`; id = first 16 hex digits of its sha256 |
| `GET /build/<id>` | console | (`Accept-Encoding: gzip` honoured) | the `.3dsx`; header `X-Raw-Length` |
| `POST /jobs` | PC | JSON `{"args": "...", "build": "<id>"}` | `job <n>` |
| `GET /jobs` | PC | | the last 20 jobs: `id status build created args`, tab-separated, newest first |
| `POST /jobs/<n>/cancel` | PC | | `ok` (only a queued job changes) |
| `GET /next` | console | | `job <n>`, `build <id or ->`, `args <...>`; 204 when nothing is queued (claims the oldest queued job) |
| `POST /jobs/<n>/cmd` | PC | one line of text | `queued <count>` |
| `POST /jobs/<n>/log?seq=<k>` | console | a chunk of the run's log | `ok`, then `cmd <text>` for each pending command; a chunk with the same seq replaces the old one |
| `GET /jobs/<n>/log?after=<k>` | PC | | chunks with seq > k, concatenated; headers `X-Relay-Last` (last seq sent) and `X-Relay-Status` |
| `POST /jobs/<n>/frame?n=<k>` | console | a BMP | `ok` |
| `GET /jobs/<n>/frames`, `GET /jobs/<n>/frame/<kkkk>.bmp` | PC | | frame names, one per line; one frame |
| `POST /jobs/<n>/done` | console | `done`, `failed` or `stopped` | `ok` |
