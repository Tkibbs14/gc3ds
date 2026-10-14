# Ahead-of-time translation (redesign candidate C12)

Status (2026-10-14): **built and exact on the PC; first 2DS A/B pending.**

## What it is

`core/tools/aot/ppc2c.py` translates a game's main program (main.dol) from PowerPC into C on the PC, from the user's
own disc. The output goes to `core/src/aotgen/`, which is gitignored and never committed or distributed, and is
compiled into the emulator. `core/src/cpu/aot.c` runs it.

- **One C function per guest function.** Each function has entries at its start, after every call, at every loop
  head, and at every address that any `bl` in the program targets (`__save_gpr` / `__restore_gpr` are entered part
  way in).
- **Guest registers live in C locals** for the whole function: GPRs, FPR lanes as doubles, the CR as eight 4-bit
  fields, LR, CTR and XER. They are loaded at entry and stored at every exit.
- **Branches inside the function are `goto`s**, and `bctr` jump tables become `switch` statements over the
  function's labels.
- **Calls, returns and branches out of the function leave**, and `aot_run` looks up the next function. Polling
  loops and `b .` also leave, so the dispatcher can skip them.
- **Memory:** MEM1 accesses are inline (a range and alignment check, then a byte swap). Everything else takes a slow
  path through the interpreter's memory functions, at the exact guest time: MMIO, the write-gather pipe, unaligned
  accesses and exceptions.
- **Floating point follows the JIT's fast-float model.** Multiply-adds round the product first, and FPSCR keeps only
  fcmp's FPCC. A per-function analysis of which lanes hold exact singles removes the 25-bit cut and gives the fast
  `stfs` / `psq_st` conversion, as OPT_KSTORE does.
- **Untranslated instructions run in the interpreter, one at a time** (`aot_interp`), with every register stored
  before and reloaded after. Examples: `mtspr` to non-LR/CTR/XER registers, `mftb`, `sc`, `rfi`, cache operations,
  `mtfsf`, record and overflow forms of float instructions.
- **Validity:** a function runs only while an FNV-64 hash of its bytes in RAM matches the translation. `icbi` and
  `jit_invalidate_all` reset the check. Translated code also runs only in the standard machine state
  (`aot_state_ok`: translation on, FP and paired singles on, round to nearest).
- **Where it plugs in:** the JIT dispatcher (`jit_entry`) and the interpreter loop (`gekko_run`) call `aot_run` when
  the PC is an entry. A JIT exit to an entry is never linked, so it always comes back to the dispatcher.

## How it was checked (PC, Twilight Princess from field 900)

- **`AOT=1 AOT_CHECK=1`** compares every call with the interpreter run from the same state: GPRs, CR, XER, LR, CTR,
  MSR, PC, the instruction count and every byte either side stored. In check mode, slow paths and fallbacks leave
  before their instruction so nothing is replayed twice.
  - Over 120 fields: **10.7 M calls, 168 M guest instructions, 0 differences.**
  - **2,730 calls differed only in an FPR**, all by 1 ulp in double-precision `fmadd` (libm sin/cos at 8036b9f0).
    That is the fast-float model, the same as the JIT.
- **`AOT=1` with check off:** the last frame after 120 fields is **byte-identical** to the interpreter's
  (md5 09585a69...).
- **Coverage:** 189.7 M of 196.2 M executed guest instructions ran translated (**96.7%**). What remains:
  - REL module code (~2%);
  - real-mode interrupt handlers;
  - OS context switches (`rfi`);
  - `DCInvalidateRange` (`dcbi`);
  - one audio mixing loop.
- **Bug found and the guard that stays:** `lmw` / `stmw` initialised the inner `ea_` from itself (C scoping). Check
  mode missed it because the garbage address went down a slow path, which check mode hands to the interpreter, and
  it was found by bisecting function ranges (`AOT_RANGE`). Two guards were added: the generated code is compiled with
  `-Werror=shadow,init-self,uninitialized`, and check mode now reports any aligned MEM1 address that reaches a slow
  path.

