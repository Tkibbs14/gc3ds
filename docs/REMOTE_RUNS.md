# Remote runs: testing on the 2DS from anywhere

The 2DS and the PC never need to be on the same network. Builds, jobs and results go through a small relay on the
owner's server (nakama-backend, `src/routes/devRelay.js` on `main`, `/api/devrelay`):

```
PC  core/tools/relay.sh ──HTTPS──►  relay (builds, job queue, logs)  ◄──HTTPS──  2DS  gc3ds "Remote runs" (host3ds/relay.c)
```

## Everyday use (PC)

```bash
bash core/host3ds/build.sh                         # build gc3ds.3dsx
bash core/tools/relay.sh run --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 300 --ab traces
bash core/tools/relay.sh queue <options>           # queue without following (prints the job id)
bash core/tools/relay.sh log <job>                 # follow a job into core/out/relay_<job>.csv
bash core/tools/relay.sh jobs                      # the last 20 jobs and their status
bash core/tools/relay.sh cancel <job>              # a job still queued
```

`run` / `queue` upload the current `gc3ds.3dsx` if the relay does not have it (builds are named by the first 16 hex
digits of their sha256) and tie the job to it.

## On the 2DS

- `sdmc:/gc3ds/relay.txt` holds `url ...`, `token ...` and `auto 1`; gc3ds then enters Remote runs 3 s after
  launch (B for the menu; menu key L enters it by hand). It polls every 10 s and runs jobs one after another.
- A job for a build the 2DS does not have: it downloads the build over its own `.3dsx` (RomFS is closed first, since it
  keeps that file open), keeps the job in `sdmc:/gc3ds/relay_pending.txt` and asks for one restart (START, then open
  gc3ds again); the job runs when Remote runs starts.
- The 2DS needs internet: phone or laptop hotspot, or home Wi-Fi (many work networks isolate devices or need a
  sign-in page the 3DS cannot use). Keep the lid open: sleep drops Wi-Fi.
- **Never queue jobs for a build whose self-update code is broken:** the 2DS claims each one, fails the download and
  marks it failed. If the card's build cannot install builds, copy the new `gc3ds.3dsx` to `sdmc:/3ds/` by card once.

## Job options (also 3dslink arguments: `3dslink -a <ip> gc3ds.3dsx -- <options>`)

| Option | Meaning |
|---|---|
| `--game SUBSTR` | the first program whose path contains SUBSTR (case-insensitive) |
| `--state PATH` | load a save state right after boot |
| `--save-state FIELD:PATH` | save the machine when the VI reaches FIELD |
| `--seconds N` | end after N real seconds (relay jobs default to 600) |
| `--ab traces\|hle\|none` | which setting alternates every 20 s (`traces` / `hle` columns in perf.csv) |
| `--traces on\|off`, `--hle on\|off` | start values (both off by default; `--ab none` is the default too) |
| `--trace-follow on\|off` | traces follow unconditional `b` |
| `--trace-side on\|off\|MASK` | side exits: 1 `bc` on a CR bit, 2 `bc` with CTR, 4 conditional `blr`/`bctr`, 8 fused flags |
| `--trace-len N` | block length limit |
| `--link on\|off` | block linking |
| `--fuzz CASES [SEED]` | the JIT differential fuzzer (tests/jit/jit_fuzz.c) on the 2DS's own CPU, at the next start |

## Save states

`System` is 0x6b78 bytes and `GXCore` 0x4a68 in both the qemu ARM build and the 3DS build (same ARM EABI), so a state
written under qemu (`gcemu ... --jit --save-state 850:<path>`) loads on the 2DS. A build that changes either size refuses
old states; make a new one (qemu, or a 2DS job with `--save-state`). `tp850.state` is TP just before the Link/Epona
cutscene.

## Access and recovery

- `core/host3ds/relay.txt` (PC) and `sdmc:/gc3ds/relay.txt` (card) hold the token; both are outside git. A copy is kept
  at `C:\Users\tyler\Nakama-Backups\gc3ds-relay.txt`. The relay stores only the token's sha256 (`devrelay_tokens`).
- A new token: generate 32 random bytes as hex, insert its sha256 into `devrelay_tokens`, write the new relay.txt to the
  PC and the card.
- Relay side: tables `devrelay_tokens`, `devrelay_builds`, `devrelay_jobs`, `devrelay_log` (nakama-backend
  `sql/2026-10-12_devrelay.sql`), builds in the private storage bucket `devrelay`. If the route ever disappears from the
  backend's `main` (for example a branch pushed over it), restore `src/routes/devRelay.js` and its one `app.use` line
  in `src/index.js` from commit 527c5b4.
