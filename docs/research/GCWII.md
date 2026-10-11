# GameCube and Wii: what games need, and what the 3DS still has to prove

Research digest, 2026-10-11, from five web-research passes:
- Gekko/Broadway CPU accuracy;
- Flipper/Hollywood GPU;
- memory, buses, timing and I/O;
- Wii IOS and the Wii Remote;
- Dolphin's 1,891 per-game settings files.

It adds to ARCHITECTURE.md (the basic hardware reference). Tags:
- **[S]**: read in the cited source.
- **[I]**: inferred.
- **[?]**: unverified or found only in a search snippet.

Main sources: the IBM 750CL manual (fail0verflow mirror), Dolphin source (`Source/Core/Core/HW`, `PowerPC`,
`VideoCommon`, `IOS`, `Data/Sys/GameSettings`), YAGCD, WiiBrew, and libogc.

## 1. What games need, by count (Dolphin GameSettings, 1,891 INI files) [S]

The counts are INI files that force a setting. They are a lower bound on need, and some files only restate the
default.

| Feature | Files | Examples | For gc3ds |
|---|---|---|---|
| Texture-cache safety (hash 0 = all texels, or 512 samples) | 361 | Pokemon Colosseum (0), Metroid Prime, Animal Crossing (512) | texture hashing cost decides the policy (gap H4) |
| **EFB copies to RAM** (EFBToTextureEnable=False) | 157 | Wind Waker, Paper Mario TTYD, Sunshine, Mario Party 8 | GPU→CPU readback path (gaps H1, H2) |
| Single-threaded CPU/GPU (CPUThread=False) | 98 | Rogue Squadron II/III, F-Zero GX, Pokemon XD, Wii Sports | gc3ds's GX thread already syncs at PE reads and idle (deterministic) |
| DSP LLE audio | 79 | mostly SNK titles (Metal Slug, KOF, Samurai Shodown) | an LLE DSP on 3DS is unrealistic (§5); these titles need special treatment |
| CPU EFB access (peek/poke) | 44 | Wind Waker, Galaxy 1/2, Sunshine, **Twilight Princess (Wii)** | single-pixel readback latency (gap H3) |
| EFB format-change emulation | 35 | Mario Kart Wii, Mario & Sonic | render-target formats (gap H6) |
| Defer EFB copies off | 33 | Paper Mario TTYD, Wii Fit | copy eagerly |
| MMU (page tables) | 32 | Rogue Squadron II/III, Pokemon XD, Spider-Man 2 | software TLB cost in the JIT (gap H9) |
| Vertex rounding | 25 | Shrek Smash n' Crash, Frogger | cheap shader tweak |
| FPRF (float result flags) | 24 | F-Zero GX | JIT flag computation |
| XFB to texture off | 22 | Eternal Darkness, Resident Evil 0 | XFB handling |
| SyncOnSkipIdle off | 15 | Battalion Wars, Tetris Worlds | frame pacing |
| Bounding box | 8 | Paper Mario TTYD, Magical Mirror | CPU-side bbox from transformed vertices |
| Perf queries, FastDiscSpeed, AccurateNaNs, ... | 1–3 each | Sunshine (perf queries) | skip |

- **TP:** GameCube (GZ2) only pins VISkip=False and CPUCull=True. Wii (RZD) also needs CPU EFB access.
- **Big games with no special needs:** Smash Melee and Brawl, Mario Kart Double Dash, Pikmin 1/2.
- **Dolphin Android** defaults to dual core. The 98 single-thread files are the ones that break under that default.

## 2. CPU (Gekko / Broadway) accuracy points [S unless marked]

1. **fres / frsqrte use hardware estimate tables:** resolution < 1/4000, rounding toward zero, ignoring RN (750CL
   manual).
   - Dolphin bug #7304: Mario Party 6/7, Mario Sunshine, **Twilight Princess**, MK Double Dash and Mario Galaxy broke
     when these were "too precise".
   - **gc3ds computes them exactly in both the interpreter and the JIT: an accuracy bug.**
   - Dolphin's tables are GPL and gc3ds is MIT, so they cannot be copied. Derive them independently (an owner
     decision on the source).
