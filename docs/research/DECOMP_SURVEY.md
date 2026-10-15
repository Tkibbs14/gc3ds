# GameCube / Wii decompilation and port survey (for gc3ds native mode)

Captured: 2026-10-15. Method: WebFetch and WebSearch only. Nothing cloned or downloaded.
Tags: **SOURCED** = read on the cited page (or, where marked "search excerpt", in a search result snippet; the article was not opened). **INFERRED** = my reasoning from sourced facts. "not found" = searched, nothing usable.
Progress figures are reported as the tracker shows them. decomp.dev pages show **no date** for progress; the only time marker is relative ("updated N hours ago") in listings, so every decomp.dev figure below is undated.

## 0. Findings that constrain the design

- **No complete project list is reachable.** decomp.dev says "the main index and adding new projects are disabled," while individual project pages remain. SOURCED: https://decomp.dev/ . The decomp.wiki (Decompedia) home page fetched with no body text. SOURCED: https://decomp.wiki (empty). The awesome-gamecube-development list was only seen as a search excerpt. SOURCED (excerpt): https://www.context-awesome.com/list/command-tab/awesome-gamecube-development
- **Aurora is the one shared GC/Wii layer with a working port, and it targets desktop GPUs.** Aurora is "a source-level GameCube & Wii compatibility layer intended for use with game decompilation projects," covering GX, PAD, DVD and CARD, MIT-licensed. Its GX backends are D3D12, Vulkan and Metal, and GX is "built on top of WebGPU" (Dawn). Platforms: Windows, Linux, macOS, iOS, tvOS, Android. No 3DS, Switch, Vita or Wii U. SOURCED: https://github.com/encounter/aurora
- **Dusklight's GPU floor rules out the 2DS as-is.** "At a minimum, Dusklight requires a GPU with support for D3D12, Vulkan 1.1+, or Metal." D3D11 and OpenGL ES are "best-effort." SOURCED: https://github.com/TwilitRealm/dusklight . Inference: gc3ds would need a PICA200 backend behind Aurora's GX API, not the existing backends. INFERRED.
- **Disc-at-runtime is the norm for these ports.** Dusklight: "You must provide your own copy of the original game," and it suggests converting ISO to RVZ "to save space." SOURCED: https://github.com/TwilitRealm/dusklight . ACGC-PC-Port "reads assets from the image at startup, so no extraction step is needed." SOURCED: https://github.com/flyngmt/ACGC-PC-Port . Inference: gc3ds needs an RVZ reader on-device, which is real extra work. INFERRED.
- **Static recompilation is a different approach.** Mario Kart Wiicompiled, Wind Waker Recomp and DolRecomp are static recompilers, not source ports. Their code cannot be reused as game source. Listed in section 3 for reference only.
- **Provenance policies differ.** Petari and NSMBW-Decomp both say AI-generated decompilation work is not allowed. SOURCED: https://github.com/SMGCommunity/Petari , https://github.com/NSMBW-Community/NSMBW-Decomp . Press reports that Mario Kart Wiicompiled used AI-assisted coding. Search excerpt: https://www.notebookcheck.net/Mario-Kart-Wii-gets-native-PC-port-with-60-FPS-and-online-play-even-as-fans-decry-AI-use.1346163.0.html . Check provenance before integrating any port.

## 1. Project table (decomps, one row per game)

Columns: game and platform; target IDs; decomp repo, license, progress; port, license, runtime assets.

