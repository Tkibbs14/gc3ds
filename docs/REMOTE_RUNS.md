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
bash core/tools/relay.sh jobs                      # the last 20 jobs and their status
bash core/tools/relay.sh cancel <job>              # a job still queued
bash core/tools/relay.sh cmd <job> "set opt 1"     # a live command for the running job (see host3ds/main.c apply_cmd)
bash core/tools/relay.sh frames <job>              # the job's top-screen frames (BMP, PNG when ImageMagick is there)
```

`run` / `queue` upload the current `gc3ds.3dsx` if the relay does not have it (builds are named by the first 16 hex
digits of their sha256) and tie the job to it; `queue` refuses to queue when the upload failed.

Live commands (`cmd`, taken within ~2 s): `shot` (a top-screen frame, then `frames`), `stop`, `set NAME VALUE` (opt,
sched, merge, vtrim, dlcache, shapes, ...), and for steering a game from the PC: `press BUTTON N` (A, B, X, Y, Z, L, R,
START, UP, DOWN, LEFT, RIGHT, held N fields, default 6), `stick X Y N` (the main stick, 0-255 with 128 the centre, held
N fields) and `save sdmc:/gc3ds/states/NAME.state` (a save state at the next field). Menus answer the stick, not the
d-pad. That is how gameplay benchmark states are made: queue a long run, then press / shot / look until the game is
where you want it, and save. `--press` takes the same buttons plus `SLEFT` / `SRIGHT` / `SUP` / `SDOWN` (the stick to
an edge) for scripted runs; its fields count from the run's start.

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
| `--ab NAME` | alternate one setting every 20 s within the run (its column in perf.csv says which half each row is in). The names are the `abXxx` fields in `host3ds/main.c`'s `RunOpts` (parsed at `--ab`); among them `opt`, `kstore`, `merge`, `hle`, `ps`, `psgx`, `psflush`, `rawdraw`, `rawstate`, `mips`, `smalltex`, `tevver`, `deflit`, `shlight`, `vscache`, `blacklit`, `vramtex`, `cmdbuf`, `qbig`, `splitverts`, `l2pf`, `gxdiag`, `profclock`, `gpudiag`, `texdiv`, `gfskip` |
| `--opt`, `--kstore`, `--merge`, `--hle`, `--mips`, `--deflit`, `--shlight`, `--vramtex`, ... `on\|off`; `--smalltex N`, `--splitverts N`, `--l2pf N`, `--gpudiag BITS`, `--texdiv N` | start values (see `host3ds/main.c` `opts_default` for the defaults) |
| `--gfskip ADDR`, `--gfskip-n N`, `--gfskip-ok PTR+OFF=VAL,..` | guest frame skip: the game's renderer at ADDR every Nth call, only while the guards hold (TP: `--gfskip 8000A604 --gfskip-ok 80450BC8+4=1,80450BC8+20=ffffffff`) |
| `--memo ADDR` | function memoization probe on the function at ADDR (measurement only; `# memo 10s`) |
| `--probe-faults on`, `--probe-mirror on` | the fault-delivery probe; Luma's physical-memory mirror and the L2C-310's registers (read only) |
| `--trace-follow on\|off`, `--trace-side on\|off\|MASK`, `--trace-len N`, `--link on\|off` | JIT block shape |
| `--profile on` | hot guest code, guest stacks, the GX thread's position and the decoder's command classes (`# hot guest code`, `# guest stacks`, `# gx thread where`, `# gx decode`) |
| `--fuzz CASES [SEED]` | the JIT differential fuzzer (tests/jit/jit_fuzz.c) on the console's own CPU |

Score an A/B with `python3 core/tools/ab_phases.py core/out/relay_<job>.csv <column> 60 <on value>` (each phase
against its neighbours, from 60 s; pooled means showed phantom gains), or `core/tools/ab_split.py --col <column>` for
the tp850 state. `core/tools/perf_heavy.py --by <column>` gives means and standard errors over the heavy rows. Only
compare within one run: the same build varies ~5% between runs.

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