2. **fmuls / ps_mul round the multiplier operand to 25 bits.** gc3ds does this (`ld_c25`). Dolphin also applies it to
   the fmadd addend; check gc3ds.
3. **Signed zero:** PowerPC fnmsub gives −0 where ARM gives +0 (the Inazuma Eleven desync).
4. **FPSCR[NI]** (non-IEEE mode) zeroes denormal operands.
5. **Locked L1 cache (HID2 LCE):** 16 KB of the D-cache as a scratchpad at 0xE0000000, dcbz_l, and DMA via DMAU/DMAL
   (SPR 922/923) in 32-byte lines (length 0 = 128 lines).
   - HID2 masks: WPE 0x40000000, PSE 0x20000000, LCE 0x10000000.
   - **Bit numbering differs between the 750CL manual (MSB-0) and WiiBrew (LSB-0).**
6. **Write gather pipe** (WPAR, SPR 921): a 128-byte FIFO sending 32-byte bursts.
7. **dcbi must invalidate:** Casper's Scare School fails its boot check otherwise; Disney Infinity and Cars 2 also
   depend on it. icbi is not broadcast.
8. **MMU page tables:** Rogue Squadron II/III, Spider-Man 2, Ultimate Spider-Man, Clone Wars. DSI at 0x300, ISI at
   0x400. 4 IBAT + 4 DBAT pairs.
9. **Time base and decrementer** run at bus/4: 40.5 MHz on GameCube, 60.75 MHz on Wii (CPU/12).
10. **Idle loops:** Dolphin's detection misses custom loops (Sonic Colors, Fire Emblem PoR, Metroid Prime).
11. **Broadway:** 729 MHz, PVR 87102 (Gekko 83410), adds HID4 (SPR 1011).

Ranked by game impact: estimate tables, then MMU, then dcbi, then idle loops, then fmul 25-bit, then signed zero,
then the locked cache, then the write-gather pipe, then NI.

## 3. GPU (Flipper / Hollywood) facts gc3ds will need [S]

**Command processor** (0xCC000000):
- Registers: FIFO base, end, high/low watermarks, distance, write and read pointers, breakpoint at 0x20–0x3C.
- CR bits: read enable, BP enable, overflow/underflow interrupt, link, BP interrupt. YAGCD and Dolphin disagree on the
  BP bits.
- One CP interrupt (0x800).
- PE token: BP 0x48|t then 0x47|t raises INT 0x200. Dolphin delays it ≥ 500 cycles as a hack.
- Finish: BP 0x45 bit 1 raises INT 0x400.

**XF memory:**
- position matrices 0x000–0x0FF;
- normal matrices 0x400–0x45F (top 20 bits);
- post-transform 0x500–0x5FF;
- lights 0x600–0x67F.

**XF registers:**
- 0x1005 clip disable;
- 0x1009 colours;
- 0x100A–1011 channel control;
- 0x1018/19 matrix index;
- 0x101A–1F viewport (libogc adds 342);
- 0x1020–26 projection;
- 0x103F/0x1040–47 texgen;
- 0x1050–57 post-transform.

**BP registers:**

| Area | Registers |
|---|---|
| TEV | colour env 0xC0 even, alpha 0xC1 odd, registers 0xE0/E1, KSEL 0xF6–FD, TREF 0x28–2F |
| Indirect | IREF 0x27, commands 0x10–1F, matrices 0x06–08 |
| Depth | ZMODE 0x40; PE_CONTROL 0x43 (pixel format, Z format linear/near/mid/far, early-Z / zcomploc) |
| Pixel formats | RGB8_Z24, RGBA6_Z24, RGB565_Z16, Z24, Y8, U8, V8, YUV420 |
| Alpha compare | 0xF3 (two refs, two functions, AND/OR/XOR/XNOR) |
| Blend | 0x41 (subtract, logic ops, dither, masks); constant alpha 0x42 |
| Fog | range 0xE8; parameters 0xEE–F1 |
| Other | bbox 0x55/56; perf counters 0x23/24/57/67; scissor 0x20/21/59; line/point width 0x22; Z-freeze GenMode bit 19 |

