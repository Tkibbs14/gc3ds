# Audio for gc3ds: GameCube audio onto the 3DS

Research digest, 2026-10-11. It draws on two web-research passes: Dolphin's DSP HLE plus GameCube hardware docs, and
libctru ndsp plus 3DS emulator drivers.

Tags:
- **[S]**: read in the cited source.
- **[I]**: inferred.
- **[?]**: unresolved or found only in a search snippet.

**Goal:** audio that works across most games, not only Twilight Princess.

## 0. The plan, in order

1. **Identify the microcode by hash.** Hash the DSP's IRAM at boot, as Dolphin does with HashEctor and UCodeFactory,
   and dispatch to AX, Zelda, CARD, GBA or ROM. An unknown hash plays silence; it never crashes.
   - Over 99% of games use **AX** [?] (Dolphin blog, via search).
   - TP's 0x6ca33a6d belongs to the **Zelda** family [S].
2. **Protocol and timing with a null mixer.** Answer mails, acknowledge command lists, yield with the right cycle
   counts, run the AI sample counter and interrupts, and service the audio DMA. **No game may stall.** This matters
   more than sound: many games wait on audio interrupts.
3. **Voice processing at the native cadence:**
   - AX: 5 ms frames (160 samples at 32 kHz), parameter-block updates every 1 ms.
   - Zelda: 80-sample frames.
   - Main left/right only; no effects.
4. **Voices onto the 3DS DSP (ndsp)** where they fit, mixing the rest on the CPU (section 3).
5. **Effects, in this order:** low-pass, AUX A/B reverb (or dropped), Zelda's Dolby positional volume, then the
   per-game flags.
6. **No DSP LLE.** It is slow even on PC.

## 1. GameCube hardware [S]

- **DSP:** 81 MHz with 16-bit words. Mailboxes at 0xCC005000/02 (CPU→DSP) and 0xCC005004/06 (DSP→CPU).
- **ARAM DMA:** 0xCC005020–2A; bit 15 of CNT_H is the direction. Dolphin models completion as (count/32)·246 ticks.
- **Audio interface (AI):** 0xCC006C00. AISCNT (0x6C08) counts stereo pairs; AIIT (0x6C0C) raises the interrupt.
  - Dolphin inverts YAGCD's sample-rate bit; AX runs at 32 kHz. Follow Dolphin.
- **Audio DMA (DSP side):** 0xCC005030/32 (start), 0xCC005036 (length/32, bit 15 = play). It moves 32 bytes per
  4 kHz tick. The interrupt is scheduled ~200 cycles after the DMA starts; shorter delays crashed some games.
- **Disc streaming (DTK):** DI command 0xE1. Which games use it: not found.

## 2. Microcodes [S] (Dolphin UCodes.cpp)

- **AX:**
  - CRCs 0x3ad3b7ac, 0x3daf59b9, 0x4e8a8b21, 0x07f88145, 0xe2136399, 0x3389a79e; AXWii has 8 more.
  - Command list: SETUP, DL_AND_VOL_MIX, PB_ADDR, PROCESS, MIX_AUXA/B, SET_LR, COMPRESSOR, OUTPUT, END.
  - An empty list yields with 2500 cycles (`AX_EMPTY_COMMAND_LIST_CYCLES`); some games depend on it.
  - Per voice: ADPCM with yn1, yn2, pred_scale and loop info; a 16.16 resampler; a volume ramp; a one-pole
    low-pass; mixing to main, AUX A and AUX B.
  - Polyphase coefficients come from `dsp_coef.bin`, a dumped console file: never ship it. Use linear
    interpolation without it.
  - "PB_ADDR, PROCESS, OUTPUT, END give partial audio in ~90% of games" (delroth, 2012).
- **Zelda family:**
  - CRCs include 0x86840740 (Wind Waker), **0x6ca33a6d (TP)**, 0xd643001f, 0x6ba3b3ea, 0x24b22038, 0x2fcdf1ec,
    0xdf059f68, 0x4be6a5cb, 0x267fd05a, 0x42f64ac4, 0x56d36052, 0x6c3f6f94, 0xb7eb9a9c, 0xeaeb38cc.
  - Games [?]: Mario Sunshine, Pikmin 1/2, Wind Waker, TP, the IPL, DK Jungle Beat, Four Swords and Animal
    Crossing (the last uses the "light" protocol). Sources disagree on Double Dash and Luigi's Mansion.
  - Protocol: default or light. Command 0x01 sets up (coefficients, patterns, sine table, AFC coefficients);
    0x02 renders.
  - Voice sources: square, saw and constant patterns; PCM8 and PCM16 from ARAM; AFC 2-bit and 4-bit; PCM16 from
    main RAM.
  - Processing: 4-tap resampler, low-pass, biquad, reverb, Dolby positional volume.
  - Per-game flags: MAKE_DOLBY_LOUDER (TP), SYNC_PER_FRAME (Pikmin PAL, Sunshine), LIGHT_PROTOCOL.
