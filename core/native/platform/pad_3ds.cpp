// gc3ds native mode: the GameCube pad on the 3DS's controls (Aurora's pad layer is SDL's). Port 0 only.
//   A B X Y START -> the same; L R -> the analog triggers, fully pressed; ZL / ZR (New 3DS) -> Z; D-pad -> D-pad;
//   circle pad -> main stick; C-stick (New 3DS) -> C-stick. The circle pad's range (about +-156) is scaled to the
//   GameCube stick's (about +-100 after PADClamp).
#include <3ds.h>

#include <dolphin/pad.h>

#include "native.h"

#include <cstdlib>
#include <cstring>

static s8 scale_stick(int v) {
    int s = v * 100 / 156;
    return (s8)(s > 127 ? 127 : s < -128 ? -128 : s);
}

BOOL PADInit() { return TRUE; }
BOOL PADReset(u32) { return TRUE; }
BOOL PADRecalibrate(u32) { return TRUE; }
void PADSetSpec(u32) {}
void PADSetAnalogMode(u32) {}
void PADControlMotor(u32, u32) {}

u32 PADRead(PADStatus* status) {
    for (int i = 1; i < 4; i++) {
        memset(&status[i], 0, sizeof status[i]);
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    PADStatus& s = status[0];
    memset(&s, 0, sizeof s);
    const u32 held = hidKeysHeld() | native::scripted_keys();  // --press: scripted buttons (automated runs)
    if (held & KEY_A) s.button |= PAD_BUTTON_A;
    if (held & KEY_B) s.button |= PAD_BUTTON_B;
    if (held & KEY_X) s.button |= PAD_BUTTON_X;
    if (held & KEY_Y) s.button |= PAD_BUTTON_Y;
    if (held & KEY_START) s.button |= PAD_BUTTON_START;
    if (held & KEY_DUP) s.button |= PAD_BUTTON_UP;
    if (held & KEY_DDOWN) s.button |= PAD_BUTTON_DOWN;
    if (held & KEY_DLEFT) s.button |= PAD_BUTTON_LEFT;
    if (held & KEY_DRIGHT) s.button |= PAD_BUTTON_RIGHT;
    if (held & (KEY_ZL | KEY_ZR)) s.button |= PAD_TRIGGER_Z;
    if (held & KEY_L) {
        s.button |= PAD_TRIGGER_L;
        s.triggerLeft = 255;
    }
    if (held & KEY_R) {
        s.button |= PAD_TRIGGER_R;
        s.triggerRight = 255;
    }
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    int sdx, sdy;
    if (native::scripted_stick(&sdx, &sdy)) { cp.dx = (s16)sdx; cp.dy = (s16)sdy; }
    s.stickX = scale_stick(cp.dx);
    s.stickY = scale_stick(cp.dy);
    s.substickX = scale_stick(cs.dx);
    s.substickY = scale_stick(cs.dy);
    s.analogA = (held & KEY_A) ? 255 : 0;
    s.analogB = (held & KEY_B) ? 255 : 0;
    s.err = PAD_ERR_NONE;
    return 1u << 31;  // PAD_CHAN0_BIT: port 0 is connected
}

// The SDK's clamps: the stick to an octagon-ish range, the triggers above their dead zone. The scaled 3DS ranges are
// already inside, so these only cut the dead zones.
static void clamp_axis(s8& x, s8& y, int dead, int max) {
    int ax = abs(x) < dead ? 0 : x, ay = abs(y) < dead ? 0 : y;
    x = (s8)(ax > max ? max : ax < -max ? -max : ax);
    y = (s8)(ay > max ? max : ay < -max ? -max : ay);
}
void PADClamp(PADStatus* status) {
    for (int i = 0; i < 4; i++) {
        if (status[i].err != PAD_ERR_NONE) continue;
        clamp_axis(status[i].stickX, status[i].stickY, 15, 72 + 15);
        clamp_axis(status[i].substickX, status[i].substickY, 15, 59 + 15);
    }
}
void PADClampCircle(PADStatus* status) { PADClamp(status); }
