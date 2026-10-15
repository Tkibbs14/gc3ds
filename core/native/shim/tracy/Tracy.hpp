// Native mode (gc3ds): the Tracy profiler's macros compiled out.
#pragma once
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedC(color)
#define ZoneScopedNC(name, color)
#define ZoneNamed(var, active)
#define ZoneNamedN(var, name, active)
#define ZoneText(text, size)
#define ZoneName(text, size)
#define ZoneValue(value)
#define FrameMark
#define FrameMarkNamed(name)
#define FrameMarkStart(name)
#define FrameMarkEnd(name)
#define TracyPlot(name, value)
#define TracyMessage(text, size)
#define TracyMessageL(text)
#define TracyAlloc(ptr, size)
#define TracyFree(ptr)
#define TracyLockable(type, var) type var
#define LockableBase(type) type
#define TracyCZoneN(ctx, name, active)
#define TracyCZoneEnd(ctx)
