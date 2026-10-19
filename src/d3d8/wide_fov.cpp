#include "wide_fov.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "core/projection.h"

namespace gp4u {

const uint8_t kWideFovSite[12] = {0xD9, 0xE8,                               // fld1
                                  0xD9, 0xF3,                               // fpatan
                                  0xDC, 0xC0,                               // fadd st0, st0
                                  0xD9, 0x1D, 0x90, 0x32, 0xA5, 0x00};      // fstp [0xa53290]

WideFovTargets gp4_wide_fov_targets() { return {0x493ECF, 0xA532A0, 0xA53290, 0xA532D0}; }

namespace {
WideFovTargets g_t{};
double g_ref = 16.0 / 9.0;
double g_zoom = 1.0;
unsigned g_calls = 0;
}  // namespace

}  // namespace gp4u

// Called by the stub (cdecl, no arguments; the x87 stack holds nothing of GP4's).
extern "C" void gp4u_wide_fov_c() __asm__("gp4u_wide_fov_c");
extern "C" __attribute__((used)) void gp4u_wide_fov_c() {
    using namespace gp4u;
    float* h = (float*)(uintptr_t)g_t.hfov;
    float* v = (float*)(uintptr_t)g_t.vfov;
    float a = *(float*)(uintptr_t)g_t.aspect;
    CameraFov f = hor_plus_fov(*h, a, g_ref, g_zoom);
    *v = (float)f.v;
    *h = (float)f.h;
    ++g_calls;
}

extern "C" {
// The near plane GP4 keeps on the x87 stack, saved with its full 80-bit precision.
__attribute__((used)) unsigned char gp4u_wide_fov_save[16] __asm__("gp4u_wide_fov_save");
}
extern "C" void gp4u_wide_fov_stub() __asm__("gp4u_wide_fov_stub");

// On entry st0 = tan(h/2)/aspect (GP4's half-done result, dropped) and st1 = the near plane.
__asm__(
    ".text\n"
    ".globl gp4u_wide_fov_stub\n"
    "gp4u_wide_fov_stub:\n"
    "    fstp %st(0)\n"
    "    fstpt gp4u_wide_fov_save\n"
    "    pushfl\n"
    "    pushal\n"
    "    cld\n"
    "    call gp4u_wide_fov_c\n"
    "    popal\n"
    "    popfl\n"
    "    fldt gp4u_wide_fov_save\n"
    "    ret\n");

namespace gp4u {

void wide_fov_configure(const WideFovTargets& t, double ref_aspect, double zoom) {
    g_t = t;
    g_ref = ref_aspect;
    g_zoom = zoom;
}

void* wide_fov_stub() { return (void*)&gp4u_wide_fov_stub; }
unsigned wide_fov_calls() { return g_calls; }

bool install_wide_fov(const WideFovTargets& t, double ref_aspect, std::string* why, double zoom) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    uint8_t cur[sizeof kWideFovSite];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)t.site, cur, sizeof cur, &got) || got != sizeof cur)
        return fail("code not readable (not GP4?)");
    if (std::memcmp(cur, kWideFovSite, sizeof cur)) return fail("camera code differs (other GP4 build or already patched)");
    wide_fov_configure(t, ref_aspect, zoom);
    uint8_t patch[sizeof kWideFovSite];
    std::memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)&gp4u_wide_fov_stub - (t.site + 5));
    std::memcpy(patch + 1, &rel, 4);
    DWORD old;
    if (!VirtualProtect((LPVOID)(uintptr_t)t.site, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) return fail("VirtualProtect failed");
    std::memcpy((void*)(uintptr_t)t.site, patch, sizeof patch);
    VirtualProtect((LPVOID)(uintptr_t)t.site, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)t.site, sizeof patch);
    return true;
}

}  // namespace gp4u
