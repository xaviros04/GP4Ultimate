#include "net_sync.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "core/net_predict.h"
#include "log.h"

namespace gp4u {

namespace {
NetSyncTargets g_t{};
bool g_installed = false;
NetPacer g_pacer;
NetSyncStats g_total, g_sec;
double g_sec_start = -1;
int g_chain = 0;          // extra steps run in a row so far
bool g_predict = false;
RemotePredictor g_predictor;
double g_drop_ms = 10000;
double g_last_step = -1;     // when the previous step ran (a long pause: a new race)
double g_heard[32];         // when each player's last race packet arrived (-1 never)
int g_master_idx = -1;      // player index of the race director, as last heard
uint32_t g_dropped = 0;     // players already taken out (bit per index)
bool g_last_catchup = false;

double qpc_ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
// Sleep(1) can last a whole 15.6 ms timer tick; yield until the time has passed instead
// (GP4's own wait polled without sleeping at all).
void os_sleep(int ms) {
    double end = qpc_ms() + ms;
    while (qpc_ms() < end) SwitchToThread();
}
double (*g_now)() = qpc_ms;
void (*g_on_step)() = nullptr;
void (*g_sleep)(int) = os_sleep;

template <typename T>
T& at(uint32_t a) { return *(T*)(uintptr_t)a; }

void add_lead(NetSyncStats& s, double l) {
    if (!s.lead_known || l < s.lead_min) s.lead_min = l;
    if (!s.lead_known || l > s.lead_max) s.lead_max = l;
    s.lead_known = true;
    s.lead_sum += l;
    ++s.lead_n;
}
}  // namespace

NetSyncTargets gp4_net_sync_targets() {
    return {0x43637F, 0x445D60, 0x445A59, 0x4459B7, 0x445B89, 0x445E43, 0x7A8DF8, 0x7AB024, 0x62F8B0, 0x62F8B4,
            0x62F8F4, 0x62F8E0, 0x62F8C0, 0x7AB022, 0x62F8EA, 0x62F8EB, 0x62F8ED, 0x62F92C, 0x62F8F9, 0x66D814,
            0x66FEF0, 0x435B61, 0x62F8C8, 0x62F8DC};
}

bool net_sync_installed() { return g_installed; }
NetSyncStats net_sync_stats() {
    NetSyncStats s = g_total;   // logged seconds + the one in progress
    s.steps += g_sec.steps;
    s.sent += g_sec.sent;
    s.received += g_sec.received;
    s.waits += g_sec.waits;
    s.catchups += g_sec.catchups;
    s.predicted += g_sec.predicted;
    s.dropped += g_sec.dropped;
    s.wait_ms += g_sec.wait_ms;
    return s;
}
void net_sync_set_step_callback(void (*on_step)()) { g_on_step = on_step; }
void net_sync_set_drop_after(double ms) { g_drop_ms = ms; }
void net_sync_set_prediction(bool on, int max_steps) {
    g_predict = on;
    g_predictor = RemotePredictor(22, 0x2AC, max_steps);
}
void net_sync_set_clock(double (*now_ms)(), void (*sleep_ms)(int)) {
    g_now = now_ms ? now_ms : qpc_ms;
    g_sleep = sleep_ms ? sleep_ms : os_sleep;
}

}  // namespace gp4u

// Calls a GP4 routine that takes eax/esi and reports in CF; every register preserved.
extern "C" uint32_t __cdecl gp4u_ncall(uint32_t fn, uint32_t eax, uint32_t esi);
#if defined(_MSC_VER)
extern "C" __declspec(naked) uint32_t __cdecl gp4u_ncall(uint32_t, uint32_t, uint32_t) {
    __asm {
        push ebp
        mov ebp, esp
        pushad
        mov ecx, [ebp + 8]
        mov eax, [ebp + 12]
        mov esi, [ebp + 16]
        cld
        call ecx
        setc al
        movzx eax, al
        mov [esp + 28], eax
        popad
        pop ebp
        ret
    }
}
#else
asm(".text\n"
    ".globl _gp4u_ncall\n"
    "_gp4u_ncall:\n"
    "    pushl %ebp\n"
    "    movl %esp, %ebp\n"
    "    pushal\n"
    "    movl 8(%ebp), %ecx\n"
    "    movl 12(%ebp), %eax\n"
    "    movl 16(%ebp), %esi\n"
    "    cld\n"
    "    call *%ecx\n"
    "    setc %al\n"
    "    movzbl %al, %eax\n"
    "    movl %eax, 28(%esp)\n"
    "    popal\n"
    "    popl %ebp\n"
    "    ret\n");
#endif

