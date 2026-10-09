#include "cockpit_patch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "core/cockpit.h"

namespace gp4u {

namespace {
double g_lock_deg = 0;
CockpitTargets g_t{};
}  // namespace

CockpitTargets gp4_cockpit_targets() {
    return {cockpit::kFunc, cockpit::kMulA, cockpit::kLimit, cockpit::kMulB};
}

}  // namespace gp4u

// Called by the thunk with the game's esi (the car record). cdecl: preserves ebx, esi,
// edi, ebp; the thunk saves ecx/edx like the original (which only changes eax and flags).
extern "C" int32_t __cdecl gp4u_wheel_angle(const uint8_t* car) {
    using namespace gp4u;
    cockpit::Consts k;
    std::memcpy(&k.mul_a, (const void*)(uintptr_t)g_t.mul_a, 4);
    std::memcpy(&k.limit, (const void*)(uintptr_t)g_t.limit, 4);
    std::memcpy(&k.mul_b, (const void*)(uintptr_t)g_t.mul_b, 4);
    return cockpit::patched_angle(car, k, g_lock_deg);
}

#if defined(_MSC_VER)
extern "C" __declspec(naked) void gp4u_wheel_thunk() {
    __asm {
        push ecx
        push edx
        push esi
        call gp4u_wheel_angle
        add esp, 4
        pop edx
        pop ecx
        ret
    }
}
#else
extern "C" void gp4u_wheel_thunk();
asm(".text\n"
    ".globl _gp4u_wheel_thunk\n"
    "_gp4u_wheel_thunk:\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl %esi\n"
    "    call _gp4u_wheel_angle\n"
    "    addl $4, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    ret\n");
#endif

namespace gp4u {

bool install_cockpit_wheel(double lock_to_lock_deg, const CockpitTargets& t, std::string* why) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (!(lock_to_lock_deg > 0)) return fail("disabled");
    uint8_t cur[sizeof cockpit::kSignature];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)t.func, cur, sizeof cur, &got) || got != sizeof cur)
        return fail("code not readable (not GP4?)");
    if (std::memcmp(cur, cockpit::kSignature, sizeof cur) != 0) return fail("original bytes differ (other GP4 build or already patched)");
    g_t = t;
    g_lock_deg = lock_to_lock_deg;
    uint8_t jmp[5] = {0xE9};
    int32_t rel = (int32_t)((uintptr_t)&gp4u_wheel_thunk - (uintptr_t)(t.func + 5));
    std::memcpy(jmp + 1, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)t.func, sizeof jmp, PAGE_EXECUTE_READWRITE, &old)) return fail("VirtualProtect failed");
    std::memcpy((void*)(uintptr_t)t.func, jmp, sizeof jmp);
    VirtualProtect((LPVOID)(uintptr_t)t.func, sizeof jmp, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)t.func, sizeof jmp);
    return true;
}

}  // namespace gp4u
