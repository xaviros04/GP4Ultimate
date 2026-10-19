#include "menu_aspect.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstring>

namespace gp4u {

const uint8_t kMenuSetter[22] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x4C, 0x24, 0x08, 0xA3, 0xB8, 0xD5,
                                 0x9E, 0x00, 0x89, 0x0D, 0xBC, 0xD5, 0x9E, 0x00, 0xC2, 0x08, 0x00};
const uint8_t kMenuInitStore[6] = {0x89, 0x35, 0xB8, 0xD5, 0x9E, 0x00};
const uint8_t kMenuResetStore[6] = {0x89, 0x0D, 0xB8, 0xD5, 0x9E, 0x00};

MenuSizeTargets gp4_menu_size_targets() { return {0x5B73B0, 0x5B47E5, 0x5CCC61, 0x9ED5B8, 0x9ED5BC}; }

namespace {
MenuSizeTargets g_t{};
double g_max = 16.0 / 9.0;
}  // namespace

}  // namespace gp4u

extern "C" void gp4u_menu_store_w(int w, int h) __asm__("gp4u_menu_store_w");
extern "C" __attribute__((used)) void gp4u_menu_store_w(int w, int h) {
    *(volatile int*)(uintptr_t)gp4u::g_t.width = gp4u::menu_width(w, h, gp4u::g_max);
}

// GP4's SetScreenSize(w, h), stdcall like the original (ret 8).
extern "C" void __stdcall gp4u_menu_set_size(int w, int h) {
    *(volatile int*)(uintptr_t)gp4u::g_t.width = gp4u::menu_width(w, h, gp4u::g_max);
    *(volatile int*)(uintptr_t)gp4u::g_t.height = h;
}

extern "C" void gp4u_menu_init_stub() __asm__("gp4u_menu_init_stub");
extern "C" void gp4u_menu_reset_stub() __asm__("gp4u_menu_reset_stub");
// Replace "mov [width], esi" (h in edi) and "mov [width], ecx" (h in eax); every register
// and flag kept.
__asm__(
    ".text\n"
    ".globl gp4u_menu_init_stub\n"
    "gp4u_menu_init_stub:\n"
    "    pushfl\n"
    "    pushal\n"
    "    cld\n"
    "    pushl %edi\n"
    "    pushl %esi\n"
    "    call gp4u_menu_store_w\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n"
    ".globl gp4u_menu_reset_stub\n"
    "gp4u_menu_reset_stub:\n"
    "    pushfl\n"
    "    pushal\n"
    "    cld\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    call gp4u_menu_store_w\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n");

namespace gp4u {

void menu_aspect_configure(const MenuSizeTargets& t, double max_aspect) {
    g_t = t;
    g_max = max_aspect;
}
void* menu_setter_fn() { return (void*)&gp4u_menu_set_size; }
void* menu_init_stub() { return (void*)&gp4u_menu_init_stub; }
void* menu_reset_stub() { return (void*)&gp4u_menu_reset_stub; }

namespace {
bool same(uint32_t addr, const uint8_t* want, size_t n) {
    uint8_t cur[32];
    SIZE_T got = 0;
    return n <= sizeof cur && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, cur, n, &got) && got == n &&
           std::memcmp(cur, want, n) == 0;
}
void write(uint32_t addr, const uint8_t* b, size_t n) {
    DWORD old;
    VirtualProtect((LPVOID)(uintptr_t)addr, n, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy((void*)(uintptr_t)addr, b, n);
    VirtualProtect((LPVOID)(uintptr_t)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, n);
}
void branch(uint32_t at, uint8_t op, void* to, size_t len) {
    uint8_t b[8];
    std::memset(b, 0x90, sizeof b);
    b[0] = op;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    std::memcpy(b + 1, &rel, 4);
    write(at, b, len);
}
}  // namespace

bool install_menu_aspect(const MenuSizeTargets& t, double max_aspect, std::string* why) {
    if (!same(t.setter, kMenuSetter, sizeof kMenuSetter) || !same(t.init_store, kMenuInitStore, sizeof kMenuInitStore) ||
        !same(t.reset_store, kMenuResetStore, sizeof kMenuResetStore)) {
        if (why) *why = "menu code differs (other GP4 build or already patched)";
        return false;
    }
    menu_aspect_configure(t, max_aspect);
    branch(t.setter, 0xE9, (void*)&gp4u_menu_set_size, 5);
    branch(t.init_store, 0xE8, (void*)&gp4u_menu_init_stub, 6);
    branch(t.reset_store, 0xE8, (void*)&gp4u_menu_reset_stub, 6);
    // In case GP4 set its size already.
    int w = *(volatile int*)(uintptr_t)t.width, h = *(volatile int*)(uintptr_t)t.height;
    if (w > 0 && h > 0) *(volatile int*)(uintptr_t)t.width = menu_width(w, h, max_aspect);
    return true;
}

}  // namespace gp4u
