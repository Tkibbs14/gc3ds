# GameCube / Wii on New 3DS — hardware architecture reference

Purpose: the hardware facts a GameCube/Wii JIT emulator for the New 3DS / New 2DS XL has to design around. Every
number carries a source link (reference list at the end). Measurements we took ourselves live in
[FINDINGS.md](FINDINGS.md) and are quoted here as **[measured]**, not re-derived. Statements marked *(inference)* are
our reasoning, not a source; **unverified** means no source was found in this pass.

Sourcing conventions: Dolphin source is cited at `master` as fetched on 2026-10-10; YAGCD, WiiBrew and 3dbrew are
community wikis; ARM and IBM manuals are primary. Where two sources disagree, both are given.

---

## 1. GameCube

### 1.1 Gekko CPU (IBM PowerPC 750 derivative)

| Property | Value | Source |
|---|---|---|
| Core clock | 486 MHz (Nintendo's sheet says 485 MHz) | [yagcd2], [dolST], [ninGC] |
| Issue | superscalar, "3 issue (2 plus branch folding)", BHT + BTIC | [hcGekko] |
| L1 I-cache | 32 KB, 8-way | [yagcd2], [hcGekko] |
| L1 D-cache | 32 KB, 8-way; 16 KB of it lockable as scratchpad | [hcGekko], [750cl] HID2 |
| L2 | 256 KB unified, 2-way | [yagcd2], [hcGekko] |
| Integer units | two fixed-point units (FXU1/FXU2) | [hcGekko] |
| FPRs | 32 × 64-bit; each holds one double or two singles (PS0/PS1) | [hcGekko], [yagcd3] |
| Peak FP | two multiply-adds per cycle → 1.9 GFLOPS | [hcGekko] |
| Dhrystone | 1125 DMIPS | [ninGC] |
| Bus to Flipper | 162 MHz, 64-bit, 1.3 GB/s peak | [ninGC], [dolST] |

**Paired singles.** The FPU performs one double-precision op or two single-precision ops per cycle; there are
lane-crossing merge and sum instructions, and one FPSCR merges status from both lanes [hcGekko]. Paired-single
instructions are illegal unless HID2[PSE] is set [750cl]; YAGCD names HID2 bit 2 PSE and bit 0 LSQE [yagcd3].

**Quantized load/store (GQRs).** `psq_l`/`psq_st` convert 8/16-bit signed/unsigned integers to/from single floats
with a scale, giving 2:1 and 4:1 compression and a claimed 5.2 GB/s effective bus bandwidth [hcGekko]. Eight GQRs
(SPR 912–919) each hold LD_TYPE/LD_SCALE (6-bit) and ST_TYPE/ST_SCALE; the instruction's 3-bit I field picks the GQR
[750cl] §2.1.2.11. Type codes: 0 = float (no conversion), 1–3 reserved, 4 = u8, 5 = u16, 6 = s8, 7 = s16 [750cl] Table
2-20 (the PDF text extraction misaligns that table; the ordering here is the conventional reading — verify against the
PDF page 77).

**FPU timing** (750CL Instruction Timing tables, pp. 241–242) [750cl]: almost all FP and paired-single arithmetic
(`fadd(s)`, `fmul`, `fmadd`, `ps_add`, `ps_mul`, `ps_madd`, `ps_merge*`, `ps_sum*`) is fully pipelined with a
`1-1-1` (3-cycle) stage pattern; several single-precision multiply-class forms (`fmuls`, `fnmsubs`, `fres`, `frsp`,
`fsel`, `ps_res`, `ps_rsqrte`) show `2-1-1`; divides are not pipelined (17 cycles for `fdivs`/`ps_div`-class, 31 for
`fdiv`, by the column reading). The text extraction shifted the cycles column, so treat the per-mnemonic assignment as
**needs re-check against the PDF**; the shape (3-cycle pipelined FMA, ~17/31-cycle divide) is solid.

**Locked cache + DMA.** HID2[LCE]=1 splits the D-cache into 16 KB normal + 16 KB locked; `dcbz_l` allocates lines in
the locked half [750cl] Table 2-7, [hcGekko]. DMA is issued through DMAU/DMAL (SPR 922/923) [750cl]; the queue holds
15 entries, each moving up to 4 KB, completion is polled [hcGekko]. HID2 also reports DMA queue length and sticky
overflow/`dcbz_l`-hit error bits [750cl]. YAGCD maps 0xE0000000–0xE0003FFF (16 KB) and labels it "L2 Cache" [yagcd4];
this is the locked-cache window (YAGCD's label disagrees with the IBM manual's "locked L1" description).

**Write-gather pipe.** WPAR (SPR 921) holds the cache-line-aligned physical address of gathered non-cacheable stores;
writing WPAR resets the pipe and discards buffered data; BNE reports pending data; a store with non-zero low 5 address
bits gathers "incorrect data" [750cl] §2.1.2.12. A 128-byte FIFO gathers byte/half/word/double stores and sends
32-byte bursts in parallel with execution, designed for streaming graphics data [hcGekko]. On the GameCube it is
mapped at 0xCC008000 and feeds the GX FIFO [yagcd5].

**MMU/BATs.** Games almost all run on BAT mappings of 0x80000000 (cached) and 0xC0000000 (uncached) over the same
24 MB [yagcd4]. A minority use page tables: Dolphin's "MMU speedhack" (default since 2014) redirects games using
Nintendo's ARAM library through an extra BAT, and in 2016 Dolphin counted 21 titles that need more than the speedhack
[dolFinal]. Rogue Squadron II/III were the hard cases; Dolphin's "Faster MMU 2" gave ~80% there [dolMMU15], and
page-table fastmem only arrived in release 2603 [dol2603].

### 1.2 Memory

| Region | Size / speed | Address | Source |
|---|---|---|---|
| Main RAM (1T-SRAM, 2 × 12 MB) | 24 MB, 324 MHz, 2.6 GB/s peak, ~10 ns sustained latency | 0x80000000 cached / 0xC0000000 uncached (phys 0x00000000–0x017FFFFF) | [ninGC], [yagcd2], [yagcd4], [dolST] |
| ARAM ("A-memory", DRAM) | 16 MB, 81 MHz, 8-bit bus, 81 MB/s | not CPU-mapped; DMA only (AR_DMA_* at 0xCC005020–0xCC00502A) | [ninGC], [wpGC], [yagcd5] |
| Embedded framebuffer (EFB) | ~2 MB 1T-SRAM, 6.2 ns | GPU-internal | [ninGC] |
| Texture cache (TMEM) | ~1 MB, 6.2 ns, 10.4 GB/s peak read; Dolphin `TMEM_SIZE = 1024*1024`, 32-byte lines | GPU-internal | [ninGC], [dolTD] |
| IPL ROM | 1 MB, mapped at 0xFFF00000 at boot | 0xFFF00000 | [yagcd4] |

### 1.3 Flipper GPU

| Property | Value | Source |
|---|---|---|
| Clock | 162 MHz | [ninGC] |
| Pipelines | 1 vertex pipeline, 4 pixel pipelines (1 TMU each), 4 ROPs | [wpGC] |
| Fill rate | 648 Mpixel/s | [wpGC] |
| Polygons | 6–12 M/s "actual game conditions" (Nintendo); 60 M raw | [ninGC], [wpGC] |
| Hardware lights | 8 | [ninGC], [dolXF] `Light lights[8]` |
| TEV stages | up to 16 (`GenMode` stores count − 1) | [dolBP] |
| Texture coordinates / maps | 8 (`TCoordInfo texcoords[8]`) | [dolBP] |
| Indirect texturing | 4 indirect stages (Wikipedia); Dolphin stores `numindstages` in a 3-bit field | [wpGC], [dolBP] |
| Texture decompression | S3TC in hardware (CMPR) | [ninGC] |

**Command processor / FIFO.** The CPU streams GX commands through the write-gather pipe at 0xCC008000; CP registers
at 0xCC000000 hold the FIFO base (0xCC000020), end (0xCC000024), high/low watermarks (0xCC000028/2C) that drive the CP
interrupt [yagcd5]; PI_FIFO_WP is at 0xCC003014 [wbPI]. GP opcodes include 0x80 QUADS and 0x90 TRIANGLES [yagcd5].

**XF (transform unit).** XF memory: 256 floats of position matrices (0x000–0x0FF), 96 of normal matrices
(0x400–0x45F), 256 of post-transform matrices (0x500–0x5FF), 8 lights (0x600–0x67F), registers 0x1000–0x1057; two
color channels; 8 texgen matrix slots [dolXF].

**TEV (BP registers).** Each stage computes `dest = (d OP lerp(a, b, c) + bias) * scale` with OP add/sub and scale
1/2/4/0.5; 16 color-input and 8 alpha-input selectors; konst colors via 8 KSel registers [dolBP]. Dolphin notes most
games use TEV "in a very basic way, often making only use of one processing stage" [dolUber] (via search excerpt).

**Texture formats** [dolTD]: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2 (palette, TLUT formats IA8,
RGB565, RGB5A3), CMPR (S3TC-family, 0xE).

**EFB.** Dolphin models the EFB as 640×528 (`EFB_WIDTH`, `EFB_HEIGHT`), max XFB 720×576, 24-bit depth [dolVC].
Pixel formats: RGB8_Z24, RGBA6_Z24, RGB565_Z16, Z24, Y8/U8/V8, YUV420 [dolBP]. The 3× multisample mode shrinks the
EFB to 640×264, so a full 640×480 frame must be rendered in two halves; "basically nothing uses it" (Dolphin dev,
forum) [dolAA].

**EFB copies and XFB.** An EFB copy moves the EFB (or a rectangle) to main RAM in a texture format (R4…RGBA8, Z
variants) [dolTD]; a copy with the XFB bit converts to YUV into the external framebuffer that VI scans out [dolXFB].
Dolphin's default "EFB copies to texture only" avoids the copy to RAM; it breaks any game whose CPU reads the copy
(developers estimated ~40% of games have something break with it, per forum/bug tracker discussion — approximate)
[dolEFB].

### 1.4 DSP and audio

| Property | Value | Source |
|---|---|---|
| Core | custom Macronix 16-bit DSP, big-endian | [ninGC], [wbDSP] |
| Clock | 81 MHz (GC), 121.5 MHz (Wii) | [ninGC], [wbDSP] |
| Memory | IRAM 8 KB, IROM 8 KB, DRAM 8 KB, DROM 4 KB | [wbDSP] |
| Voices | 64 simultaneous, ADPCM | [ninGC] |
| Output rate | AI selects 48 kHz or 32 kHz (AICR bit 6 / AFR) | [yagcd5] |

- **AX microcode** is the common one; Dolphin HLE reimplements it. GC AX processes 32 samples per ms, 5 ms per frame
  (160 samples); Wii AX processes 3 ms per call (96-sample max) [dolAX], [dolAXV]. A Dolphin bug thread observes a
  typical AX game writing 160 samples every 5 ms [dolI7241].
- Sample-rate conversion types: polyphase (coefficients live in DSP DROM), linear, none [dolAXV].
- ADPCM: 4-bit nibbles; a predictor/scale byte every 8 bytes (16 nibbles); 3-bit index selects one of 8 coefficient
  pairs; PCM8/PCM16 also supported by the accelerator [dolACC].
- HLE vs LLE: LLE runs the real microcode and stalls with the emulated CPU; HLE need not stay in lock-step (Dolphin
  forum, developer statement) [dolDSPf]. Claim that most GC games mix at 32 kHz with ~5 titles at 48 kHz: Dolphin
  forum, 2015 — **unverified**, link not pinned.
- DSP mailboxes: CPU→DSP 0xCC005000/02, DSP→CPU 0xCC005004/06, bit 31 = status [yagcd5].

### 1.5 Hardware register blocks (base 0xCC000000) [yagcd5]

| Block | Base | Size | Access |
|---|---|---|---|
| CP command processor | 0xCC000000 | 0x80 | 16-bit |
| PE pixel engine | 0xCC001000 | 0x100 | 16-bit |
| VI video interface | 0xCC002000 | 0x100 | 32-bit |
| PI processor interface (interrupts, FIFO) | 0xCC003000 | 0x100 | 32-bit |
| MI memory interface | 0xCC004000 | 0x80 | 32-bit |
| DSP interface (+ARAM DMA, AI DMA) | 0xCC005000 | 0x200 | 16-bit |
| DI DVD interface | 0xCC006000 | 0x40 | 32-bit |
| SI serial (controllers) | 0xCC006400 | 0x100 | 32-bit |
| EXI external (memory cards, IPL/RTC) | 0xCC006800 | 0x40 | 32-bit |
| AI audio streaming | 0xCC006C00 | 0x20 | 32-bit |
| Write-gather pipe (GX FIFO) | 0xCC008000 | 32 B | any |

PI interrupt cause bits (0xCC003000): 0 GP error, 1 reset, 2 DVD, 3 SI, 4 EXI, 5 AI, 6 DSP, 7 MI, 8 VI, 9 PE token,
10 PE finish, 11 CP FIFO, 12 debug, 13 high-speed port, 14 Hollywood IRQs (Wii) [wbPI].

### 1.6 Disc

| Property | Value | Source |
|---|---|---|
| Media | 8 cm optical, ~1.5 GB | [ninGC] |
| Transfer | 16–25 Mbit/s (≈2–3 MB/s), CAV | [ninGC], [wpGC] |
| Average access | 128 ms | [ninGC] |
| Sector / ECC block | 2048 B sector; 16-sector (32 KiB) minimum drive read | [dolDVD] |
| Buffer → RAM | Dolphin models 32 MiB/s from drive buffer; 1 MiB read-ahead; 300 µs min command, 600 µs read start | [dolDVD] |

**GCM layout** [yagcd13]: boot.bin 0x000–0x43F (game code 0x00, maker 0x04, magic 0xC2339F3D at 0x1C, DOL offset
0x420, FST offset/size/max at 0x424/0x428/0x42C); bi2.bin at 0x440 (0x2000 B, loaded to 0x800000F4); apploader at
0x2440 (entry 0x10, size 0x14, trailer 0x18, code from 0x20 loaded to 0x81200000); FST = 12-byte entries plus string
table.

**DOL** [wbDOL]: big-endian 0x100-byte header; up to 7 text + 11 data sections (offsets, load addresses, sizes), BSS
address/size at 0xD8/0xDC, entry at 0xE0; sections and sizes 32-byte aligned.

### 1.7 Timing

- Clocks: Gekko 486, Gekko↔Flipper bus 162, Flipper↔1T-SRAM 324, ARAM/DSP 81 MHz [dolST].
- Timebase/decrementer = bus/4 = CPU/12, so Dolphin's `TIMER_RATIO = 12` (≈40.5 MHz on GC) [dolST].
- VI: four display-interrupt registers DI0–DI3 (0xCC002030–3C) with per-register enable and VCT/HCT position;
  top/bottom field XFB address TFBL/BFBL; 27 MHz video clock, 54 MHz in progressive [yagcd5]. Dolphin schedules VI by
  half-line [dolSTc].
- Dolphin's HLE IPC poll period is 1500 Hz, called "a pure assumption" in the source [dolSTc].

---

## 2. Wii differences

| Item | GameCube | Wii | Source |
|---|---|---|---|
| CPU | Gekko 486 MHz | Broadway 729 MHz (downclocks to 486), 90 nm; same 32/32 KB 8-way L1, 256 KB 2-way L2 | [wbBroadway] |
| Bus | 162 MHz | 243 MHz (162 in GC mode) | [wbBroadway], [dolST] |
| GPU | Flipper 162 MHz | Hollywood GPU 243 MHz, 3 MB embedded memory | [wbHW] |
| RAM | 24 MB 1T-SRAM | MEM1 24 MB 1T-SRAM + MEM2 64 MB GDDR3 (88 MB) | [wbHW] |
| DSP | 81 MHz | 121.5 MHz | [wbDSP] |
| Aux RAM | 16 MB ARAM | no ARAM (DSP uses MEM2) — **unverified** | — |
| Security/IO | none | Starlet ARM926EJ-S @ 243 MHz, 96 KB SRAM, runs IOS | [wbStarlet] |

Dolphin's timer header lists the Wii clocks (Broadway 729, bus 243, 1T-SRAM 486) as partly guesses [dolST].

**Memory map** [wbMap]:

| Region | Virtual | Physical |
|---|---|---|
| MEM1 cached / uncached | 0x80000000 / 0xC0000000, 24 MB | 0x00000000–0x017FFFFF |
| MEM2 cached / uncached | 0x90000000 / 0xD0000000, 64 MB | 0x10000000–0x13FFFFFF |
| Legacy (GC) registers | 0xCC000000 | 0x0C000000 |
| Hollywood registers (shared with Starlet) | 0xCD000000, 32 KB | 0x0D000000 |
| EFB | 0xC8000000, 3 MB | 0x08000000 |

IOS can lock part of MEM2 so Broadway cannot access it [wbStarlet]; exact reserved range **unverified**.

**IPC.** Registers at 0x0D800000: HW_IPC_PPCMSG (+0), PPCCTRL (+4), ARMMSG (+8), ARMCTRL (+C). Protocol: PPC writes a
pointer to a 0x40-byte request into PPCMSG and sets X1; Starlet acks with Y2, completes with Y1 and the reply pointer
in ARMMSG; X2 = relaunch; interrupt #30 to PPC, #31 to Starlet [wbIPC]. Request types: open, close, read, write,
seek, ioctl, ioctlv (plus cmd 8 async reply) [wbIOS].

**What an emulator must HLE** (devices from [wbIOS]; selection is *(inference)*): `/dev/fs` (NAND filesystem: saves,
settings), `/dev/es` (title/ticket services, launching), `/dev/di` (disc reads, partition open/decrypt),
`/dev/usb/oh1` (internal USB → Bluetooth dongle → Wii Remotes), `/dev/stm` (power/reset events), `/dev/sdio` (SD),
`/dev/net/*` (most games tolerate failure), `/dev/aes`, `/dev/sha`.

**AES/SHA engines.** AES-128-CBC over 16-byte blocks via DMA, base 0x0D020000 [wbAES]; SHA-1 over 64-byte blocks via
DMA, base 0x0D030000, no padding in hardware [wbSHA].

**Disc encryption (structure only)** [wbDisc], [wbTicket]:
- Header like GC's for 0x400 bytes; Wii magic 0x5D1C9EA3 at 0x18; partition table at 0x40000 (offsets stored >> 2);
  region byte at 0x4E000.
- Partition: ticket (0x2A4 B) at +0; TMD, cert chain, H3 table (always 0x18000 B) and data offsets in the header;
  data normally at partition +0x20000.
- Data in 0x8000-byte clusters: 0x400 B encrypted hash area + 0x7C00 B encrypted user data. H0 = 31 SHA-1s (one per
  0x400 B of user data); H1 = 8 hashes (clusters in a subgroup); H2 = 8 hashes (subgroups in a group of 64 clusters);
  H3 table = hash of each group, unencrypted but signed.
- Hash area: AES-128-CBC, zero IV; user data: IV = bytes 0x3D0–0x3DF of the encrypted hash area.
- Title key: 16 bytes at ticket 0x1BF, encrypted with the common key selected by ticket 0x1F1, AES-CBC with IV = title
  ID (ticket 0x1DC) padded with 8 zero bytes. No key material is reproduced here.

**Wii Remote.** Broadcom BCM2042; HID over L2CAP PSM 0x11 (control) / 0x13 (data); report modes 0x30–0x3F; IR camera
tracks 4 points at 1024×768; ADXL330 accelerometer (±3 g); speaker 4-bit Yamaha ADPCM or 8-bit PCM; legacy PIN
pairing only [wbWiimote]. Report rate: **unverified** (commonly quoted 100 Hz not on the page).

**NAND.** 512 MiB; 2048-byte pages + 64 spare; 64-page (128 KiB) blocks; 4096 blocks; SFFS filesystem; per-console AES
encryption + HMAC [wbNAND].

---

## 3. New 3DS host

### 3.1 CPU: ARM11 MPCore

| Property | Value | Source |
|---|---|---|
| Cores | 4 (New 3DS), 2 (Old) | [3bHW] |
| Clock | up to 804 MHz; 268 MHz unless the title flag or Home Menu raises it; only ×3 via official kernel | [3bHW] |
| System tick | `SYSCLOCK_ARM11` = 268,111,856 Hz (16,756,991 × 16) — `svcGetSystemTick` counts at this rate even in 804 MHz mode | [ctruOsH] |
| Pipeline | 8 stages | [mpcore] §1.7 |
| L1 caches | configurable 16/32/64 KB each, 4-way, 8-word (32 B) lines; D-cache PIPT write-back, I-cache VIPT | [mpcore] §1.4, CP15 c0 |
| 3DS L1 size | **unverified** (16 KB commonly claimed; read the Cache Type Register to settle) | — |
| L2 (New 3DS only) | 2 MB, PL310/L2C-310 r3p3 at 0x17E10000 | [gbatek], [3bMem] |
| TLB | MicroTLBs backed by a 64-entry 2-way unified main TLB, 8 lockable; 4 KB/64 KB/1 MB/16 MB pages | [mpcore] |
| Store buffer | four 64-bit slots, merging | [mpcore] |
| Branch prediction | 128-entry BTAC (dynamic), static fallback, 3-entry return stack | [mpcore] |

**Branch costs** [mpcore] Table 15-11: folded dynamic prediction 0 cycles; not-folded 1; correct static taken 4;
mispredict 5–7; `BX lr` predicted by return stack 4, mispredicted 7, empty stack 5; **register-indirect branches are
never predicted** — taken `BX <reg>`/`BLX <reg>` cost 5–7. *(Inference: JIT block exits should be patched direct
`B`/`BL`; a `BX`-based dispatcher pays ≥5 cycles per exit, consistent with the 13→37 ns linked-vs-dispatcher
gap in [FINDINGS].)*

**Cores available to an application.**
- Core 0: app core, always usable. Core 1: system core; usable for one thread after `APT_SetAppCpuTimeLimit`
  [ctruThread]; range 5–89%, max set by exheader, cannot be set back to 0 once non-zero; >30% helps only on New 3DS
  [3bAPT], [3bMT]. **[measured]** 80% → ~78% usable.
- Core 2: New 3DS only; needs exheader kernel flag 0x2000 (bit 13, "access to CPU core 2") [ctruThread], [3bExh].
  **[measured]** fully parallel.
- Core 3: "Normal applications cannot create threads on this core" [ctruThread].
- Scheduler: strict priority, no time slicing — a spinning thread starves equal/lower priority threads on its core
  [3bMT].

**Clock/L2 control.** Exheader Flag1 bit 1 = 804 MHz, bit 0 = L2 enable (NS only ever sends 0 or 3) [3bExh];
`PTMSYSM:ConfigureNew3DSCPU` → `svcKernelSetState(10, …)`, New-3DS kernel only [3bPTM], [3bSVC]; libctru's
`osSetSpeedupEnable(true)` passes 3 [ctruOs]. DaedalusX64-3DS reported performance *dropping* with L2 + 804 MHz
enabled via Rosalina (user report, cause unknown) [daedalus] — worth an A/B on our own JIT.

**VFP11** (VFPv2) [vfp11], [mpcore] ch.16:
- Three pipelines (FMAC, DS divide/sqrt, LS) run in parallel; single-cycle throughput for single precision and most
  double; **double multiply/MAC: 2-cycle throughput** [vfp11] §1.4.
- Latencies (Table 4-17): FADD/FMUL/FMAC single 8 cycles; FDIV/FSQRT single 19 latency / 15 throughput, double 33 / 29;
  FMRS 2, FMSR 4; FMXR/FMRX serialize [vfp11]. **[measured]** 8–9 cycle add/mul, 17–19 div/sqrt, 5-cycle round trip.
- Short vectors (up to 8 single / 4 double) "issue the individual operations … in a serial fashion" [vfp11] §1.6 —
  the hardware reason short-vector mode gave no throughput gain **[measured]**.
- Subnormal inputs trap to support code unless flush-to-zero; RunFast = FZ + default NaN + all traps disabled:
  subnormals and tiny results become +0, NaNs become the default NaN, everything in hardware [vfp11] §1.5.4.
  *(Inference: Gekko is IEEE with denormals; RunFast diverges on denormals and NaN payloads — usually invisible in
  games but a correctness risk for `ps_` NaN-boxing tricks and for bit-exact physics.)*

**ARMv6K instructions an emulator wants** (timings from [mpcore] Table 15-7, 15-12):

| Instruction | Use | Cycles / result latency |
|---|---|---|
| `REV`, `REV16`, `REVSH` | byte-swap guest big-endian data | 1 / 1 |
| `SETEND BE/LE` | flip data endianness for a run of loads/stores | 1 |
| `SXTH`, `SXTB`, `UXTH`, `UXTB16`, `SXTAH`… | `extsh`/`rlwinm` masks, quantized unpack | 1 / 1 |
| `PKHBT`, `PKHTB` | pack halfwords | 1 / 1 |
| `SSAT`, `USAT`, `SSAT16`, `USAT16` | `psq_st` clamping | 1 / 2 |
| `SEL` + GE flags, `UADD8/16`, `QADD16`, `USAD8`… | SIMD-within-a-register (texture decode, audio mix) | 1 / 1–3 |
| `LDREX/STREX` (+B/H/D), `CLREX` | `lwarx/stwcx.` | supported (ID_ISAR3 SyncPrim = 2) |

- **No Thumb-2, no TableBranch** (ID_ISAR3 fields = 0) [mpcore]; hence **no `MOVW/MOVT`** (introduced in ARMv6T2)
  [movw] — 32-bit constants need a literal pool or `MOV`+`ORR` sequences.
- **No NEON** (VFPv2 only) [3bHW].
- Unaligned LDR is supported but "potentially ARMv6 unaligned" loads cost an extra cycle [mpcore] §15.10.

### 3.2 Memory

| Region | Physical | Size | Source |
|---|---|---|---|
| FCRAM | 0x20000000 (+ 0x28000000 New-3DS extension) | 128 + 128 MB = 256 MB | [gbatek], [3bMem] |
| VRAM | 0x18000000 | 6 MB = two 3 MB banks VRAM_A/VRAM_B | [gbatek] |
| AXI WRAM | 0x1FF80000 | 512 KB | [gbatek] |
| DSP code / data (shared WRAM) | 0x1FF00000 / 0x1FF40000 | 256 KB + 256 KB | [gbatek] |

**APPLICATION region by memory mode** [3bMem], [3bExh]:

| Mode | Application size | Note |
|---|---|---|
| Old 0 (Prod) | 64 MB | default |
| Old 2 / 3 / 4 / 5 | 96 / 80 / 72 / 32 MB | dev modes |
| New 1 / 3 (Prod, Dev2) | 124 MB | New-3DS default |
| New 2 (Dev1) | 178 MB | New-3DS only |

The exheader value must not exceed the AccessDesc value [3bExh]. *(Inference: a .cia can request the 178 MB mode in
its own exheader; a .3dsx inherits whatever the host title got, which is why it sees less.)*

**Heaps.**
- Linear heap = physically contiguous (constant VA↔PA offset), required for GPU/DMA; normal heap is not contiguous
  [3bMM]. Linear heap VA 0x14000000 or 0x30000000 (8.0.0+) [3bMem].
- libctru's default split [ctruHeap]: linear = half of remaining, then heap capped at 24 MB, then linear capped at
  32 MB with the rest going back to the heap. With ~90 MB free that yields 58 MB heap + 32 MB linear — matching our
  **[measured]** 58 MB max malloc / ~30 MB linearAlloc *(inference)*. Override with `__ctru_heap_size` /
  `__ctru_linear_heap_size`.
- **VRAM**: libctru splits VRAM into two pools (A, B) of `OS_VRAM_SIZE/2`; one allocation never spans both [ctruVram]
  → the **[measured]** 3 MB cap. VRAM is mapped at VA 0x1F000000 only as "specified by the exheader" [3bMem]
  *(inference: the **[measured]** CPU-write crash is that missing mapping/permission — VRAM is GPU-only here; fill it
  with GX MemoryFill/DisplayTransfer/TextureCopy).*
- **Executable memory**: `svcControlProcessMemory` is the only SVC that maps executable pages (types 4/5/6, not the
  current-process alias); `ControlMemory`/`MapMemoryBlock` only map XN R/W [3bMM]. Stock firmware limits it to RO;
  Luma patches out SVC permission checks (v9.0+) [luma]. RetroArch's dynarecs run from .3dsx on Luma ≥ 8.0, with "no
  difference in speed between .cia and .3dsx" [lrPCSX].

### 3.3 PICA200 GPU

| Property | Value | Source |
|---|---|---|
| Clock | 268 MHz | [3bHW], [3bER] |
| Vertex processors | 3 programmable + primitive engine (geometry shader) | [3bHW] |
| Shader limits | ≤512 instructions, ≤128 operand descriptors; c0–c95 float4, i0–i3, b0–b7; r0–r15; v0–v15; o0–o15 | [3bShader] |
| Shader float | 1 sign + 7 exp + 16 mantissa (24-bit), not IEEE, subnormals → +0 | [3bShader] |
| Control flow | 4-deep CALL, 8-deep IF, 4-deep LOOP stacks | [3bShader] |
| Vertex attributes | 12 attribute buffers | [3bIR] |
| Texture units | 3 + procedural texture unit | [3bIR] |
| Texture size | power of two, 8–1024 per side (citro3d check) | [c3dTex] |
| Texture formats | RGBA8888, RGB888, RGBA5551, RGB565, RGBA4444, IA8, HILO8, I8, A8, IA44, I4, A4, ETC1, ETC1A4 | [3bIR] |
| Color buffer | RGBA8, RGB5A1, RGB565, RGBA4 | [3bIR] |
| Depth | 16, 24, 24+8 stencil; early-depth unit (`GPUREG_EARLYDEPTH_*`) | [3bIR] |
| Combiners | 6 TexEnv stages + combiner buffer (`GPUREG_TEXENV_UPDATE_BUFFER`) | [3bIR] |
| Fragment lighting | 8 lights, per-fragment, LUT-driven (256-entry LUTs), primary + secondary color outputs to combiners | [3bIR], [3bFL] |
| Fog / gas | 128-entry fog LUT; gas mode | [3bIR] |
| Fill rate | 800 Mpixel/s at 200 MHz (DMP figure); 3DS-clock figure **unverified** | [wpPICA] |
| AA | 2×2 full-scene (via DisplayTransfer downscale) | [wpPICA], [3bER] |

**Command submission (GSP GX commands)** [3bGSP], [3bER]:
- `ProcessCommandList` (8-byte aligned, linear/VRAM).
- `MemoryFill` — 16/24/32-bit fill, VRAM targets, two buffers per command.
- `DisplayTransfer` — tiled↔linear conversion, format conversion, vertical flip, 2×1 or 2×2 box downscale.
- `TextureCopy` — raw copy with line width/gap, no conversion.
- `RequestDma`, `FlushCacheRegions`.

**Pitfalls** [3bPit]: every configured shader output component must be written exactly once or the PICA freezes; two
consecutive `MOVA` freeze it; 4-bit textures in VRAM on TEXUNIT1 can destabilize the GPU.

### 3.4 DSP (audio)

- CEVA TeakLite at 134 MHz, 24 channels, 32728 Hz [3bHW]. Shared-memory frames carry 160 stereo samples; 24 input
  configs, ADPCM coefficient blocks per input [3bDSPMem]. libctru's ndsp sleeps ~4.888 ms per frame [ctruNdsp].
- ndsp loads the DSP firmware from `/3ds/dspfirm.cdc` on SD, or from an `hb:ndsp` handle supplied by the launcher
  [ctruNdsp]; users dump it with DSP1 [dsp1].

### 3.5 Storage and homebrew environment

- SD: FAT32, up to 2 TB cards tested [3bHW]. **Throughput unverified** — no published 3DS SD benchmark found.
- Luma3DS: SVC/service permission checks patched out; `svcCustomBackdoor` (supervisor-mode call); data-cache flush
  SVCs including L2C and I-cache invalidate SVCs; `svcControlMemoryEx`; debug state on by default so user exception
  handlers work without `svcKernelSetState(6,1,1)` [luma]. Stock `svcBackdoor` (0x7B) was removed for ARM11 in
  11.0.0-33 [3bSVC].
- Exception handlers: `threadOnException(handler, stack, exception_data)` — handler must be ARM, must not return
  [ctruThread]; on stock kernels it needs UNITINFO≠0 [ctruThread], which Luma's debug-state patch covers [luma].
- **[measured]** I-cache must be invalidated after writing JIT code (199/200 stale without it).

---

## 4. Mapping: GC/Wii subsystem → 3DS mechanism

"Fact" columns are sourced above; the mechanism and bottleneck columns are **inference** unless marked.

| Guest subsystem | Likely 3DS mechanism | Expected bottleneck |
|---|---|---|
| Gekko/Broadway integer | ARM JIT on core 0; blocks linked with direct `B`; guest RAM byte-swapped or `REV` on access | I-cache/BTAC thrash once JIT code is large (**[measured]** 13→61 ns); register-indirect exits unpredicted ([mpcore]) |
| FPU / paired singles | VFP scalar ops, ps0/ps1 in adjacent S registers; RunFast | VFP latency 8 vs Gekko 3; one FP op/cycle vs Gekko's two SP FMAs/cycle ([vfp11], [hcGekko]); **[measured]** 55 MIPS on PS code |
| Quantized load/store (GQR) | specialize per GQR value at JIT time; `SXTB/UXTH` + VFP convert, `SSAT/USAT` on store | core↔VFP transfer cost (**[measured]** 5 cycles) per lane |
| Locked cache / DMA | 16 KB host buffer; DMA as memcpy at `DMAL` write | negligible |
| Write-gather pipe → CP FIFO | detect stores to 0xCC008000 in JIT, append to a host ring buffer, decode on core 2 | FIFO decode + vertex conversion throughput |
| MMU (BAT) | static mask of 0x80000000/0xC0000000 mirrors to one host base (*Dolphin "fastmem" analogue*) | none for BAT games |
| MMU (page tables, ~21 GC titles per [dolFinal]) | slow path with software TLB | expect unplayable; Dolphin needed years of work [dolMMU15], [dol2603] |
| 24 MB MEM1 + 16 MB ARAM | normal heap (58 MB available **[measured]**) | fits |
| Wii MEM2 64 MB | heap + linear + custom split, or .cia with 124/178 MB mode | 24+64 MB exceeds default 58 MB heap; needs `__ctru_heap_size` override or larger app memory mode |
| XF transform + 8 lights | PICA vertex shader (512 instr, 96 c-regs) | XF matrix memory (256+96+256 floats) exceeds 96 vec4 uniforms → upload only referenced matrices per draw; or transform on CPU |
| TEV (≤16 stages) | PICA 6 TexEnv combiners; multipass or CPU fallback beyond 6 | games using >6 stages or indirect texturing (no PICA equivalent) |
| 8 texture maps | 3 PICA texture units + procedural | multipass when >3 textures |
| Texture formats | decode I4/I8/IA/RGB565/RGB5A3/RGBA8 to PICA formats; CMPR → RGB565 or RGBA8 (no S3TC on PICA; ETC1 is a different codec) | CPU texture decode; FCRAM bandwidth (**[measured]** memcpy ~307 MB/s) |
| Max texture 1024 | Flipper allows up to 1024 too — **unverified** | — |
| EFB 640×528 | PICA color + depth buffers in VRAM_A/VRAM_B | 640×528 RGBA8 ≈ 1.35 MB per buffer — fits 3 MB banks |
| EFB copies to texture | render-to-texture + DisplayTransfer (tiled↔linear, 2×2 downscale) | copies the CPU reads back (Dolphin's "EFB to RAM" class) need VRAM→FCRAM transfer, then CPU YUV/format encode |
| XFB / VI | DisplayTransfer to LCD framebuffer, ignore XFB unless the game edits it (Dolphin's Hybrid XFB approach [dolXFB]) | 640×480 → 400×240 scaling quality |
| DSP AX | HLE AX into ndsp, resample 32 kHz → 32728 Hz | per-voice ADPCM decode + polyphase SRC on CPU (core 1 at ~78%) |
| AI streaming (disc audio) | ndsp channel | ADPCM decode |
| DI disc | pre-decrypted image on SD; Wii clusters 0x8000 → 0x7C00 data | SD throughput **unverified**; AES on ARM11 in software if not pre-decrypted |
| SI pads / Wii Remote | HID buttons, circle pad, gyro / touch for pointer | none |
| IOS (/dev/fs, es, di, usb…) | HLE in C on the host side, IPC as a function call | none |
| Timing (TB = CPU/12) | derive from `svcGetSystemTick` (268 MHz) or guest cycle counter | — |

### 4.1 How others handled similar gaps

- **Dolphin dropped its 32-bit ARM JIT**: 32-bit ARM "was never going to support all of Dolphin's JIT tricks, like
  fastmem"; 32-bit devices are "too slow to run Dolphin at full speed"; developers say Dolphin runs "really terrible
  on in-order CPUs" (Cortex-A53 class) [dolArm32]. *(Inference: Dolphin's design is not a template for an in-order
  804 MHz ARM11; expect to trade accuracy for speed throughout — no MMU, HLE everything, texture-only EFB copies.)*
- **Dolphin's MMU speedhack and fastmem**: BAT-only fastmem plus a fake BAT for ARAM-library games [dolMMU15],
  [dolFinal] — the cheap path to copy.
- **Dolphin ubershaders**: interpret TEV state in one shader to avoid compile stutter [dolUber]; not applicable to
  PICA's fixed function, but the opposite problem (fixed combiners) needs a TEV→TexEnv translator with a cache keyed
  on TEV state.
- **Citra (reverse direction)**: generates a host fragment shader per PICA combiner configuration, cached by a key
  copied from the TEV registers, and rounds each stage to 8 bits to match PICA precision [citraTEV] (commit history;
  exact commit-to-message mapping unverified).
- **RetroArch 3DS dynarecs** (gpSP, PCSX ReARMed): need Luma ≥ 8.0 for .3dsx; PCSX ReARMed switched to the Unai
  software renderer, "less accurate, but much faster", 10–20 fps faster in many games on New 3DS [lrPCSX].
- **DaedalusX64-3DS** (N64): "proof of concept with very poor performance" [daedalus].
- **DraStic**: no 3DS-relevant source found — **unverified**.

---

## 5. Not verified in this pass

- 3DS ARM11 L1 cache size (TRM allows 16/32/64 KB); read CP15 c0 Cache Type Register via `svcCustomBackdoor`.
- PICA200 fill rate and triangle rate at 268 MHz (only DMP's 200 MHz figures exist).
- 3DS SD card throughput.
- Gekko pipeline stage count; exact per-mnemonic FP latencies (750CL table column alignment).
- GQR type-code ordering (conventional reading given).
- Wii: ARAM absence and DSP access path to MEM2; IOS's reserved MEM2 range; Wii Remote report rate; Bluetooth chip in
  the console.
- "Most GC games mix at 32 kHz" (forum claim).
- Flipper maximum texture dimension; Flipper indirect stage count (Wikipedia says 4).
- Whether a .3dsx under Luma can raise its own app memory mode.

---

## References

[FINDINGS]: FINDINGS.md
[yagcd2]: https://www.gc-forever.com/yagcd/chap2.html
[yagcd3]: https://www.gc-forever.com/yagcd/chap3.html
[yagcd4]: https://www.gc-forever.com/yagcd/chap4.html
[yagcd5]: https://www.gc-forever.com/yagcd/chap5.html
[yagcd13]: https://www.gc-forever.com/yagcd/chap13.html
[ninGC]: https://www.nintendo.com/en-gb/Corporate/Nintendo-History/Nintendo-GameCube/Technical-Details/Technical-Details-627134.html
[wpGC]: https://en.wikipedia.org/wiki/GameCube_technical_specifications
[hcGekko]: https://old.hotchips.org/wp-content/uploads/hc_archives/hc13/2_Mon/06ibm-gekko.pdf
[750cl]: https://fail0verflow.com/media/files/ppc_750cl.pdf
[dolST]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/SystemTimers.h
[dolSTc]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/SystemTimers.cpp
[dolVC]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/VideoCommon/VideoCommon.h
[dolTD]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/VideoCommon/TextureDecoder.h
[dolBP]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/VideoCommon/BPMemory.h
[dolXF]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/VideoCommon/XFMemory.h
[dolDVD]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/DVD/DVDInterface.cpp
[dolAX]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/DSPHLE/UCodes/AX.h
[dolAXV]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/DSPHLE/UCodes/AXVoice.h
[dolACC]: https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/DSP/DSPAccelerator.cpp
[dolI7241]: https://bugs.dolphin-emu.org/issues/7241
[dolDSPf]: https://forums.dolphin-emu.org/Thread-new-ax-hle-what-is-it-and-how-does-it-work?pid=248893
[dolMMU15]: https://dolphin-emu.org/blog/1W
[dolFinal]: https://dolphin-emu.org/blog/3S
[dol2603]: https://dolphin-emu.org/blog/7H
[dolXFB]: https://dolphin-emu.org/blog/4X
[dolUber]: https://dolphin-emu.org/blog/2017/07/30/ubershaders/
[dolAA]: https://forums.dolphin-emu.org/Thread-question-about-efb-vs-aa?page=1
[dolEFB]: https://bugs.dolphin-emu.org/issues/10626
[dolArm32]: https://www.slashgear.com/dolphin-brings-gamecube-to-nvidia-shield-ditches-32-bit-02391465/
[wbHW]: https://wiibrew.org/wiki/Hardware
[wbBroadway]: https://wiibrew.org/wiki/Broadway
[wbMap]: https://wiibrew.org/wiki/Memory_map
[wbStarlet]: https://wiibrew.org/wiki/Hardware/Starlet
[wbIPC]: https://wiibrew.org/wiki/Hardware/IPC
[wbIOS]: https://wiibrew.org/wiki/IOS
[wbDisc]: https://wiibrew.org/wiki/Wii_disc
[wbTicket]: https://wiibrew.org/wiki/Ticket
[wbAES]: https://wiibrew.org/wiki/Hardware/AES_Engine
[wbSHA]: https://wiibrew.org/wiki/Hardware/SHA-1_Engine
[wbNAND]: https://wiibrew.org/wiki/Hardware/NAND
[wbWiimote]: https://wiibrew.org/wiki/Wiimote
[wbDSP]: https://wiibrew.org/wiki/Hardware/Macronix_DSP
[wbPI]: https://wiibrew.org/wiki/Hardware/Processor_Interface
[wbDOL]: https://wiibrew.org/wiki/DOL
[3bHW]: https://www.3dbrew.org/wiki/Hardware
[3bMem]: https://www.3dbrew.org/wiki/Memory_layout
[3bMM]: https://www.3dbrew.org/wiki/Memory_Management
[3bExh]: https://www.3dbrew.org/wiki/NCCH/Extended_Header
[3bMT]: https://www.3dbrew.org/wiki/Multi-threading
[3bAPT]: https://www.3dbrew.org/wiki/APT:SetApplicationCpuTimeLimit
[3bSVC]: https://www.3dbrew.org/wiki/SVC
[3bPTM]: https://www.3dbrew.org/wiki/PTMSYSM:ConfigureNew3DSCPU
[3bShader]: https://www.3dbrew.org/wiki/GPU/Shader_Instruction_Set
[3bIR]: https://www.3dbrew.org/wiki/GPU/Internal_Registers
[3bER]: https://www.3dbrew.org/wiki/GPU/External_Registers
[3bFL]: https://www.3dbrew.org/wiki/GPU/Fragment_Lighting
[3bPit]: https://www.3dbrew.org/wiki/GPU/Pitfalls
[3bGSP]: https://www.3dbrew.org/wiki/GSP_Shared_Memory
[3bDSPMem]: https://www.3dbrew.org/wiki/DSP_Memory_Region
[gbatek]: https://problemkaputt.de/gbatek-3ds-memory-and-i-o-map.htm
[mpcore]: https://documentation-service.arm.com/static/5e8e1e0388295d1e18d368b2
[vfp11]: https://documentation-service.arm.com/static/5e8e21eb88295d1e18d375b0
[movw]: https://ffmpeg.org/pipermail/ffmpeg-devel/2011-April/110575.html
[ctruThread]: https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/include/3ds/thread.h
[ctruHeap]: https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/source/system/allocateHeaps.c
[ctruVram]: https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/source/allocator/vram.cpp
[ctruOs]: https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/source/os.c
[ctruOsH]: https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/include/3ds/os.h
[ctruNdsp]: https://raw.githubusercontent.com/devkitPro/libctru/master/libctru/source/ndsp/ndsp.c
[c3dTex]: https://raw.githubusercontent.com/devkitPro/citro3d/master/source/texture.c
[luma]: https://github.com/LumaTeam/Luma3DS/wiki/Standard-features
[lrPCSX]: https://www.libretro.com/index.php/retroarch-3ds-full-speed-ps1-now-possible-with-pcsx-rearmed-w-unai-renderer/
[wpPICA]: https://en.wikipedia.org/wiki/PICA200
[dsp1]: https://www.gamebrew.org/wiki/DSP1_3DS
[citraTEV]: https://git.openpunk.com/OpenPunk/citra/commit/734279ff228f5d0981907a2ec1ac39aa31659b78
[daedalus]: https://www.elotrolado.net/hilo_ho-daedalusx64-nintendo-64-portado-a-3ds-beta-v0-2_2373216

Manual identifiers: ARM11 MPCore r2p0 TRM = ARM DDI 0360F; VFP11 TRM = ARM DDI 0274G (ARM1136JF-S r1p3 edition; the
MPCore TRM ch.16 carries the same VFP11 description); IBM 750CL User's Manual v1.0 (preliminary), 2007-08-08.
