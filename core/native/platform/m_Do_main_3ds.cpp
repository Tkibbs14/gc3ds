// gc3ds native mode: the game's entry point and main loop on the 3DS, in place of Dusklight's src/m_Do/m_Do_main.cpp
// (which the native Makefile leaves out). Same start-up order and the same loop as Dusklight's game_main() / main01(),
// without its PC layer: no command line, window, SDL events, ImGui, RmlUi menus, mods, Discord or crash reporting.
// The globals and helpers below are Dusklight's (CC0), carried over unchanged unless a comment says otherwise.
#include "m_Do/m_Do_main.h"
#include "DynamicLink.h"
#include "JSystem/JFramework/JFWSystem.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/JUtility/JUTConsole.h"
#include "c/c_dylink.h"
#include "d/d_com_inf_game.h"
#include "d/actor/d_a_movie_player.h"
#include "f_ap/f_ap_game.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_dvd_thread.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_machine.h"
#include "SSystem/SComponent/c_API_graphic.h"
#include "f_pc/f_pc_manager.h"

#include "dusk/game_clock.h"
#include "dusk/interp/frame_interpolation.h"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "dusk/version.hpp"

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <cstring>

#include "native.h"

DUSK_GAME_DATA s8 mDoMain::developmentMode = -1;
DUSK_GAME_DATA OSTime mDoMain::sPowerOnTime;
DUSK_GAME_DATA OSTime mDoMain::sHungUpTime;
DUSK_GAME_DATA u32 mDoMain::memMargin = 0xFFFFFFFF;
DUSK_GAME_DATA char mDoMain::COPYDATE_STRING[18] = "??/??/?? ??:??:??";
// As the GameCube build: Dusklight doubles it on PC (TARGET_PC), where structures grow with 64-bit pointers.
const int audioHeapSize = 0x14D800;

#define COPYDATE_PATH "/str/Final/Release/COPYDATE"

s32 LOAD_COPYDATE(void*) {
    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    DVDFileInfo fi;
    if (DVDOpen(COPYDATE_PATH, &fi)) {
        u32 readLen = (fi.length < sizeof(buffer) - 1) ? fi.length : sizeof(buffer) - 1;
        u32 alignedLen = (readLen + 31) & ~31;  // DVDReadPrio wants a 32-byte aligned buffer and length
        alignas(32) char readBuf[64];
        DVDReadPrio(&fi, readBuf, alignedLen, 0, 2);
        DVDClose(&fi);
        memcpy(buffer, readBuf, readLen);
        buffer[readLen] = '\0';
    } else {
        strcpy(buffer, "GC3DS NATIVE");
    }
    memcpy(mDoMain::COPYDATE_STRING, buffer, sizeof(mDoMain::COPYDATE_STRING) - 1);
    mDoMain::COPYDATE_STRING[sizeof(mDoMain::COPYDATE_STRING) - 1] = '\0';
    return 1;
}

AuroraInfo auroraInfo;

static u8 selectedLanguage;
u8 OSGetLanguage() { return selectedLanguage; }

static void LanguageInit() {
    if (!dusk::version::isRegionPal()) return;  // English unless a PAL disc
    selectedLanguage = static_cast<u8>(dusk::getSettings().game.language.getValue());
}

