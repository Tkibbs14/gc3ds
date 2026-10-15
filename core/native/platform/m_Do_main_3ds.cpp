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
    mDoGph_Create();
    mDoCPd_c::create();
    JUTConsole* console = JFWSystem::getSystemConsole();
    if (console) {
        console->setOutput(mDoMain::developmentMode ? JUTConsole::OUTPUT_OSR_AND_CONSOLE : JUTConsole::OUTPUT_NONE);
        console->setPosition(32, 42);
    }
    mDoDvdThd_callback_c::create((mDoDvdThd_callback_func)LOAD_COPYDATE, NULL);
    fapGm_Create();
    fopAcM_initManager();
    cDyl_InitAsync();
    g_mDoAud_audioHeap = JKRCreateSolidHeap(audioHeapSize, JKRGetCurrentHeap(), false);
    JKRHEAP_NAME(g_mDoAud_audioHeap, "g_mDoAud_audioHeap");
    if (!native::audio_enabled()) {
        mDoAud_zelAudio_c::onInitFlag();  // as Dusklight's DUSK_AUDIO_DISABLED: boot as if the audio engine were up
    }
    dusk::game_clock::initialize();
    do {
        native::poll_events();
        if (!aurora_begin_frame()) continue;
        VIWaitForRetrace();
        native::frame_begin();
        mDoGph_gInf_c::updateRenderSize();
        const auto timing = dusk::game_clock::advance();
        (void)timing;
        dusk::interp::begin_frame(0.0f);
        dusk::game_clock::begin_sim_tick();
        mDoCPd_c::read();
        fapGm_Execute();  // game logic and drawing (mDoGph_Painter -> JFWDisplay -> GX)
        if (native::audio_enabled()) mDoAud_Execute();
        dusk::game_clock::commit_sim_tick();
        native::frame_logic_done();
        aurora_end_frame();
        native::frame_end();
    } while (dusk::IsRunning && !native::exit_requested());
}

// Dusklight's game_main without its PC set-up: settings and data paths from the SD card, the disc gc3ds picked, then
// the same start-up order (version, language, OSInit, reset data, game context) and the loop.
int game_main(int argc, char* argv[]) {
    dusk::registerSettings();
    native::init_data_paths();
    native::apply_settings();  // frame interpolation off, the game's own frame rate
    AuroraConfig config{};
    config.appName = "gc3ds";
    config.mem1Size = native::arena_bytes();
    config.mem2Size = 0;
    auroraInfo = aurora_initialize(argc, argv, &config);
    if (!aurora_dvd_open(native::disc_path())) {
        native::fatal("cannot open the disc image");
        return 1;
    }
    dusk::IsGameLaunched = true;
    dusk::version::init();
    LanguageInit();
    OSInit();
    mDoMain::sPowerOnTime = OSGetSystemTime();
    static mDoRstData sResetData = {0};
    mDoRst::setResetData(&sResetData);
    mDoRst::offReset();
    mDoRst::setLogoScnFlag(0);
    dComIfG_ct();
    mDoDvdThd::SyncWidthSound = false;
    main01();
    if (daMP_c::m_myObj) daMP_c::m_myObj->daMP_c_Finish();
    mDoMch_Destroy();
    OSResetSystem(OS_RESET_SHUTDOWN, 0, 0);
    aurora_shutdown();
    return 0;
}
