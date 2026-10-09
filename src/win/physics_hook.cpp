#include "physics_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace gp4u {

namespace {
PhysTargets g_t{};
bool g_installed = false;
volatile LONG g_seq = 0;
PhysSnap g_snap;

constexpr uint32_t kStride = 0x300, kFx = 0x1A0, kFy = 0xE0, kMz = 0x274;
}  // namespace

PhysTargets gp4_phys_targets() { return {0x41874A, 0x418942, 0x66D820, 0x6380D8, 0x623EA0, 0x623EA8}; }

bool physics_hook_installed() { return g_installed; }

}  // namespace gp4u

// Called by the stub after GP4's own end-of-step function, with the car record in esi.
extern "C" void __cdecl gp4u_after_car_step(const uint8_t* car) {
    using namespace gp4u;
    uint32_t player = *(const volatile uint32_t*)(uintptr_t)g_t.player_ptr;
    if (!player || (uint32_t)(uintptr_t)car != player) return;
    InterlockedIncrement(&g_seq);   // odd: writing
    MemoryBarrier();
    for (int w = 0; w < 4; ++w) {
        const uint8_t* b = (const uint8_t*)(uintptr_t)(g_t.wheel_block + w * kStride);
        std::memcpy(&g_snap.fx[w], b + kFx, 4);
        std::memcpy(&g_snap.fy[w], b + kFy, 4);
        std::memcpy(&g_snap.mz[w], b + kMz, 4);
    }
    std::memcpy(&g_snap.g_lat, (const void*)(uintptr_t)g_t.g_lat, 4);
    std::memcpy(&g_snap.g_long, (const void*)(uintptr_t)g_t.g_long, 4);
    ++g_snap.steps;
    MemoryBarrier();
    InterlockedIncrement(&g_seq);   // even: done
}

extern "C" uint32_t gp4u_step_fn = 0;   // GP4's original callee, called indirectly

#if defined(_MSC_VER)
extern "C" __declspec(naked) void gp4u_step_stub() {
    __asm {
        call dword ptr [gp4u_step_fn]
        pushfd
        pushad
        push esi
        call gp4u_after_car_step
        add esp, 4
        popad
        popfd
        ret
    }
}
#else
extern "C" void gp4u_step_stub();
asm(".text\n"
    ".globl _gp4u_step_stub\n"
    "_gp4u_step_stub:\n"
    "    call *_gp4u_step_fn\n"
    "    pushfl\n"
    "    pushal\n"
    "    pushl %esi\n"
    "    call _gp4u_after_car_step\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n");
#endif

namespace gp4u {

bool install_physics_hook(const PhysTargets& t, std::string* why) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    uint8_t cur[5];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)t.call_site, cur, 5, &got) || got != 5)
        return fail("code not readable (not GP4?)");
    int32_t rel = (int32_t)(t.step_fn - (t.call_site + 5));
    uint8_t want[5] = {0xE8};
    std::memcpy(want + 1, &rel, 4);
    if (std::memcmp(cur, want, 5) != 0) return fail("original call not found (other GP4 build or already hooked)");
    g_t = t;
    gp4u_step_fn = t.step_fn;
    int32_t nrel = (int32_t)((uintptr_t)&gp4u_step_stub - (t.call_site + 5));
    uint8_t patch[5] = {0xE8};
    std::memcpy(patch + 1, &nrel, 4);
    DWORD old;
    if (!VirtualProtect((LPVOID)(uintptr_t)t.call_site, 5, PAGE_EXECUTE_READWRITE, &old)) return fail("VirtualProtect failed");
    std::memcpy((void*)(uintptr_t)t.call_site, patch, 5);
    VirtualProtect((LPVOID)(uintptr_t)t.call_site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)t.call_site, 5);
    g_installed = true;
    return true;
}

bool physics_snapshot(PhysSnap& out) {
    for (int tries = 0; tries < 100; ++tries) {
        LONG a = g_seq;
        if (a & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        PhysSnap copy;
        std::memcpy(&copy, (const void*)&g_snap, sizeof copy);
        MemoryBarrier();
        if (g_seq == a) {
            out = copy;
            return copy.steps > 0;
        }
    }
    return false;
}

}  // namespace gp4u
