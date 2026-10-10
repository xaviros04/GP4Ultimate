#include "sound_fix.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <mutex>

#include "log.h"

namespace gp4u {

namespace {

constexpr uint32_t kFlag = 0x3C;       // busy flag (byte)
constexpr uint32_t kBuffers = 0x25C;   // IDirectSoundBuffer* table
constexpr double kWaitMs = 500;        // longest wait for the other thread's hold

// Which thread holds each object's flag (GP4 has one or two of these objects).
std::mutex g_owner_mtx;
struct Owner { void* obj; DWORD tid; } g_owner[8];

DWORD owner_of(void* obj) {
    std::lock_guard<std::mutex> lk(g_owner_mtx);
    for (auto& o : g_owner)
        if (o.obj == obj) return o.tid;
    return 0;
}
void set_owner(void* obj, DWORD tid) {
    std::lock_guard<std::mutex> lk(g_owner_mtx);
    Owner* free_slot = nullptr;
    for (auto& o : g_owner) {
        if (o.obj == obj) { o.tid = tid; return; }
        if (!o.obj && !free_slot) free_slot = &o;
    }
    if (free_slot) *free_slot = {obj, tid};
}

bool try_take(uint8_t* self) {
    volatile char* f = (volatile char*)(self + kFlag);
#if defined(_MSC_VER)
    return _InterlockedCompareExchange8(f, 1, 0) == 0;
#else
    char expected = 0;
    return __atomic_compare_exchange_n(f, &expected, (char)1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}

double ms_now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

// Takes the flag, waiting for another thread's hold; false on recursion or timeout.
bool take(uint8_t* self, bool fail_if_mine) {
    DWORD me = GetCurrentThreadId();
    double start = -1;
    for (int i = 0;; ++i) {
        if (try_take(self)) {
            set_owner(self, me);
            return true;
        }
        if (owner_of(self) == me) return !fail_if_mine;   // held by this thread already
        if (start < 0) start = ms_now();
        else if (ms_now() - start > kWaitMs) return false;
        if (i < 50) SwitchToThread();
        else Sleep(1);
    }
}

unsigned g_waits_failed = 0;

// thiscall functions with no stack arguments: __fastcall with this in ecx.
uint32_t __fastcall lock_fn(uint8_t* self, void*) {
    if (take(self, true)) return 0;
    if (++g_waits_failed <= 5) logf("sound: busy flag still held after %.0f ms (GP4 skips this sound change)", kWaitMs);
    return 5;   // what GP4's own version returns when busy
}

void __fastcall set_fn(uint8_t* self, void*) {
    if (!take(self, false)) self[kFlag] = 1;   // as GP4 did (it never waited)
}

void __fastcall unlock_fn(uint8_t* self, void*) {
    set_owner(self, 0);
    *(volatile uint8_t*)(self + kFlag) = 0;
}

typedef HRESULT(__stdcall* GetVolumeFn)(void*, LONG*);
typedef HRESULT(__stdcall* SetVolumeFn)(void*, LONG);

// GP4's SetVolume (0x5ad730), with the slot read once more right before it is used.
uint32_t __fastcall setvol_fn(uint8_t* self, void*, uint32_t slot, uint32_t volume) {
    uint32_t vol = volume & 0xFFFF;
    if (vol > 10000) vol = 10000;
    uint32_t i = slot & 0xFFFF;
    void** table = *(void***)(self + kBuffers);
    void* buf = table ? table[i] : nullptr;
    if (!buf) return 0xBBF;
    LONG cur = (LONG)vol;   // GP4 reuses the argument's stack slot for the answer
    ((GetVolumeFn)(*(void***)buf)[6])(buf, &cur);
    LONG target = -(LONG)vol;
    if (cur == target) return 0;
    table = *(void***)(self + kBuffers);
    buf = table ? table[i] : nullptr;
    if (!buf) return 0xBBF;   // freed meanwhile: GP4 crashed here
    HRESULT hr = ((SetVolumeFn)(*(void***)buf)[15])(buf, target);
    return hr != 0 ? 1 : 0;
}

bool readable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return a + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

void put(uint32_t slot, void* fn) {
    DWORD old;
    VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &old);
    *(void**)(uintptr_t)slot = fn;
    VirtualProtect((void*)(uintptr_t)slot, 4, old, &old);
}

}  // namespace

SoundFixTargets gp4_sound_fix_targets() { return {0x5F9C80, 0x5F9F54, 0x5A85A0, 0x5A85E0, 0x5A85D0, 0x5AD730}; }

bool install_sound_fix(const SoundFixTargets& t, std::string* why) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (!readable(t.vtable, 0xA0)) return fail("vtable not readable (not GP4?)");
    const uint32_t* v = (const uint32_t*)(uintptr_t)t.vtable;
    if (v[0x0C / 4] != t.lock_fn || v[0x10 / 4] != t.set_fn || v[0x14 / 4] != t.unlock_fn || v[0x9C / 4] != t.setvol_fn)
        return fail("vtable differs (other GP4 build or already patched)");
    if (t.vtable2_setvol && (!readable(t.vtable2_setvol, 4) || *(const uint32_t*)(uintptr_t)t.vtable2_setvol != t.setvol_fn))
        return fail("second vtable differs");
    static const uint8_t setvol_start[] = {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x66, 0x81, 0xFF, 0x10, 0x27};
    if (!readable(t.setvol_fn, sizeof setvol_start) || std::memcmp((void*)(uintptr_t)t.setvol_fn, setvol_start, sizeof setvol_start))
        return fail("SetVolume code differs");
    put(t.vtable + 0x0C, (void*)&lock_fn);
    put(t.vtable + 0x10, (void*)&set_fn);
    put(t.vtable + 0x14, (void*)&unlock_fn);
    put(t.vtable + 0x9C, (void*)&setvol_fn);
    if (t.vtable2_setvol) put(t.vtable2_setvol, (void*)&setvol_fn);
    return true;
}

}  // namespace gp4u