Debug switches (PC): `AOT_NOSLOW`, `AOT_NOFB` (slow paths / fallbacks leave instead), `AOT_RANGE=lo-hi` (only those
functions), `AOT_SLOWLOG`. `PCHIST=file` writes the interpreter's executed-instruction histogram, used to pick
functions and to find what is still interpreted.

## Regenerate

```bash
bash core/tools/tp_run.sh ...    # with PCHIST=/root/aot/hist.bin, over a representative stretch
python3 core/tools/aot/ppc2c.py --dol <extracted>/sys/main.dol --symbols <zeldaret/tp config/GZ2E01/symbols.txt> \
    --hist /root/aot/hist.bin --out core/src/aotgen --files 16
make -C core && bash core/host3ds/build.sh -j8
```

On the 3DS: `--aot on`, `--ab aot`, the live command `set aot 0|1`, the perf.csv column `aot`, and the
`# aot 10s` line (share translated, calls, runs, fallbacks and slow accesses per second).

## Status and next steps (2026-10-14)

Done, each gated by the PC check (only the known `atan2` stack-store difference), the identical PC frame and the ARM
check under qemu:

1. **Direct calls** with per-site liveness: a call flushes only dirty registers and reloads only the volatile ones read
   after it. Every written register has a dirty bit, so flushes store only what changed.
2. **The write-gather pipe:** a store whose address the compiler sees is `0xCC008000` appends to the burst buffer at
   the site; other pipe stores append inside the slow helper.
3. **No symbol map needed:** functions are found from `bl` targets and prologues (13,704 in TP's DOL).
4. **Register pressure:** slow paths call their helper through `aot_vcall`, which keeps r1-r3 and d0-d7, so hot paths
   keep guest values in all sixteen VFP registers; psq_l's two paths meet as floats.
5. **Per-entry loads:** each entry loads only what is live into it.
6. **Cache instructions inline:** `dcbst` / `dcbf` / `dcbi` / `dcbz` on MEM1 as the JIT does them (only the page stamp
   and dcbz's zeroes remain). Fallbacks over 120 fields: 1,008,067 -> 28,397.

Measured under qemu (TP 850-870, ARM instructions per guest instruction inside translated functions, with the AOT_PROF
counters): 10.26 at the start of the day, 9.37 after items 4-5. The JIT is 8.04 inside its buffer, but that leaves out
its dispatcher and helpers, so the 2DS A/B (`--ab aot`) is the measure that counts.

Open, in order:

1. **2DS A/B** of the current build (job 140 or later).
2. **Code size** (the .3dsx is 16 MB with 625 functions; a 28 MB build left too little heap): `-Os` for cold
   functions, smaller slow-path sites, then more coverage.
3. **REL modules:** N64Recomp's relocation markers, or hashing modules after they are relocated at their (usually fixed)
   load addresses.
4. **Translation from the user's own disc** (James, 2026-10-14). Open design question: a PC tool that builds a module
   gc3ds loads from the SD card, or translation on the 2DS itself (no C compiler there: the JIT's back end run ahead of
   time over whole functions, cached on the card).
5. **Code quality:** the matrix functions (PSMTXConcat 15 ARM per guest instruction) are bound by double-precision
   multiply-adds and register pressure; paired-single register saves in prologues convert both lanes by bits; CR fields
   are materialized at every exit.
6. **GX side:** the combiner plan is built once per material (in the build); geometry-shader quads and persistent
   vertex buffers next.

## Prior art read (Haiku research, 2026-10-14; READMEs only, not the code)

- N64Recomp: relocations and indirect calls via `LOOKUP_FUNC`.
- XenonRecomp: jump tables to `switch`; registers as locals after a working build; volatile byte-swapped accesses.
- ReXGlue SDK.
- sp00nznet/ww (a Wind Waker static recompilation): OS replaced by native code; 8,148 functions.
- sp00nznet/ikaruga (gcrecomp): symbol-free, 5,138 functions.
- PS2Recomp.
- Mario Kart 64 3DS port and Minish Cap 3DS port (EstebanPdN).
- None reports a speedup over a JIT with numbers, and none targets a 32-bit ARM host.