- **Others:** CARD unlock 0x65d6cc6f; GBA link 0xdd7e72d5; ROM and INIT.

## 3. The 3DS side (ndsp) [S] (libctru ndsp.h and channel.h; RetroArch ctr_dsp_audio.c)

- **DSP:** CEVA TeakLite at ~134 MHz, 24 channels, output at 32728 Hz (SYSCLOCK_SOC/512).
  - It needs `sdmc:/3ds/dspfirm.cdc`, dumped by DSP1 or Luma. Without it there is no sound, so the app must say
    so on screen.
- **Formats:** MONO_PCM8, MONO_PCM16, MONO_ADPCM (DSPADPCM: 8 coefficient pairs via `ndspChnSetAdpcmCoefs`, plus a
  context `{index, history0, history1}` per wave buffer), STEREO_PCM8 and STEREO_PCM16.
  - **No stereo ADPCM.**
  - **No loop start/end or loop context:** only a whole-buffer `looping` flag.
- **Per channel:**
  - Interpolation: polyphase, linear or none.
  - Rate via `ndspChnSetRate`.
  - 12 mix levels (front L/R, back L/R, AUX 0 ×4, AUX 1 ×4).
  - A frame callback every ~5 ms.
  - Buffers in linear memory, flushed before playing.
- **Can GameCube ADPCM go straight to ndsp?** The two research passes disagree:
  - Against: GameCube ADPCM frames are 8 bytes, a header byte (scale nibble | coefficient index) followed by
    14 nibbles, and the coefficient index changes per frame.
  - For: ndsp counts DSPADPCM samples "in nibbles without frame headers", which implies the 3DS format carries the
    same per-frame headers, with `index` as the initial predictor/scale [I].
  - **Test A1 in MEASURE_PLAN.md settles it.** If it passes, mono ADPCM voices cost the ARM11 nothing to decode.
- **Mapping:**
  - Mono ADPCM, PCM8 and PCM16 voices → ndsp channels (up to 24).
  - Stereo voices → two mono channels.
  - Loop points → split buffers at the loop start, or decode on the CPU.
  - Effects, voices beyond 24, and the AUX mix → CPU.
- **RetroArch's driver:** one stereo PCM16 ring of 2048 frames at 32728 Hz, with no interpolation and blocking
  writes woken by the frame callback. No time-stretching.

## 4. Running slower than full speed

- At 15% speed the game generates 15% of the samples per real second, so a ring buffer only underruns.
- Dolphin's modern design ties audio speed to emulated CPU speed and stutters when the CPU is slow.
- Options [I]:
  - **Time-stretching (WSOLA-style):** normal pitch, some artefacts, costs CPU.
  - **Slowed playback (pitch drops):** cheapest, sounds wrong.
  - **Drop audio below a speed threshold.**
- Recommendation: **mute below ~50% speed, stretch between 50% and 100%**, and pace to audio at 100%. Owner
  decision.

## Sources

- Dolphin `Source/Core/Core/HW/`: DSPHLE/UCodes/UCodes.cpp, AX.cpp, AXVoice.h, Zelda.cpp; AudioInterface.cpp;
  DSP.cpp
- YAGCD chapter 5: hitmen.c02.at/files/yagcd/yagcd/chap5.html
- DSP-ADPCM format: wiki.axiodl.com/w/DSP_(File_Format)
- delroth, "Emulating GameCube audio" (2012): delroth.net/posts/emulating-gamecube-audio-dolphin/
- libctru: ndsp.h, channel.h, services/dsp.c, services/csnd.c
- RetroArch: audio/drivers/ctr_dsp_audio.c, ctr_csnd_audio.c
- 3dbrew: DSP_Binary, DSP_Memory_Region, DSP_Services, CSND
