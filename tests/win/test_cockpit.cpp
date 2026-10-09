// End-to-end test of the cockpit wheel patch (src/win/cockpit_patch.cpp):
//  1. patches a buffer holding the signature, calls it exactly like GP4 does (car in esi,
//     result in eax) and checks the result and the preserved registers;
//  2. refuses to patch other bytes or an already patched function;
//  3. with a GP4.exe path as argv[1]: copies the ORIGINAL 0x409a47 out of the user's exe at
//     run time (nothing from it is stored in the repository), maps its globals at their real
//     addresses relocated, and checks core/cockpit.h's re-implementation against it bit for bit, then
//     patches that copy and checks the raw-input path.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

#include "core/cockpit.h"
#include "core/gp4_resolution.h"
#include "win/cockpit_patch.h"
#include "win/physics_hook.h"
#include "win/net_sync.h"

using namespace gp4u;

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECKV(c, v) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s  (value %ld)\n", __LINE__, #c, (long)(v)); } } while (0)

struct Regs { uint32_t eax, ecx, edx, esi, edi; };

// Calls fn like GP4's callers: esi = car; ecx/edx hold sentinels that must survive.
static Regs call_gp4(uint32_t fn, uint8_t* car) {
    Regs r{};
    uint32_t esi = (uint32_t)(uintptr_t)car, edi = fn;
    asm volatile("movl $0x11111111, %%ecx\n\t"
                 "movl $0x22222222, %%edx\n\t"
                 "call *%%edi\n\t"
                 : "=a"(r.eax), "=c"(r.ecx), "=d"(r.edx), "+S"(esi), "+D"(edi)
                 :
                 : "memory", "cc", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)",
                   "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
    r.esi = esi;
    r.edi = edi;
    return r;
}

static void put_car(uint8_t* car, int16_t front, uint8_t flags, int16_t raw) {
    std::memcpy(car + cockpit::kCarFront, &front, 2);
    car[cockpit::kCarFlags] = flags;
    std::memcpy(car + cockpit::kCarRaw, &raw, 2);
}

