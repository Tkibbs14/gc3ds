// Native mode (gc3ds): Aurora's event.h names the joystick id type; no SDL joysticks on the 3DS.
#pragma once
#include <stdint.h>
typedef uint32_t SDL_JoystickID;