**EFB:** 640×528.
- The XFB is up to 720×576; copies are 640 wide.
- Copy trigger 0x52: clamp, format, gamma 1.0/1.7/2.2, clear, field, target.
- Source 0x49, size 0x4A, destination 0x4B, stride 0x4D, Y scale 0x4E, clear values 0x4F–51.
- Copy formats include Z copies (Z4, Z8M, Z8L, Z16L).
- Vertical filter 0x53/54 (7 weights).

**Textures:** TMEM 1 MiB.
- Preload 0x60–63, TLUT 0x64/65, invalidate 0x66.
- Formats I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2, CMPR, plus Z8/Z16/Z24X8.
- Maximum 1024×1024.

**Hollywood:** 243 MHz, "identical to Flipper with minimal corrections" (Copetti); Dolphin uses the same EFB
constants for both.

## 4. Memory, buses, timing, I/O [S]

**Memory:**
- MEM1 24 MB.
- ARAM 16 MB, DSP DMA only, at 246 CPU ticks per 32 bytes (measured on hardware per Dolphin): ~63 MB/s.
- Wii MEM2 64 MB; IOS reserves the top 12–16 MB.
- Size words at 0x80000028, 0x800000F0 and 0x800000D0.

**Disc (DI):**
- 300 µs command latency, 600 µs read latency, 32 MiB/s buffered, 1 MiB read-ahead.
- Seek is 0.035 s + 50 s/m·d below 1 mm, else 0.075 s + 4.5 s/m·d (measured).
- **Instant reads break games:** Metroid Prime doors, Sonic Riders crashes, Summoner, Pitfall.

**EXI:**
- Memory card: commands 0xF1/F2/52/83/89/81; 8 KB blocks, 5 system blocks, 128-byte pages; ID 0xC221.
- Card speed: read 512 KiB/s, write 98 KB/s.
- RTC: seconds since 2000 plus the SRAM bias at 0x0C.
- SRAM 64 bytes; IPL ROM fonts.

**SI:**
- 4 µs per bit.
- Pad reply: 8 bytes, `[00?SYXBA][1LRZUDRL][x][y][cx][cy][l][r]`.
- Polled about once per frame; Dolphin's default poll time X is 492.

**AI and VI:**
- AI "48 kHz" is really 48,043 Hz.
- VI frame rates come from the half-line counters. Display interrupts DI0–3 are at 0xCC002030–3C.

**PI interrupt causes:** PI 0x1, RSW 0x2, DI 0x4, SI 0x8, EXI 0x10, AI 0x20, DSP 0x40, MEM 0x80, VI 0x100, PE_TOKEN
0x200, PE_FINISH 0x400, CP 0x800, HSP 0x2000, WII_IPC 0x4000, RST 0x10000.

**Minimum timing to model:**
1. Time base and decrementer from CPU cycles.
2. VI half-line interrupts.
3. Disc latency and seek.
4. SI once per frame.
5. Memory-card busy/ready states.
6. Size-proportional DMA completion.
7. AI pacing.
8. Read-to-clear PI interrupt status.

## 5. Wii [S]