static uint8_t* exec_buf(size_t n) {
    return (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void test_patch_on_signature() {
    static int32_t mul_a = 24576, limit = 3641, mul_b = 18432;
    uint8_t* fn = exec_buf(4096);
    std::memset(fn, 0xC3, 4096);
    std::memcpy(fn, cockpit::kSignature, sizeof cockpit::kSignature);
    CockpitTargets t{(uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)&mul_a, (uint32_t)(uintptr_t)&limit,
                     (uint32_t)(uintptr_t)&mul_b};
    std::string why;
    CHECK(!install_cockpit_wheel(0, t, &why) && why == "disabled");
    CHECK(install_cockpit_wheel(360, t, &why));
    CHECK(fn[0] == 0xE9);
    CHECK(!install_cockpit_wheel(360, t, &why));   // already patched: refused

    std::vector<uint8_t> car(cockpit::kCarFront + 16, 0);
    cockpit::Consts k{mul_a, limit, mul_b};
    // Raw input drives it: full right = +180 degrees on a 360 degree setting, half = 90.
    put_car(car.data(), 500, 0, 4064);
    Regs r = call_gp4(t.func, car.data());
    CHECKV(r.eax == 32768u, r.eax);
    CHECK(r.ecx == 0x11111111 && r.edx == 0x22222222);
    CHECK(r.esi == (uint32_t)(uintptr_t)car.data() && r.edi == t.func);
    put_car(car.data(), 500, 0, -2032);
    r = call_gp4(t.func, car.data());
    CHECKV((int32_t)r.eax == -16384, r.eax);
    CHECK(std::abs(cockpit::visual_degrees((int32_t)r.eax) + 90.0) < 1e-9);
    // No raw input (AI car, keyboard): the original formula.
    for (int16_t front : {0, 100, -100, 2000, -2000, 3000, -3000, 30000, -30000}) {
        for (uint8_t fl : {0, 1}) {
            put_car(car.data(), front, fl, 0);
            r = call_gp4(t.func, car.data());
            CHECKV((int32_t)r.eax == cockpit::original_angle(car.data(), k), front);
        }
    }

    // Other bytes: refused, left alone.
    uint8_t* other = exec_buf(4096);
    std::memset(other, 0x90, 64);
    CockpitTargets t2 = t;
    t2.func = (uint32_t)(uintptr_t)other;
    CHECK(!install_cockpit_wheel(360, t2, &why) && other[0] == 0x90);
}

// --- differential test against the user's GP4.exe -----------------------------------
static bool read_exe_bytes(const char* path, uint32_t va, size_t n, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
    if (img.size() < 0x400) return false;
    uint32_t pe;
    std::memcpy(&pe, &img[0x3C], 4);
    uint16_t nsec, optsz;
    std::memcpy(&nsec, &img[pe + 6], 2);
    std::memcpy(&optsz, &img[pe + 20], 2);
    uint32_t base;
    std::memcpy(&base, &img[pe + 24 + 28], 4);
    uint32_t rva = va - base;
    for (int i = 0; i < nsec; ++i) {
        const uint8_t* s = &img[pe + 24 + optsz + 40 * i];
        uint32_t vsz, vaddr, rawsz, rawptr;
        std::memcpy(&vsz, s + 8, 4);
        std::memcpy(&vaddr, s + 12, 4);
        std::memcpy(&rawsz, s + 16, 4);
        std::memcpy(&rawptr, s + 20, 4);
        if (rva >= vaddr && rva + n <= vaddr + rawsz) {
            out.assign(img.begin() + rawptr + (rva - vaddr), img.begin() + rawptr + (rva - vaddr) + n);
            return true;
        }
    }
    return false;
}

static void test_against_original(const char* exe) {
    std::vector<uint8_t> code, a, b, seven;
    if (!read_exe_bytes(exe, cockpit::kFunc, 0x6A, code) || !read_exe_bytes(exe, cockpit::kMulA, 4, a) ||
        !read_exe_bytes(exe, cockpit::kMulB, 4, b) || !read_exe_bytes(exe, 0x62ee80, 8, seven)) {
        std::printf("(could not read %s: differential test skipped)\n", exe);
        return;
    }
    CHECK(std::memcmp(code.data(), cockpit::kSignature, sizeof cockpit::kSignature) == 0);
    CHECK(code[0x69] == 0xC3);
    int32_t mul_a, mul_b;
    double c7;
    std::memcpy(&mul_a, a.data(), 4);
    std::memcpy(&mul_b, b.data(), 4);
    std::memcpy(&c7, seven.data(), 8);
    CHECKV(mul_a == 24576, mul_a);
    CHECKV(mul_b == 18432, mul_b);
    CHECK(c7 == 7.0);
    // The original addresses its globals absolutely; relocate the copy onto our own block
    // (each absolute operand is rewritten; the expected count proves nothing else matched).
    static uint8_t data[64];
    uint8_t* const g_mul_a = data, *const g_limit = data + 8, *const g_mul_b = data + 16, *const g_seven = data + 24,
                   *const g_scratch = data + 32;
    struct Reloc { uint32_t from; uint8_t* to; int expect; } relocs[] = {
        {cockpit::kMulA, g_mul_a, 1}, {cockpit::kLimit, g_limit, 4}, {cockpit::kMulB, g_mul_b, 1},
        {0x62ee80, g_seven, 1}, {0x6f2bf4, g_scratch, 4}};
    for (auto& r : relocs) {
        int n = 0;
        for (size_t i = 0; i + 4 <= code.size(); ++i) {
            uint32_t v;
            std::memcpy(&v, &code[i], 4);
            if (v != r.from) continue;
            uint32_t to = (uint32_t)(uintptr_t)r.to;
            std::memcpy(&code[i], &to, 4);
            ++n;
        }
        CHECKV(n == r.expect, (long)r.from);
    }
    std::memcpy(g_mul_a, &mul_a, 4);
    std::memcpy(g_mul_b, &mul_b, 4);
    std::memcpy(g_seven, &c7, 8);
    uint8_t* fn = exec_buf(4096);
    std::memcpy(fn, code.data(), code.size());
    std::vector<uint8_t> car(cockpit::kCarFront + 16, 0);
    std::mt19937 rng(42);
    int mismatches = 0, n = 0;
    for (int32_t limit : {3641, 1000, 6000}) {
        std::memcpy(g_limit, &limit, 4);
        cockpit::Consts k{mul_a, limit, mul_b};
        for (int i = 0; i < 20000; ++i) {
            int16_t front = (int16_t)(i < 65536 / 4 ? (int)(rng() & 0xFFFF) : (int)(rng() % 9000) - 4500);
            uint8_t fl = (uint8_t)(rng() & 0xFF);
            put_car(car.data(), front, fl, (int16_t)(rng() & 0xFFFF));   // raw is ignored by the original
            Regs r = call_gp4((uint32_t)(uintptr_t)fn, car.data());
            ++n;
            if ((int32_t)r.eax != cockpit::original_angle(car.data(), k)) ++mismatches;
            if (r.ecx != 0x11111111 || r.edx != 0x22222222) ++mismatches;
        }
    }
    std::printf("differential vs GP4.exe 0x409a47: %d inputs, %d mismatches\n", n, mismatches);
    // Physics-step hook site: call 0x418942 at 0x41874A in the real exe.
    {
        std::vector<uint8_t> site;
        CHECK(read_exe_bytes(exe, 0x41874A, 5, site) && site[0] == 0xE8);
        int32_t rel;
        std::memcpy(&rel, site.data() + 1, 4);
        CHECK(0x41874A + 5 + rel == 0x418942);
    }
    // Network step site: call 0x445d60 at 0x43637f, and the routines/fields net_sync uses.
    {
        std::vector<uint8_t> site;
        CHECK(read_exe_bytes(exe, 0x43637F, 5, site) && site[0] == 0xE8);
        int32_t rel;
        std::memcpy(&rel, site.data() + 1, 4);
        CHECK(0x43637F + 5 + rel == 0x445D60);
        // 0x445d60 starts with "cmp byte [0x62f8f9], 0xff" (the catch-up flag) and the
        // master's flags constant is mov eax, 0x23 at 0x445d8f.
        std::vector<uint8_t> b;
        CHECK(read_exe_bytes(exe, 0x445D60, 7, b) && b[0] == 0x80 && b[1] == 0x3D && b[6] == 0xFF);
        uint32_t a;
        std::memcpy(&a, b.data() + 2, 4);
        CHECK(a == 0x62F8F9);
        CHECK(read_exe_bytes(exe, 0x445D8F, 5, b) && b[0] == 0xB8 && b[1] == 0x23);
        // 0x445a59 begins pushal; call 0x51ef67 (receive); 0x445b89 is mov eax,1; call 0x44587c (send).
        CHECK(read_exe_bytes(exe, 0x445A59, 1, b) && b[0] == 0x60);
        CHECK(read_exe_bytes(exe, 0x445B89, 5, b) && b[0] == 0xB8 && b[1] == 1);
    }
    // [display] resolution patch sites (core/gp4_resolution.h) hold exactly the expected
    // instructions in the real exe.
    for (auto& s : gp4res::sites()) {
        std::vector<uint8_t> real;
        CHECK(read_exe_bytes(exe, s.addr, s.op_len + 4, real) && real == gp4res::bytes(s, s.orig));
    }
    CHECKV(mismatches == 0, mismatches);
    // Patch the real copy and check both paths again.
    int32_t limit = 3641;
    std::memcpy(g_limit, &limit, 4);
    // The patch reads the constants from the targets: point them at the relocated block.
    CockpitTargets t{(uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)g_mul_a, (uint32_t)(uintptr_t)g_limit,
                     (uint32_t)(uintptr_t)g_mul_b};
    // The relocation rewrote the imul operand inside the signature; put the original bytes
    // back (after the patch that code is no longer executed).
    std::memcpy(fn, cockpit::kSignature, sizeof cockpit::kSignature);
    std::string why;
    CHECK(install_cockpit_wheel(540, t, &why));
    put_car(car.data(), 1234, 0, 4064);
    Regs r = call_gp4(t.func, car.data());
    CHECK(std::abs(cockpit::visual_degrees((int32_t)r.eax) - (-90.0)) < 0.01 ||   // 270 deg == -90 after & 0xFFFF
          std::abs(cockpit::visual_degrees((int32_t)r.eax) - 270.0) < 0.01);
    put_car(car.data(), 1234, 0, 0);
    r = call_gp4(t.func, car.data());
    CHECKV((int32_t)r.eax == cockpit::original_angle(car.data(), {mul_a, limit, mul_b}), r.eax);
}

// Physics-step hook: the patched call still runs GP4's function, preserves the registers,
// and snapshots the wheel forces only for the player's car.
static volatile LONG g_called = 0;
static void test_physics_hook() {
    uint8_t* code = exec_buf(4096);
    std::memset(code, 0xCC, 4096);
    // site: call step_fn ; ret     step_fn (at +0x100): inc dword [g_called] ; ret
    uint32_t site = (uint32_t)(uintptr_t)code, fn = site + 0x100;
    int32_t rel = (int32_t)(fn - (site + 5));
    code[0] = 0xE8;
    std::memcpy(code + 1, &rel, 4);
    code[5] = 0xC3;
    uint8_t* f = code + 0x100;
    f[0] = 0xFF; f[1] = 0x05;
    uint32_t a = (uint32_t)(uintptr_t)&g_called;
    std::memcpy(f + 2, &a, 4);
    f[6] = 0xC3;
    static uint8_t wheels[4 * 0x300];
    static int32_t glat = 111, glong = -222;
    static uint32_t player_ptr = 0;
    std::vector<uint8_t> player(0x41C, 0), other(0x41C, 0);
    for (int w = 0; w < 4; ++w) {
        int32_t fx = 1000 * (w + 1), fy = -500 * (w + 1), mz = 7 * (w + 1);
        std::memcpy(wheels + w * 0x300 + 0x1A0, &fx, 4);
        std::memcpy(wheels + w * 0x300 + 0xE0, &fy, 4);
        std::memcpy(wheels + w * 0x300 + 0x274, &mz, 4);
    }
    player_ptr = (uint32_t)(uintptr_t)player.data();
    PhysTargets t{site, fn, (uint32_t)(uintptr_t)&player_ptr, (uint32_t)(uintptr_t)wheels, (uint32_t)(uintptr_t)&glat,
                  (uint32_t)(uintptr_t)&glong};
    std::string why;
    PhysSnap snap;
    CHECK(!physics_snapshot(snap));
    PhysTargets bad = t;
    bad.step_fn = fn + 1;
    CHECK(!install_physics_hook(bad, &why));   // call target does not match: refused
    CHECK(install_physics_hook(t, &why));
    CHECK(code[0] == 0xE8 && std::memcmp(code + 1, &rel, 4) != 0);
    // Another car: GP4's function runs, nothing captured.
    Regs r = call_gp4(site, other.data());
    CHECK(g_called == 1 && !physics_snapshot(snap));
    CHECK(r.ecx == 0x11111111 && r.edx == 0x22222222 && r.esi == (uint32_t)(uintptr_t)other.data());
    // The player's car: captured.
    r = call_gp4(site, player.data());
    CHECK(g_called == 2);
    CHECK(r.ecx == 0x11111111 && r.edx == 0x22222222 && r.esi == (uint32_t)(uintptr_t)player.data());
    CHECK(physics_snapshot(snap) && snap.steps == 1);
    CHECK(snap.fx[2] == 3000 && snap.fx[3] == 4000 && snap.fy[0] == -500 && snap.mz[3] == 28);
    CHECK(snap.g_lat == 111 && snap.g_long == -222);
}

// Free-running race network (src/win/net_sync.cpp): the redirected per-step call drives
// fake GP4 routines with GP4's register conventions (CF results, car in esi).
namespace fakenet {
struct Pkt { bool race; uint8_t from, type, hdr; uint32_t flags, time; };
std::vector<Pkt> queue;
uint8_t is_master, recv_state, my_index, pkt_from, pkt_type, pkt_hdr, catchup;
uint32_t player_car, ai_car, flags32, events, got_mask, pkt_time, race_time;
int recv_post_calls, next_ai_calls, original_calls;
struct Sent { uint32_t esi, flags; };
std::vector<Sent> sent;
double clock_ms;
double now() { return clock_ms; }
void sleep(int ms) { clock_ms += ms; }
}  // namespace fakenet

extern "C" uint32_t __cdecl fake_recv_c() {
    using namespace fakenet;
    if (queue.empty()) { recv_state = 0; return 0; }
    Pkt p = queue.front();
    queue.erase(queue.begin());
    recv_state = 1;
    if (!p.race) return 0;   // a menu message GP4 handles itself
    pkt_from = p.from; pkt_type = p.type; pkt_hdr = p.hdr; flags32 = p.flags; pkt_time = p.time;
    return 1;
}
extern "C" void __cdecl fake_send_c(uint32_t esi) { fakenet::sent.push_back({esi, fakenet::flags32}); }
extern "C" void __cdecl fake_post_c() { ++fakenet::recv_post_calls; }
extern "C" void __cdecl fake_next_ai_c() { ++fakenet::next_ai_calls; fakenet::ai_car = 0x1234; }
extern "C" void __cdecl fake_original_c() { ++fakenet::original_calls; }
extern "C" void fake_recv(), fake_send(), fake_post(), fake_next_ai(), fake_original();
asm(".text\n"
    "_fake_recv:  pushal\n call _fake_recv_c\n negl %eax\n popal\n ret\n"     // CF = race packet
    "_fake_send:  pushal\n pushl %esi\n call _fake_send_c\n addl $4, %esp\n popal\n ret\n"
    "_fake_post:  pushal\n call _fake_post_c\n popal\n ret\n"
    "_fake_next_ai: pushal\n call _fake_next_ai_c\n popal\n ret\n"
    "_fake_original: pushal\n call _fake_original_c\n popal\n ret\n");

// Calls fn with a value on the x87 stack and in xmm7; true if both survive.
static bool fpu_survives(uint32_t fn) {
    double in = 1234.5, st = 0, x = 0;
    asm volatile("fldl %2\n\t"
                 "movsd %2, %%xmm7\n\t"
                 "call *%3\n\t"
                 "fstpl %0\n\t"
                 "movsd %%xmm7, %1\n\t"
                 : "=m"(st), "=m"(x)
                 : "m"(in), "b"(fn)
                 : "eax", "ecx", "edx", "esi", "edi", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
                   "xmm5", "xmm6", "xmm7");
    return st == in && x == in;
}

static void test_net_sync() {
    using namespace fakenet;
    uint8_t* code = exec_buf(4096);
    std::memset(code, 0xCC, 4096);
    // site: call original ; ret
    uint32_t site = (uint32_t)(uintptr_t)code, orig = (uint32_t)(uintptr_t)&fake_original;
    int32_t rel = (int32_t)(orig - (site + 5));
    code[0] = 0xE8;
    std::memcpy(code + 1, &rel, 4);
    code[5] = 0xC3;
    auto A = [](const void* p) { return (uint32_t)(uintptr_t)p; };
    NetSyncTargets t{site, orig, A((void*)&fake_recv), A((void*)&fake_post), A((void*)&fake_send),
                     A((void*)&fake_next_ai), A(&is_master), A(&recv_state), A(&player_car), A(&ai_car),
                     A(&flags32), A(&events), A(&got_mask), A(&my_index), A(&pkt_from), A(&pkt_type),
                     A(&pkt_hdr), A(&pkt_time), A(&catchup), A(&race_time)};
    net_sync_set_clock(&fakenet::now, &fakenet::sleep);
    NetPacerConfig pc;   // window 50 ms, wait <= 50 ms, catch-up <= 3
    pc.window_ms = 50;
    std::string why;
    NetSyncTargets bad = t;
    bad.step_fn = orig + 1;
    CHECK(!install_net_sync(bad, pc, &why));
    // Before the patch the original exchange runs.
    std::vector<uint8_t> dummy(16);
    call_gp4(site, dummy.data());
    CHECK(original_calls == 1);
    CHECK(install_net_sync(t, pc, &why));
    CHECK(code[0] == 0xE8 && std::memcmp(code + 1, &rel, 4) != 0);

    // Race master: sends its car + an AI car + time/seed/events without waiting, and
    // processes the client packets that are there (a menu message in between).
    is_master = 0xFF; my_index = 0; player_car = 0x5000; ai_car = 0xFFFFFFFF; events = 0x10;
    queue = {{true, 1, 1, 0, 6, 9990}, {false}, {true, 2, 1, 0, 6, 9995}};
    clock_ms = 0;
    Regs r = call_gp4(site, dummy.data());
    CHECK(original_calls == 1);   // GP4's waiting exchange no longer runs
    CHECK(r.ecx == 0x11111111 && r.edx == 0x22222222 && r.esi == A(dummy.data()));
    CHECK(sent.size() == 1 && sent[0].esi == 0x5000 && sent[0].flags == 0x2F);
    CHECK(next_ai_calls == 1 && events == 0);
    CHECK(queue.empty() && recv_post_calls == 2 && got_mask == 0x7);
    CHECK(catchup == 0);
    // Nothing waiting: returns at once.
    double before = clock_ms;
    call_gp4(site, dummy.data());
    CHECK(clock_ms == before && sent.size() == 2);

    // Client: sends its car + time, learns the master's clock, follows it.
    is_master = 0; my_index = 1; sent.clear();
    clock_ms = 1000;
    queue = {{true, 0, 1, 0x80, 0x2F, 50000}};
    race_time = 50000;
    call_gp4(site, dummy.data());
    CHECK(sent.size() == 1 && sent[0].esi == 0x5000 && sent[0].flags == 6);
    CHECK(got_mask == 0x3 && catchup == 0);
    // In step 30 ms later: no wait.
    clock_ms = 1030; race_time = 50030;
    call_gp4(site, dummy.data());
    CHECK(clock_ms == 1030 && catchup == 0);
    // 120 ms ahead: waits (sleeping) at most max_wait_ms, then runs.
    race_time = 50150;
    call_gp4(site, dummy.data());
    CHECKV(clock_ms - 1030 >= 49 && clock_ms - 1030 <= 51, (long)(clock_ms - 1030));
    CHECK(catchup == 0 && net_sync_stats().waits == 1);
    // 70 ms ahead: waits only until inside the window (~20 ms).
    queue = {{true, 0, 1, 0x80, 0x2F, 51000}};
    clock_ms = 2000;
    call_gp4(site, dummy.data());   // learns master = 51000 at 2000
    race_time = 51070;
    double w0 = clock_ms;
    call_gp4(site, dummy.data());
    CHECKV(clock_ms - w0 >= 19 && clock_ms - w0 <= 22, (long)(clock_ms - w0));
    // 200 ms behind: catch-up steps, at most 3 in a row, then a drawn frame.
    race_time = 50800;
    int extra = 0;
    for (int i = 0; i < 5; ++i) {
        call_gp4(site, dummy.data());
        if (catchup == 0xFF) ++extra;
        else break;
    }
    CHECKV(extra == 3, extra);
    call_gp4(site, dummy.data());   // next frame may catch up again
    CHECK(catchup == 0xFF);
    // Master silent for longer than 500 ms: no pacing.
    clock_ms += 2000; race_time = 90000;
    double w1 = clock_ms;
    call_gp4(site, dummy.data());
    CHECK(clock_ms == w1 && catchup == 0);
    // GP4's FPU/SSE state survives the C++ step (it does double maths and sleeps).
    CHECK(fpu_survives(site));
    // No local car (spectating): nothing sent.
    player_car = 0; sent.clear();
    call_gp4(site, dummy.data());
    CHECK(sent.empty());
    net_sync_set_clock(nullptr, nullptr);
}

int main(int argc, char** argv) {
    test_patch_on_signature();
    test_physics_hook();
    test_net_sync();
    if (argc > 1) test_against_original(argv[1]);
    else std::printf("(no GP4.exe given: differential test skipped)\n");
    std::printf("[cockpit] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