namespace gp4u {
namespace {

// Everything that has arrived: GP4 processes each packet (remote cars, master's random
// seed and events); the mod notes who was heard from and the master's race time.
void drain(double now) {
    const NetSyncTargets& t = g_t;
    for (int guard = 0; guard < 256; ++guard) {
        bool race = gp4u_ncall(t.recv_fn, 0, 0) != 0;
        if (!race) {
            if (at<uint8_t>(t.recv_state) == 0) break;   // queue empty
            continue;                                   // menu/system message: GP4 handled it
        }
        gp4u_ncall(t.recv_post, 0, 0);
        if (at<uint8_t>(t.pkt_type) != 1) continue;
        ++g_sec.received;
        uint8_t from = at<uint8_t>(t.pkt_from) & 31;
        at<uint32_t>(t.got_mask) |= 1u << from;
        g_heard[from] = now;
        if (at<uint8_t>(t.pkt_hdr) & 0x80) g_master_idx = from;
        if ((at<uint8_t>(t.pkt_hdr) & 0x80) && (at<uint32_t>(t.pkt_flags32) & 2))
            g_pacer.on_master((int32_t)at<uint32_t>(t.pkt_time), now);
    }
}

void send_mine() {
    const NetSyncTargets& t = g_t;
    uint32_t car = at<uint32_t>(t.player_car);
    if (at<uint8_t>(t.is_master) == 0xFF) {
        gp4u_ncall(t.next_ai_fn, 0, 0);
        uint32_t f = 0x23;   // events, seed, race time (as GP4 0x445d8f)
        if (car) f |= 4;
        if (at<uint32_t>(t.ai_car) != 0xFFFFFFFFu) f |= 8;
        at<uint32_t>(t.pkt_flags32) = f;
        gp4u_ncall(t.send_fn, 0, car);
        at<uint32_t>(t.events) = 0;
        ++g_sec.sent;
    } else {
        at<uint32_t>(t.events) = 0;
        if (car) {
            at<uint32_t>(t.pkt_flags32) = 2 | 4;   // race time + own car (as GP4 0x445df9)
            gp4u_ncall(t.send_fn, 0, car);
            ++g_sec.sent;
        }
    }
}

// Players whose GP4 stopped sending are taken out like players who left: the director
// marks them in its event mask, which every PC applies at the end of the step (0x446058,
// 0x445f75); a silent director is taken out by every PC itself.
void drop_silent_players(double now) {
    const NetSyncTargets& t = g_t;
    if (g_drop_ms <= 0 || !t.active_mask || !t.gone_mask) return;
    uint32_t active = at<uint32_t>(t.active_mask);
    uint8_t me = at<uint8_t>(t.my_index) & 31;
    bool master = at<uint8_t>(t.is_master) == 0xFF;
    g_dropped &= active;   // forget those already gone (or a new race)
    for (int i = 0; i < 32; ++i) {
        uint32_t bit = 1u << i;
        if (i == me || !(active & bit) || (g_dropped & bit) || g_heard[i] < 0) continue;
        double silent = now - g_heard[i];
        if (silent < g_drop_ms) continue;
        if (master) {
            at<uint32_t>(t.events) |= bit;   // sent with the next packet, applied everywhere
        } else if (i == g_master_idx) {
            at<uint32_t>(t.gone_mask) |= bit;   // nobody else can announce it
        } else {
            continue;   // the director announces it
        }
        g_dropped |= bit;
        ++g_sec.dropped;
        logf("net: player %d silent for %.1f s (its game crashed?): taking out its car%s", i, silent / 1000.0,
             i == g_master_idx ? "; it directed the race, GP4 picks another director" : "");
    }
}

// Remote cars whose record no packet replaced this step get GP4's own update (with the
// inputs of their last packet), as GP4 does for the AI cars (core/net_predict.h).
void predict_remote_cars() {
    const NetSyncTargets& t = g_t;
    if (!g_predict || !t.car_base || !t.car_update) return;
    for (uint32_t i = 0; i < 22; ++i) {
        uint32_t car = t.car_base + i * 0x41C;
        const uint8_t* rec = (const uint8_t*)(uintptr_t)car;
        if (g_predictor.due(i, rec, rec[0x414], rec[0xE5])) {
            gp4u_ncall(t.car_update, 0, car);
            ++g_sec.predicted;
        }
        g_predictor.seen(i, rec);
    }
}

void log_second(double now) {
    if (g_sec_start < 0) g_sec_start = now;
    if (now - g_sec_start < 1000) return;
    const NetSyncStats& s = g_sec;
    char lead[96] = "lead -";
    if (s.lead_n)
        std::snprintf(lead, sizeof lead, "lead avg %.0f [%.0f..%.0f] ms", s.lead_sum / s.lead_n, s.lead_min, s.lead_max);
    char pred[32] = "";
    if (g_predict) std::snprintf(pred, sizeof pred, " predicted %u", s.predicted);
    logf("netsync %.1fs: %s | steps %u sent %u recv %u | %s | waits %u (%.0f ms) catch-up %u%s",
         (now - g_sec_start) / 1000.0, at<uint8_t>(g_t.is_master) == 0xFF ? "master" : "client", s.steps, s.sent,
         s.received, lead, s.waits, s.wait_ms, s.catchups, pred);
    g_total.steps += s.steps;
    g_total.sent += s.sent;
    g_total.received += s.received;
    g_total.waits += s.waits;
    g_total.catchups += s.catchups;
    g_total.predicted += s.predicted;
    g_total.dropped += s.dropped;
    g_total.wait_ms += s.wait_ms;
    g_sec = NetSyncStats{};
    g_sec_start = now;
}

}  // namespace
}  // namespace gp4u