**Booting a disc without the System Menu** (Dolphin's `EmulatedBS2_Wii`):
1. Read the TMD and take the IOS version from it.
2. Fill low memory (0x28, 0x2C, 0x3180–0x3198, 0x30E6, 0x315C/5E, 0x3100 region).
3. Put `setting.txt` at 0x3800.
4. Open the partition through /dev/di.
5. Load the apploader from disc 0x2440 to 0x01200000 and jump to the entry point it returns.

**IOS high-level emulation:**
- **Needed:** ES (launch, title ID, DIVerify blocked), FS (/tmp wiped, 16 handles, paths under 64 characters), DI
  (read, partition, cover status), STM stubs, AES and SHA.
- **Can return errors:** SDIO (no card), networking (the game goes offline), /dev/flash, mload, usb123/usb2.
- **SYSCONF:** `/shared2/sys/SYSCONF`, 0x4000 bytes, `SCv0`, keys BT.DINF, IPL.LNG, IPL.AR, IPL.PGS, IPL.E60.

**Wii Remote:**
- Transport: fake Bluetooth HCI on /dev/usb/oh1, L2CAP PSM 0x11/0x13. Report mode 0x12 sets the mode (default 0x30;
  0x31, 0x33, 0x35 and 0x37 add accelerometer, IR and extension data). Dolphin updates at 200 Hz.
- Extensions: initialise with 0x55→0xA400F0 and 0x00→0xA400FB; the ID is at 0xA400FA.
- **On the 3DS:** circle pad for the Nunchuk; accelerometer for remote motion; gyro for MotionPlus; touchscreen or
  gyro aiming for the IR pointer.

**Disc decryption:**
- Measured on the 2DS (gcbench 4, software AES-128-CBC): **10.1 MB/s**. Wii discs need 6–9 MB/s, so decrypting
  while playing costs **61–92% of a core**.
- The 3DS hardware AES (PS service) works only with console keyslots.
- **Conclusion: decrypt Wii discs offline on the PC** (planned in HANDOFF) or keep a decrypted cache on the SD card.

**DSP LLE (79 titles)** [I]:
- The GameCube DSP runs at 81 MHz (Wii 121.5 MHz).
- An interpreter at ~20–30 ARM cycles per DSP instruction would need about 2 GHz. Even a DSP JIT at ~4 cycles per
  instruction would take a whole 3DS core.
- Those titles need a special high-level path, or they go without sound.

## 6. Gap analysis: what the 3DS still has to prove for broad compatibility

Already measured (MEASURE_PLAN.md, GPU_GUIDANCE.md): geometry, fill, combiners, draw costs, fog, memory bandwidth,
flushes, cores, audio.

| Gap | Question | Why it matters | Test (gcbench 6) |
|---|---|---|---|
| H1 | EFB-copy-to-RAM cost by size (64², 160×120, 320×240, 640×480): transfer + invalidate + CPU conversion | 157 games (Wind Waker, TTYD, Sunshine) | time each step |
| H2 | Can the depth buffer (D24S8) be read back, and are its values right? | Z copies; Z peeks for lens flares | draw known depths, TextureCopy the depth buffer, decode |
| H3 | Latency of a synchronous single-pixel readback (CPU EFB peek) | 44 games incl. TP Wii, Galaxy | µs per peek |
| H4 | Texture hashing speed (full vs sampled) | texture-cache validation, 361 settings files | MB/s for CRC32 / xxHash32 / 512-sample hash |
| H5 | GameCube texture decode speed (CMPR, RGB5A3, CI8, I4 → PICA formats, with tiling) | every texture upload | MB/s per format |
| H6 | Rendering into RGB565 / RGBA4 / RGB5A1 targets | EFB pixel-format changes (35 games) | fill rate per target format |
| H7 | 3DS gyroscope / accelerometer update rate and latency | Wii Remote motion and pointer | changes per second, read cost |
| H9 | Cost of a software-TLB lookup per memory access | MMU games (32) | cycles per access, TLB vs direct |
| H10 | Near-plane clipping, done right | GameCube near-plane geometry | a triangle whose visible part covers the samples |
| — | DSP LLE | 79 titles | not benchmarkable usefully; see §5 |
