# Outside research, 2026-10-20

Four web research passes (Haiku agents, each finding tagged sourced or inferred; the load-bearing ones checked here
against the primary source or the game's machine code). What they found that bears on gc3ds, and what it changes.

## 1. PowerPC-on-ARM JITs

- **Dolphin's ARMv7 JIT** was dropped from Android in 2015 (AArch64 only since). Its 2014 work rewrote ARM fastmem and
  kept guest FPRs in host registers across instructions ("sizable" gains, no numbers published). No ARMv7 phone
  benchmarks or paired-single design notes were found.
- **Thumb-2 is not available on the ARM11 MPCore** (ARMv6K: Thumb-1 only), so the "smaller code through Thumb-2" lever
  some ARM JITs use does not exist here.
- ARM11-class core facts (7-cpu, ARM1136-class SoC, not confirmed for MPCore): 128-entry direct-mapped BTAC, **3-entry
  return stack that predicts only unconditional returns**, 6-cycle mispredict, forward-not-taken / backward-taken static
  prediction. Consistent with our measured win from return stubs (`retstub`, +0.8-1.6%).
- No measured numbers for any 32-bit ARM PowerPC JIT were found. Our ~13 host cycles per guest instruction has no
  published comparison.

## 2. Decompilation and native ports

- **zeldaret/tp: the GameCube code is fully matching** (not every unit linked; Wii and debug builds not matching).
- **Dusklight (TwilitRealm)**: a native PC reimplementation of Twilight Princess built on the decomp, running its GX
  through **Aurora** (encounter/aurora, the GameCube / Wii compatibility layer from Metaforce, on WebGPU / Dawn).
  This is the decomp + reimplemented-GX model at full game scale: the same model as gc3ds's parked native mode
  (memory: rendered TP at 5-7 fps, parked by the owner for the emulator first).
- Static recompilers for GameCube code exist but are early (DolRecomp, sp00nznet gcrecomp, a Rust GCRecomp); the
  Xbox 360 precedent is XenonRecomp (Sonic Unleashed).
- **No emulator HLEs JSystem.** Dolphin has generic OS-function HLE hooks only. Our J2D / J3D ports are new ground.
- Closest published precedent for our ports: ApeEscapeRecomp 0.5.0 replaced a renderer routine with native code
  checked over 256 ABI-boundary comparisons, ~27-28 -> ~30 fps in places (the same model as `--port check`).

## 3. Frame skipping and speed hacks

- No emulator was found that skips the guest's own render work without visible artifacts. PPSSPP's "skip buffer
  effects" (off-screen rendering) warns of severe artifacts, the same class as our frame-skip fade glitches; Dolphin
  removed frameskip as unreliable (Kirby: sky-blue frames from EFB copies not made every frame).
- **Dolphin's Twilight Princess "Hyrule Field Speed Hack" (GZ2E01.ini `[OnFrame]`) NOPs four calls at 0x8003D50C,
  0x8003D528, 0x8003D540, 0x8003D55C**: checked against the game here, they are GXSetTevColorIn, GXSetTevColorOp,
  GXSetTevAlphaIn and GXSetTevAlphaOp in dRenderingFDAmap_c::renderingDecoration (the minimap's line drawing, 0x8003D3C0).
  It changes how the minimap draws; that function is 0.45-0.83% of our core 0 (jobs 630-631), so it is not a lever for
  gc3ds. (A fetched summary of the file also listed further NOP addresses that could not be confirmed.)
- Idle-loop skipping (Dolphin) matters only when the game idles; TP's core 0 never does in our scenes.

## 4. 3DS homebrew

- Core 1 is usable only after APT:SetApplicationCpuTimeLimit (5-89%, the ExHeader sets the maximum; cannot go back to
  0); core 2 needs ExHeader kernel flag 0x2000 outside the BASE region; core 3 is refused outside BASE. gc3ds already
  runs its GX thread on core 2 and the vertex worker on core 3.
- The kernel is preemptive and priority-based without timeslices: a spinning thread starves equal- or lower-priority
  threads on its core.
- Executable memory comes only through svcControlProcessMemory (with a real process handle), which gc3ds's JIT buffer
  already uses. svcFlushProcessDataCache / svcInvalidateProcessDataCache exist; no 3DS JIT documents its sequence.
- Other 3DS dynarecs (gpSP, PCSX ReARMed) publish no dynarec speed figures; their gains came from renderers.
- Not found anywhere: measured FCRAM bandwidth (3.2 GB/s datasheet peak only), SCU coherency or false-sharing costs.
  Our own probes (MEASUREMENTS.md "Memory hierarchy") remain the only measurements.

## What it changes

- Nothing found outranks the measured plan in HANDOFF.md: the J2D pane walk as one port, then J3D drawing.
- The strongest outside signal is strategic: a native build of the game's code (Dusklight's model, gc3ds's parked
  native mode) is the only route to full speed that does not pay the JIT's ~13 cycles per guest instruction. It is the
  owner's call when to revisit it.
