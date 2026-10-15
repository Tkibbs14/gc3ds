// gc3ds native mode: what the game's entry point (m_Do_main_3ds.cpp) needs from the platform (the 3DS, or the PC
// build used under qemu). One implementation per platform.
#pragma once
#include <cstdint>

namespace native {
void init_data_paths();   // dusk::ConfigPath / CachePath on the platform's storage (the SD card on the 3DS)
void apply_settings();    // settings the platform requires (no frame interpolation: the game's own 30 Hz)
uint32_t arena_bytes();   // the arena the game's heaps are carved from (OSGetArenaLo / Hi)
const char* disc_path();  // the disc image gc3ds picked
bool audio_enabled();     // false: boot as if the audio engine were up, and skip mDoAud_Execute (feasibility build)
void poll_events();       // input and system events (HID, the home menu, sleep)
bool exit_requested();
// Frame timing, the feasibility measurement: per frame, the game's own work (logic and GX submission) between
// frame_begin and frame_logic_done, and the whole frame to frame_end.
void frame_begin();
void frame_logic_done();
void frame_end();
void trace(const char* fmt, ...);  // sdmc:/gc3ds/native/trace.txt, flushed at once (where start-up got to)
void trace_memory(const char* where);  // into the trace: the largest malloc block and the heaps at this point
[[noreturn]] void fatal(const char* message);
// Scripted input (--press NAME@FROM-TO,...: frames since the game loop started), ORed into the pad each frame: the 3DS
// key bits held now, and the circle pad (-156..156) when a stick direction is scripted (*stick false otherwise).
uint32_t scripted_keys();
bool scripted_stick(int* dx, int* dy);
}  // namespace native