// The game loop (Dusklight's main01): one game tick per frame at the game's own 30 Hz, no frame interpolation.
void main01(void) {
    mDoMch_Create();
    native::trace("mDoMch_Create done");
    mDoGph_Create();
    native::trace("mDoGph_Create done");
    mDoCPd_c::create();
    native::trace("mDoCPd_c::create done");
    JUTConsole* console = JFWSystem::getSystemConsole();
    if (console) {
        console->setOutput(mDoMain::developmentMode ? JUTConsole::OUTPUT_OSR_AND_CONSOLE : JUTConsole::OUTPUT_NONE);
        console->setPosition(32, 42);
    }
    mDoDvdThd_callback_c::create((mDoDvdThd_callback_func)LOAD_COPYDATE, NULL);
    native::trace("mDoDvdThd_callback_c::create done");
    fapGm_Create();
    native::trace("fapGm_Create done");
    fopAcM_initManager();
    native::trace("fopAcM_initManager done");
    cDyl_InitAsync();
    native::trace("cDyl_InitAsync done");
    g_mDoAud_audioHeap = JKRCreateSolidHeap(audioHeapSize, JKRGetCurrentHeap(), false);
    native::trace("JKRCreateSolidHeap done");
    JKRHEAP_NAME(g_mDoAud_audioHeap, "g_mDoAud_audioHeap");
    native::trace("JKRHEAP_NAME done");
    if (!native::audio_enabled()) {
        mDoAud_zelAudio_c::onInitFlag();  // as Dusklight's DUSK_AUDIO_DISABLED: boot as if the audio engine were up
    }
    dusk::game_clock::initialize();
    native::trace("dusk::game_clock::initialize done");
    do {
        native::poll_events();
        static bool first = true;
        if (first) native::trace("loop entered");
        if (!aurora_begin_frame()) continue;
        VIWaitForRetrace();
        native::frame_begin();
        mDoGph_gInf_c::updateRenderSize();
        const auto timing = dusk::game_clock::advance();
        (void)timing;
        dusk::interp::begin_frame(0.0f);
        dusk::game_clock::begin_sim_tick();
        mDoCPd_c::read();
        if (first) native::trace("fapGm_Execute...");
        fapGm_Execute();  // game logic and drawing (mDoGph_Painter -> JFWDisplay -> GX)
        if (native::audio_enabled()) mDoAud_Execute();
        dusk::game_clock::commit_sim_tick();
        native::frame_logic_done();
        first = false;
        aurora_end_frame();
        native::frame_end();
        static unsigned frames = 0;
        if (++frames <= 5 || frames % 60 == 0) native::trace("frame %u", frames);
    } while (dusk::IsRunning && !native::exit_requested());
    native::trace("} while done");
}

// Dusklight's game_main without its PC set-up: settings and data paths from the SD card, the disc gc3ds picked, then
// the same start-up order (version, language, OSInit, reset data, game context) and the loop.
// Link's position for native_3ds.cpp's per-second line (does scripted input reach the game? job 211's GX stream was
// the same size every second). false before the player actor exists.
bool native_player_pos(float* x, float* y, float* z) {
    fopAc_ac_c* p = dComIfGp_getPlayer(0);
    if (!p) return false;
    *x = p->current.pos.x;
    *y = p->current.pos.y;
    *z = p->current.pos.z;
    return true;
}
// Whether an event (cutscene, scripted entrance) holds the player: the field boot's Link never moved (job 211).
int native_event_running() { return dComIfGp_event_runCheck() ? 1 : 0; }

int game_main(int argc, char* argv[]) {
    dusk::registerSettings();
    native::trace("dusk::registerSettings done");
    native::init_data_paths();
    native::trace("native::init_data_paths done");
    native::apply_settings();  // frame interpolation off, the game's own frame rate
    AuroraConfig config{};
    config.appName = "gc3ds";
    config.mem1Size = native::arena_bytes();
    config.mem2Size = 16u << 20;  // ARAM, as the GameCube's
    auroraInfo = aurora_initialize(argc, argv, &config);
    native::trace("aurora_initialize done");
    if (!aurora_dvd_open(native::disc_path())) {
        native::fatal("cannot open the disc image");
        return 1;
    }
    dusk::IsGameLaunched = true;
    native::trace("true; done");
    dusk::version::init();
    native::trace("dusk::version::init done");
    LanguageInit();
    native::trace("LanguageInit done");
    OSInit();
    native::trace("OSInit done");
    native::trace_memory("after OSInit (the 64 MB arena)");
    mDoMain::sPowerOnTime = OSGetSystemTime();
    native::trace("OSGetSystemTime done");
    static mDoRstData sResetData = {0};
    mDoRst::setResetData(&sResetData);
    native::trace("mDoRst::setResetData done");
    mDoRst::offReset();
    native::trace("mDoRst::offReset done");
    mDoRst::setLogoScnFlag(0);
    native::trace("mDoRst::setLogoScnFlag done");
    dComIfG_ct();
    native::trace("dComIfG_ct done");
    mDoDvdThd::SyncWidthSound = false;
    native::trace("false; done");
    main01();
    native::trace("main01 done");
    if (daMP_c::m_myObj) daMP_c::m_myObj->daMP_c_Finish();
    native::trace("if done");
    mDoMch_Destroy();
    native::trace("mDoMch_Destroy done");
    OSResetSystem(OS_RESET_SHUTDOWN, 0, 0);
    native::trace("OSResetSystem done");
    aurora_shutdown();
    native::trace("aurora_shutdown done");
    return 0;
}