| Game (platform) | Target ID(s) | Decomp: repo, license, progress | Port: name, layer, license, assets |
|---|---|---|---|
| **Twilight Princess** (GC; Wii discs in port) | GZ2E01 default; GZ2P01, GZ2J01; Wii RZDE01_00, RZDE01_02, RZDP01, RZDJ01, DZDE01; Shield/ShieldD | zeldaret/tp, CC0-1.0. SOURCED: https://github.com/zeldaret/tp . GZ2E01 100.00% decompiled, 87.13% fully linked, undated. SOURCED: https://decomp.dev/zeldaret/tp . Repo says "not every TU has been linked yet." | **Dusklight**, TwilitRealm/dusklight, CC0-1.0, "Powered by Aurora" (MIT). PC and mobile. Repo: 5.9k stars, 7,201 commits. SOURCED: https://github.com/TwilitRealm/dusklight . Assets: user disc, read at run time. Wii discs run the GC version's HUD and behaviour. |
| Twilight Princess, Switch | same | same | **dusklight-NX**, a fork of TwilitRealm/dusklight. License not stated. Release listed as "5 months ago" (relative). SOURCED: https://github.com/givethesourceplox and https://newreleases.io/project/github/givethesourceplox/dusklight-NX/release/v1.0.0-test |
| **Animal Crossing** (GC) | GAFE01_00 only | ACreTeam/ac-decomp, CC0-1.0. Progress badges, no figure in page. SOURCED: https://github.com/ACreTeam/ac-decomp . Announced "100% matching and linked" in June 2025 (excerpt; article not opened): https://www.timeextension.com/news/2025/06/after-two-and-a-half-years-gamecube-animal-crossing-is-almost-fully-decompiled-let-the-modding-commence | **ACGC-PC-Port** (flyngmt), built on ac-decomp. GX replaced by "OpenGL 3.3." Windows (MSYS2 i686 GCC, SDL2). License file not named. Last release date not shown. SOURCED: https://github.com/flyngmt/ACGC-PC-Port . Runtime disc read. |
| **Super Mario Galaxy** (Wii) | RMGK01 default; RMGJ01, RMGE01, RMGP01 badges | SMGCommunity/Petari, CC0-1.0. SOURCED: https://github.com/SMGCommunity/Petari . decomp.dev/SMGCommunity/Petari: 100.00% decompiled, 68.75% linked, RMGK01, undated. SOURCED: https://decomp.dev/SMGCommunity/Petari . **Conflict:** a listing excerpt shows "Super Mario Galaxy 28.75% decompiled / 13.24% linked," undated (search excerpt, https://scriptagc.wasmer.app/https_decomp_dev). Press: fully decompiled, "this week," dated late Sept 2026 (search excerpt, https://androidauthority.com/super-mario-galaxy-decompiled-pc-port-3717193). | **AstroCore**, Luma's Workshop. Announced, "coming soon," not released. No repo, license, layer or target found. Article dated 29 Sep 2026. SOURCED: https://games.gg/news/super-mario-galaxy-pc-port-human/ . Petari says "not meant to be an effort to create a PC port." |
| **Pikmin 1** (GC) | GPIE01_01 shown | projectPiki/pikmin (repo not opened). decomp.dev/projectPiki/pikmin: 100.00% / 100.00%, undated. SOURCED: https://decomp.dev/projectPiki/pikmin . Listing excerpt elsewhere shows 99.15%, undated. | Unofficial PC build reported (search excerpt): runs the game code natively, "translates GX to OpenGL," 30/60/120 FPS, USA Rev 1 and Europe, ROM supplied by user. Repo not identified; license not found. https://www.dsogaming.com/?p=196753 |
| **Pikmin 2** (GC) | GPVE01 (USA), GPVE01_D17, GPVE01_D18, GPVP01, GPVJ01 | projectPiki/pikmin2, CC0-1.0. SOURCED: https://github.com/projectPiki/pikmin2 . decomp.dev: 99.63% decompiled, 98.09% linked, GPVE01, undated. SOURCED: https://decomp.dev/projectPiki/pikmin2 | No port in README; "does not mention a PC port, either existing or planned." Extract disc with Dolphin (`sys/main.dol`). |
| **Mario Party 4** (GC) | GMPE01_00, GMPE01_01 (USA) done; PAL/JP "stale" | mariopartyrd/marioparty4. License not stated. README: USA ✅, PAL/JP ❌. No percentage shown. SOURCED: https://github.com/mariopartyrd/marioparty4 . Press (May 2025, excerpts): 99.8% to 99.88%, "first GameCube game fully decompiled." | Alpha "Party Board," native PC, keyboard/mouse/controller, no music or SFX (article 13 Apr 2026). SOURCED: https://www.generationamiga.com/2026/04/13/mario-party-4-is-finally-playable-on-pc-thanks-to-fan-made-native-port/ . **Conflict:** the repo README says "NO working PC port yet, but it is in the making." Layer not stated. License not stated. |
| **Metroid Prime** (GC) | GM8E01_00, GM8E01_01, GM8E01_02, GM8E01_48, GM8P01_00, GM8J01_00 | PrimeDecomp/prime, CC0-1.0. No percentage shown. SOURCED: https://github.com/PrimeDecomp/prime . | **Metaforce** (AxioDL/metaforce), C++, MIT and CC0 listed. README: "Builds are currently unavailable as the project stabilizes." Windows D3D12/Vulkan/D3D11, macOS Metal, Linux Vulkan, Android, iOS. Not named Aurora in its README. SOURCED: https://github.com/AxioDL/metaforce . Aurora's README says Aurora was "originally developed for use in Metaforce." |
| Metroid Prime 2: Echoes (GC/Wii) | six DOL builds (regional/JP) | PrimeDecomp/echoes, "matching decompilation." No figure found. Repo seen in search excerpt: https://mymirror.duckdns.org/PrimeDecomp/echoes (not opened). | not found |
| **Wind Waker** (GC) | GZLE01 default; GZLP01; GZLJ01; D44J01 kiosk; KOR Rev 48 | zeldaret/tww, CC0-1.0. SOURCED: https://github.com/zeldaret/tww . decomp.dev: 79.58% decompiled, 68.07% linked, GZLE01, undated. SOURCED: https://decomp.dev/zeldaret/tww | Port: **Wind Waker Recomp** (elliotttate). **Static recompilation**, not a source port. Win10/11 64-bit, AVX2, D3D12 (Metal on Mac). Requires GZLE01 Rev 0; HD version unsupported. Single secondary source for "Dolphin hardware layer": https://borncity.com/news/wind-waker-recomp-fan-port-bringt-den-gamecube-klassiker-auf-pc-und-mac/ . License: unverified (Trendshift lists GPL-3.0, with a mismatched description, https://trendshift.io/repositories/272745 ). A later Spanish article describes an HD 0.2.1 port with Linux/Android. It conflicts with the German report; not verified. |
| **Skyward Sword** (Wii) | SOUE01 (NTSC-U 1.0 only) | zeldaret/ss, CC0-1.0. decomp.dev: 27.82% decompiled, 15.97% linked, undated. SOURCED: https://decomp.dev/zeldaret/ss , https://github.com/zeldaret/ss | not found |
| **Super Mario Sunshine** (GC) | GMSJ01 default; GMSE01; GMSP01; GMSJ01_01; GMSP01_NX (Switch 3D All-Stars build) | doldecomp/sms, CC0-1.0. decomp.dev: 48.96% decompiled, 18.28% linked, GMSJ01, undated. SOURCED: https://decomp.dev/doldecomp/sms , https://github.com/doldecomp/sms | not found |
| **Super Smash Bros. Melee** (GC) | GALE01 (v1.02) | doldecomp/melee. License not stated. SOURCED: https://github.com/doldecomp/melee . README: "matching decompilation," current effort on "naming and cleanup." decomp.dev: 100.00% / 100.00%, undated. SOURCED: https://decomp.dev/doldecomp/melee . **Conflict:** a listing excerpt shows 47.50% (undated). | not found. README does not mention a PC port. |
| **Paper Mario: TTYD** (GC) | G8MJ01 | doldecomp/ttyd. decomp.dev: 12.10% decompiled, 8.12% linked, undated. SOURCED: https://decomp.dev/doldecomp/ttyd . (Listing excerpt shows 10.55% / 7.47%, undated.) | not found |
| **Mario Kart: Double Dash** (GC) | version not shown | doldecomp/mkdd. decomp.dev: 47.09% decompiled, 41.93% linked, undated. SOURCED: https://decomp.dev/doldecomp/mkdd | not found |
| **New Super Mario Bros. Wii** (Wii) | PALv1 binaries | NSMBW-Community/NSMBW-Decomp. License not found. Badges, no % in page. SOURCED: https://github.com/NSMBW-Community/NSMBW-Decomp . decomp.dev listing excerpt: 3.21% / 3.21%, "updated 1 day ago," undated. | "not meant to be an effort to create a PC Port." SOURCED (same page). |
| **Super Smash Bros. Brawl** (Wii) | RSBE01 | Repo not found. decomp.dev listing excerpt: 1.00% / 0.87%, undated. https://scriptagc.wasmer.app/https_decomp_dev . Press: 1.09%, dated 22 Apr 2026 (Multiplayer.it excerpt). | not found |
| **Star Fox Adventures** (GC) | not found | Repo not found. Secondary table (heldgames.com, not opened): code fully matched, data ~99.96%, undated. | not found (only the unreleased N64 precursor Dinosaur Planet has a recompilation: https://readonlymemo.com/dinosaur-planet-recompiled-pc-port/ ) |
| **Mario Kart Wii** (Wii) | RMCE01 and others | No decomp repo found. A 2021 journal note mentions a disassembly, not updated since. https://gitdab.com/s/garden/src/branch/master/journal/2020-12-31.md . | Static recompilation **Mario Kart Wiicompiled** (patchzy). Announced 15 Jul 2026, public beta planned for August 2026. Beta status unknown as of today. Repo and license not found. Press: 4K, uncapped FPS, online play. https://www.ubergizmo.com/2026/07/mario-kart-wii-pc-port-first-beta-arrives-this-august/ (excerpt) |
| Xenoblade Chronicles (Wii) | not found | Listed on decomp.dev; figure cut off in excerpt. No figure. | not found |
| Wii Sports (Wii) | not found | not found | not found |
| F-Zero GX (GC) | not found | not found (search returned only F-Zero X, N64: https://github.com/inspectredc/fzerox ) | not found |
| Kirby Air Ride (GC) | not found | not found | not found |
| Sonic Adventure 2 Battle (GC) | not found | not found (search returned only Sonic Advance 2, GBA) | not found |
| Super Mario Galaxy 2 (Wii) | SB4E01 (Rev 0 USA) | SMGCommunity/Garigari (CC0 not verified). README names SB4E01. No figure found. https://gitblind.noratr.app/SMGCommunity/Garigari (excerpt) | "Syati" fork for modding; no port found |

Recompilation-only and tooling entries (not decompilations):
- **DolRecomp** (ExpansionPak): static recompiler for GC, Wii and experimental Wii U CPU code. Emits C. No runtime. Early (236 opcodes). License not found. Search excerpt: https://gbatemp.net/posts/10875919/ . Repo URL from excerpt: https://github.com/ExpansionPak/DolRecomp .
- **decomp-toolkit (dtk)**, **dtk-template**, **objdiff**: encounter's toolchain. The template README says nearly all active GC/Wii decomps use this structure. SOURCED (excerpt): https://git.axiodl.com/encounter/dtk-template . Also the Metaforce and PrimeDecomp repos are built on it.

## 2. Ranked shortlist for gc3ds native mode

Ranking criteria: a working port exists (or is closest); the decomp is near-complete; the license is GPL-2.0-compatible; and the 2DS plausibly runs it. Compatibility of MIT and CC0 with GPL-2.0 is INFERRED from general license knowledge, not sourced here.

1. **Animal Crossing (ac-decomp + ACGC-PC-Port).** The only GC port found with its own GX-to-OpenGL translation outside Aurora, on a CC0 decomp. Blockers: port license not identified; its GX-to-OpenGL 3.3 layer is not Aurora. Best first reference for a GX-to-PICA200 translation. Coverage: GAFE01_00 only.
2. **Twilight Princess (zeldaret/tp + Dusklight).** The most complete port: v2.0.0 released 19 Sep (year inferred as 2026), with a Switch fork. SOURCED: https://github.com/TwilitRealm/dusklight/releases . Sits on Aurora (MIT), CC0 decomp. Blockers: Dusklight's GPU floor (D3D12/Vulkan/Metal); TP is a large game for 124 MB (INFERRED). Highest long-term value because Aurora is shared (see section 3).
3. **Pikmin 1 (decomp 100%) and Pikmin 2 (99.63%).** Strongest matching decomps for small GC games. A Pikmin 1 PC build exists per one excerpt, but the repo and license are not identified. Pikmin 2 is CC0 and has no port found.
4. **Mario Party 4.** USA versions complete per README; an alpha port exists per press. Conflicting claim in the repo README. License unknown. Lower priority until the license is found.
5. **Metroid Prime (PrimeDecomp + Metaforce).** The Aurora origin project, MIT/CC0, but "Builds are currently unavailable." Strategic value (a Retro-engine reimplementation). Not yet playable.
6. **Super Mario Galaxy (Petari; Wii).** Decomp reported 100% (undated, conflicting). The port is unreleased, with no license or layer known. Wii RAM is the bigger problem (section 4).

Not shortlisted: matching-only decomps with no port and no near-term plan (Sunshine, Skyward Sword, TTYD, Double Dash, Melee, NSMBW, Brawl), and static recompilations (Mario Kart Wiicompiled, Wind Waker Recomp, DolRecomp) which are a different technique and cannot supply source.

## 3. Turning a matching decomp into a port

What a port needs (SOURCED from Aurora and Dusklight READMEs, INFERRED for the gc3ds-specific parts):
- **SDK reimplementations.** Aurora covers GX, PAD, DVD and CARD. SOURCED: https://github.com/encounter/aurora . The README does not state AX (audio) or OS coverage. Not found. Audio is unfinished in at least one port: the Mario Party 4 alpha has "no music or sound effects." SOURCED: generationamiga article above.
- **GX backend for PICA200.** This is the main work. Aurora's backends (D3D12, Vulkan, Metal, WebGPU/Dawn) do not run on PICA200. INFERRED.
- **ARAM emulation.** GC has a separate 16 MB ARAM, reached only through the DSP. WiiBrew: "The GameCube's 24 MB of 1T-SRAM ... Its 16 MB ARAM is a separate chip." SOURCED (search excerpt): https://wiibrew.org/wiki/Memory_map
- **Disc input.** Ports read the disc image at run time (SOURCED above). gc3ds needs readers for ISO/RVZ/WIA. The PrimeDecomp README lists ISO (GCM), RVZ, WIA, WBFS, CISO, NFS, GCZ and TGC as accepted disc formats. SOURCED: https://github.com/PrimeDecomp/prime
- **Shared-layer multiplier.** Aurora is the one layer that serves multiple projects (Dusklight, Metaforce origin, and "several completed source ports" per its README). SOURCED: https://github.com/encounter/aurora . The README names only Dusklight. Other Aurora ports were not found. A PICA200 backend for Aurora's GX would cover every Aurora port, so this is the highest-leverage piece. INFERRED.
- **Build structure multiplier.** dtk-template is shared across nearly all GC/Wii decomps (SOURCED excerpt above). A gc3ds loader can target the DOL/REL output once, not per game.

## 4. Wii-specific obstacles and the 2DS memory budget

- **Wii RAM.** 88 MB total: 24 MB MEM1 (1T-SRAM, inside Hollywood) plus 64 MB MEM2 (GDDR3). IOS reserves 12 to 16 MB of MEM2. WiiBrew: https://wiibrew.org/wiki/Memory_map (search excerpt). A native port does not run IOS, so the 88 MB is the ceiling, not the reservation (INFERRED).
- **Wii has no ARAM chip.** The DSP reads MEM1 or MEM2 directly. WiiBrew, same source. So a Wii port needs no separate 16 MB ARAM pool, but its game data competes with code and heap in one pool (INFERRED).
- **New 3DS application memory.** APPMEMTYPE 6 gives 0x07C00000 = 124 MB for applications. SOURCED: https://3dbrew.org/wiki/Memory_layout . The page covers New 3DS; it does not mention New 2DS XL. A secondary source gives the New 2DS XL as 256 MB FCRAM with 64 MB system reserved (search excerpt, https://tecnoblog.net/guias/comparativo-nintendo-2ds-vs-3ds-e-versoes-xl-qual-a-diferenca/ ). Treating the New 2DS XL as matching New 3DS is INFERRED.
- **Budget comparison.** GC: 24 + 16 MB = 40 MB. Wii: up to 88 MB. The 2DS budget is 124 MB. A Wii game at its full 88 MB leaves about 36 MB for code, GX/framebuffers, audio and the loader. INFERRED. Real Wii titles need measurement.
- **CPU.** Gekko 486 MHz (GC) and Broadway up to 729 MHz (Wii, downclocks to 486). SOURCED: https://dolphin-emu.org/blog/59 and https://wiibrew.org/wiki/Broadway (search excerpts). The 2DS CPU is an ARM11 at 804 MHz, a secondary source (https://www.smartworld.it/recensioni/new-nintendo-2ds-xl , search excerpt). PowerPC-to-ARM requires a full translation either way (INFERRED).
- **Wii-only code.** Wii discs in TP run GC-side behaviour under Dusklight. SOURCED: https://github.com/TwilitRealm/dusklight . No Wii-native decomp port was found for any title in this survey. Wii Sports, Xenoblade, Mario Kart Wii (decomp), Skyward Sword and Galaxy 2 have no port found. The Wii-specific SDK parts (WPAD, IOS file access) are not sourced here and need their own survey.

## 5. Weak-hardware ports found

- Switch: dusklight-NX (fork, section 1). SOURCED.
- Android: Dusklight (Vulkan and experimental GLES). SOURCED: https://github.com/TwilitRealm/dusklight/releases
- 3DS, PS Vita, Wii U: none found for any GC/Wii decomp. The Aurora README lists none of them. SOURCED: https://github.com/encounter/aurora

## 6. Not found / unverified (searched, nothing usable)

- Kirby Air Ride, F-Zero GX, Sonic Adventure 2 Battle, Wii Sports, Xenoblade progress figure, Mario Kart Wii decomp repo, Brawl decomp repo, Star Fox Adventures repo, Pikmin 1 repo (not opened), Mario Party 4 license, AstroCore license and layer.
- Wind Waker Recomp license (GPL-3.0 per Trendshift is unverified).
- The "more than 80 projects" count on decomp.dev. The home page fetched says the index is disabled. The count is from a search summary and is not verified here.
- A current complete list of GC/Wii decomps. None found in this pass; try the awesome-game-decompilations list (not opened, referenced in aggregator pages) and the zeldaret and doldecomp GitHub orgs directly.

## 7. Next steps for gc3ds (recommended)

1. Open flyngmt/ACGC-PC-Port and TwilitRealm/dusklight LICENSE files directly to settle license compatibility before any code reuse.
2. Check the Aurora GX source for its exact backend interface, to size a PICA200 backend. Not done here.
3. Confirm the SMG (Galaxy) and Mario Party 4 progress figures from the repos, since the sources conflict.
4. Measure a real Wii disc's RAM use before committing to any Wii title.
