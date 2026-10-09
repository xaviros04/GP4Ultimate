// Common DirectInput includes and helpers for the mod's Windows layer.
#pragma once
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include <cstdint>
#include <string>

namespace gp4u {

typedef HRESULT(WINAPI* DI8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

// DIJOYSTATE2 position block, built locally so this DLL never links against dinput8.lib
// (it *is* dinput8.dll; importing from itself would bind to itself).
const DIDATAFORMAT* joystate2_format();

// Byte offset of a DIJOYSTATE2 axis (X..S1) in our own format.
inline DWORD joystate2_axis_offset(int axis) {
    return axis < 6 ? (DWORD)(axis * 4) : (DWORD)(FIELD_OFFSET(DIJOYSTATE2, rglSlider) + (axis - 6) * 4);
}

const GUID& axis_guid(int axis);

// DIPROP_* are small integers (1, 4, 5, 9, ...) cast to GUID references. Comparing
// `&rguid == &DIPROP_X` directly lets the optimiser assume the reference is GUID-aligned
// and fold the comparison for unaligned ids to false (seen with clang: DEADZONE, BUFFERSIZE
// and AUTOCENTER were silently ignored). The volatile read hides the value from it.
inline uintptr_t prop_id(REFGUID g) {
    const GUID* volatile p = &g;
    return (uintptr_t)p;
}

// Name copy into A or W fixed buffers.
inline void set_name(char* dst, size_t n, const std::string& s) {
    lstrcpynA(dst, s.c_str(), (int)n);
}
inline void set_name(wchar_t* dst, size_t n, const std::string& s) {
    int r = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, dst, (int)n);
    if (r <= 0 && n) dst[0] = 0;
    if (n) dst[n - 1] = 0;
}

}  // namespace gp4u