// Replaces GP4's per-step exchange (called from the step, registers are free).
extern "C" void __cdecl gp4u_net_step() {
    using namespace gp4u;
    const NetSyncTargets& t = g_t;
    double now = g_now();
    if (g_on_step) g_on_step();
    if (g_last_step < 0 || now - g_last_step > 2000) {   // a new race (or session): nobody heard yet
        for (double& h : g_heard) h = -1;
        g_master_idx = -1;
        g_dropped = 0;
    }
    g_last_step = now;
    ++g_sec.steps;
    at<uint32_t>(t.got_mask) = 1u << (at<uint8_t>(t.my_index) & 31);
    drain(now);
    send_mine();
    drain(now);
    NetPacer::Action a = NetPacer::Action::Run;
    if (at<uint8_t>(t.is_master) != 0xFF) {
        int chain = g_last_catchup ? g_chain : 0;
        int32_t mine = (int32_t)at<uint32_t>(t.race_time);
        a = g_pacer.decide(mine, now, chain);
        if (a == NetPacer::Action::Wait) {
            ++g_sec.waits;
            double start = now;
            while (a == NetPacer::Action::Wait && now - start < g_pacer.config().max_wait_ms) {
                g_sleep(1);
                now = g_now();
                drain(now);
                a = g_pacer.decide(mine, now, chain);
            }
            g_sec.wait_ms += now - start;
            if (a == NetPacer::Action::Wait) a = NetPacer::Action::Run;
        }
        double l;
        if (g_pacer.lead(mine, now, l)) {
            add_lead(g_sec, l);
            add_lead(g_total, l);
        }
        g_chain = a == NetPacer::Action::CatchUp ? chain + 1 : 0;
    }
    g_last_catchup = a == NetPacer::Action::CatchUp;
    if (g_last_catchup) ++g_sec.catchups;
    at<uint8_t>(t.catchup) = g_last_catchup ? 0xFF : 0;
    predict_remote_cars();
    drop_silent_players(now);
    log_second(now);
}

// GP4's x87 and SSE state is saved around the C++ code (fxsave) and the x87 stack is
// emptied for it (fninit, control word 0x037F like GP4's own).
#if defined(_MSC_VER)
extern "C" __declspec(naked) void gp4u_net_step_stub() {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 512
        and esp, -16
        fxsave [esp]
        fninit
        pushad
        cld
        call gp4u_net_step
        popad
        fxrstor [esp]
        mov esp, ebp
        pop ebp
        ret
    }
}
#else
extern "C" void gp4u_net_step_stub();
asm(".text\n"
    ".globl _gp4u_net_step_stub\n"
    "_gp4u_net_step_stub:\n"
    "    pushl %ebp\n"
    "    movl %esp, %ebp\n"
    "    subl $512, %esp\n"
    "    andl $-16, %esp\n"
    "    fxsave (%esp)\n"
    "    fninit\n"
    "    pushal\n"
    "    cld\n"
    "    call _gp4u_net_step\n"
    "    popal\n"
    "    fxrstor (%esp)\n"
    "    movl %ebp, %esp\n"
    "    popl %ebp\n"
    "    ret\n");
#endif

namespace gp4u {

bool install_net_sync(const NetSyncTargets& t, const NetPacerConfig& cfg, std::string* why) {
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
    g_pacer = NetPacer(cfg);
    g_total = g_sec = NetSyncStats{};
    g_sec_start = -1;
    g_chain = 0;
    g_last_catchup = false;
    g_predictor.reset();
    for (double& h : g_heard) h = -1;
    g_master_idx = -1;
    g_dropped = 0;
    g_last_step = -1;
    int32_t nrel = (int32_t)((uintptr_t)&gp4u_net_step_stub - (t.call_site + 5));
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

}  // namespace gp4u
