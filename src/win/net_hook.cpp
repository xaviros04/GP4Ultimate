#include "net_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <dplay.h>
#include <dplobby.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "core/net_playout.h"
#include "core/net_stats.h"
#include "dplay_online.h"
#include "log.h"

extern "C" {
extern const int gp4_dp_QueryInterface, gp4_dp_Close, gp4_dp_CreatePlayer, gp4_dp_DestroyPlayer, gp4_dp_EnumSessions,
    gp4_dp_GetMessageCount, gp4_dp_Open, gp4_dp_Receive, gp4_dp_Send, gp4_dp_SetSessionDesc, gp4_dp_EnumConnections,
    gp4_dp_InitializeConnection, gp4_dp_SendEx;
}

namespace gp4u {

namespace {

typedef HRESULT(WINAPI* CoCreateInstanceFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef HRESULT(STDMETHODCALLTYPE* QIFn)(void*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* CloseFn)(void*);
typedef HRESULT(STDMETHODCALLTYPE* CreatePlayerFn)(void*, LPDPID, LPDPNAME, HANDLE, LPVOID, DWORD, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* DestroyPlayerFn)(void*, DPID);
typedef HRESULT(STDMETHODCALLTYPE* EnumSessionsFn)(void*, LPDPSESSIONDESC2, DWORD, LPDPENUMSESSIONSCALLBACK2, LPVOID, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* GetMessageCountFn)(void*, DPID, LPDWORD);
typedef HRESULT(STDMETHODCALLTYPE* OpenFn)(void*, LPDPSESSIONDESC2, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* ReceiveFn)(void*, LPDPID, LPDPID, DWORD, LPVOID, LPDWORD);
typedef HRESULT(STDMETHODCALLTYPE* SendFn)(void*, DPID, DPID, DWORD, LPVOID, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetSessionDescFn)(void*, LPDPSESSIONDESC2, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* EnumConnectionsFn)(void*, LPCGUID, LPDPENUMCONNECTIONSCALLBACK, LPVOID, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* InitConnFn)(void*, LPVOID, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SendExFn)(void*, DPID, DPID, DWORD, LPVOID, DWORD, DWORD, DWORD, LPVOID, DWORD_PTR*);

CoCreateInstanceFn o_CoCreateInstance;
QIFn o_QI;
CloseFn o_Close;
CreatePlayerFn o_CreatePlayer;
DestroyPlayerFn o_DestroyPlayer;
EnumSessionsFn o_EnumSessions;
GetMessageCountFn o_GetMessageCount;
OpenFn o_Open;
ReceiveFn o_Receive;
SendFn o_Send;
SetSessionDescFn o_SetSessionDesc;
EnumConnectionsFn o_EnumConnections;
InitConnFn o_InitializeConnection;
SendExFn o_SendEx;

std::mutex g_mtx;
std::unique_ptr<NetStats> g_stats;
bool g_log = true;            // [net] log
bool g_playout_on = false;    // [net] freerun: race packets paced through the playout buffer
NetPlayout g_playout;
std::vector<uint8_t> g_rxbuf(0x10000);

template <typename... A>
void nlog(const char* fmt, A... a) {
    if (g_log) logf(fmt, a...);
}

int g_dumps = 3;
LARGE_INTEGER g_freq, g_t0;

double now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart;
}

unsigned g_playout_logged = 0;
void flush_stats(double t, double period = 1.0) {
    if (!g_stats) return;
    bool any = false;
    for (auto& l : g_stats->poll(t, period)) {
        nlog("net: %s", l.c_str());
        any = any || l.rfind("net ", 0) == 0;
    }
    if (any && g_playout_on && g_playout.delivered() != g_playout_logged) {
        g_playout_logged = g_playout.delivered();
        nlog("net: playout: %u race packets delivered paced, %u to catch up a sender, %u stale ones left out (totals)",
             g_playout.delivered(), g_playout.caught_up(), g_playout.dropped());
    }
}

std::string guid_name(const GUID& g) {
    if (g == DPSPGUID_TCPIP) return "TCP/IP";
    if (g == DPSPGUID_IPX) return "IPX";
    if (g == DPSPGUID_MODEM) return "modem";
    if (g == DPSPGUID_SERIAL) return "serial";
    return guid_str(&g);
}

// A DirectPlay address is a list of {GUID type, DWORD size, data}.
std::string describe_address(const void* addr, DWORD size) {
    std::string out;
    const uint8_t* p = (const uint8_t*)addr;
    DWORD off = 0;
    while (p && off + sizeof(GUID) + 4 <= size) {
        GUID type;
        DWORD n;
        std::memcpy(&type, p + off, sizeof type);
        std::memcpy(&n, p + off + sizeof(GUID), 4);
        const uint8_t* d = p + off + sizeof(GUID) + 4;
        if (off + sizeof(GUID) + 4 + n > size) break;
        if (!out.empty()) out += ", ";
        if (type == DPAID_ServiceProvider && n == sizeof(GUID)) {
            GUID sp;
            std::memcpy(&sp, d, sizeof sp);
            out += "service provider " + guid_name(sp);
        } else if (type == DPAID_INet) {
            out += "IP '" + std::string((const char*)d, n ? n - 1 : 0) + "'";
        } else if (type == DPAID_INetPort && n == 2) {
            uint16_t port;
            std::memcpy(&port, d, 2);
            out += "port " + std::to_string(port);
        } else if (type == DPAID_TotalSize) {
            out += "(total size)";
        } else {
            out += guid_str(&type) + " (" + std::to_string(n) + " bytes)";
        }
        off += sizeof(GUID) + 4 + n;
    }
    return out.empty() ? "(empty)" : out;
}

std::string describe_session(const DPSESSIONDESC2* d) {
    if (!d) return "(null)";
    char b[256];
    std::snprintf(b, sizeof b, "'%s' flags 0x%lx players %lu/%lu user 0x%lx 0x%lx 0x%lx 0x%lx",
                  d->lpszSessionNameA ? d->lpszSessionNameA : "", (unsigned long)d->dwFlags,
                  (unsigned long)d->dwCurrentPlayers, (unsigned long)d->dwMaxPlayers, (unsigned long)d->dwUser1,
                  (unsigned long)d->dwUser2, (unsigned long)d->dwUser3, (unsigned long)d->dwUser4);
    return b;
}

const char* sysmsg_name(DWORD t) {
    switch (t) {
        case DPSYS_CREATEPLAYERORGROUP: return "player joined";
        case DPSYS_DESTROYPLAYERORGROUP: return "player left";
        case DPSYS_ADDPLAYERTOGROUP: return "add to group";
        case DPSYS_DELETEPLAYERFROMGROUP: return "removed from group";
        case DPSYS_SESSIONLOST: return "SESSION LOST";
        case DPSYS_HOST: return "now host (host migration)";
        case DPSYS_SETPLAYERORGROUPDATA: return "player data";
        case DPSYS_SETPLAYERORGROUPNAME: return "player name";
        case DPSYS_SETSESSIONDESC: return "session description";
        case DPSYS_SENDCOMPLETE: return "send complete";
        default: return "other";
    }
}

void hook_slot(void** vtbl, int idx, void* fn, void** orig) {
    if (vtbl[idx] == fn) return;
    DWORD old;
    VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vtbl[idx];
    vtbl[idx] = fn;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &old);
}

void hook_dplay(void* obj);

HRESULT STDMETHODCALLTYPE h_QI(void* self, REFIID riid, void** out) {
    HRESULT hr = o_QI(self, riid, out);
    if (SUCCEEDED(hr) && out && *out && (riid == IID_IDirectPlay4A || riid == IID_IDirectPlay3A || riid == IID_IDirectPlay2A)) {
        nlog("net: QueryInterface %s", guid_str(&riid).c_str());
        hook_dplay(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE h_Close(void* self) {
    HRESULT hr = o_Close(self);
    std::lock_guard<std::mutex> lk(g_mtx);
    flush_stats(now(), 0.0);   // the partial last period, with its real length
    nlog("net: Close -> 0x%08lx", (unsigned long)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE h_CreatePlayer(void* self, LPDPID id, LPDPNAME name, HANDLE ev, LPVOID data, DWORD size, DWORD flags) {
    HRESULT hr = o_CreatePlayer(self, id, name, ev, data, size, flags);
    nlog("net: CreatePlayer '%s' flags 0x%lx data %lu bytes -> 0x%08lx id 0x%08lx",
         name && name->lpszShortNameA ? name->lpszShortNameA : "", (unsigned long)flags, (unsigned long)size,
         (unsigned long)hr, (unsigned long)(id ? *id : 0));
    return hr;
}

HRESULT STDMETHODCALLTYPE h_DestroyPlayer(void* self, DPID id) {
    HRESULT hr = o_DestroyPlayer(self, id);
    nlog("net: DestroyPlayer 0x%08lx -> 0x%08lx", (unsigned long)id, (unsigned long)hr);
    return hr;
}

struct EnumCtx {
    LPDPENUMSESSIONSCALLBACK2 cb;
    LPVOID ctx;
    int found;
    std::string list;
};

// GP4's games menu lists asynchronously, every frame: log a call only when its flags or
// what it found change, or every 30 s.
struct EnumLog {
    DWORD flags = ~0u;
    std::string found;
    double t = -1e9;
    unsigned quiet = 0;
} g_enum_log;

BOOL FAR PASCAL enum_sessions_quiet(LPCDPSESSIONDESC2 d, LPDWORD timeout, DWORD flags, LPVOID ctx) {
    auto* c = (EnumCtx*)ctx;
    if (!(flags & DPESC_TIMEDOUT)) {
        ++c->found;
        c->list += describe_session(d) + "\n";
    }
    return c->cb(d, timeout, flags, c->ctx);
}

HRESULT STDMETHODCALLTYPE h_EnumSessions(void* self, LPDPSESSIONDESC2 d, DWORD timeout, LPDPENUMSESSIONSCALLBACK2 cb, LPVOID ctx, DWORD flags) {
    EnumCtx c{cb, ctx, 0, {}};
    HRESULT hr = o_EnumSessions(self, d, timeout, cb ? enum_sessions_quiet : nullptr, cb ? (LPVOID)&c : ctx, flags);
    double t = now();
    std::lock_guard<std::mutex> lk(g_mtx);
    EnumLog& L = g_enum_log;
    if (flags != L.flags || c.list != L.found || t - L.t >= 30 || FAILED(hr)) {
        nlog("net: EnumSessions timeout %lu flags 0x%lx -> 0x%08lx, %d sessions%s", (unsigned long)timeout,
             (unsigned long)flags, (unsigned long)hr, c.found,
             L.quiet ? (" (" + std::to_string(L.quiet) + " identical calls not logged)").c_str() : "");
        size_t p = 0;
        for (size_t e; (e = c.list.find('\n', p)) != std::string::npos; p = e + 1)
            nlog("net:   session found: %s", c.list.substr(p, e - p).c_str());
        L.flags = flags;
        L.found = c.list;
        L.t = t;
        L.quiet = 0;
    } else {
        ++L.quiet;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE h_GetMessageCount(void* self, DPID id, LPDWORD n) { return o_GetMessageCount(self, id, n); }

// Race director choice ([net] race_director). GP4 times a loop at start-up (0x54c746,
// result in [0x833c98]), every PC sends that time when it creates or joins a game, and the
// fastest PC directs the race (0x541b03 orders the players by it, 0x4456c7 makes the
// first one the director). Writing another value right after Open, before GP4 reads it
// (0x53f80f creating, 0x5401a8 joining), picks the role:
//   prefer 1, avoid 0x7ffffff0, and auto (online through the server) 1000 + 10 x the
//   round trip to the server plus twice its jitter, in ms: the best connection directs.
//   Every PC on the server uses the same scale; the server PC itself (local) is at ~0.
int g_director = 0;   // 0 GP4 decides, 1 prefer, 2 avoid, 3 auto

bool readable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return a + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

void apply_race_director() {
    if (!g_director) return;
    uint32_t want;
    char why[96];
    if (g_director == 1) {
        want = 1;
        std::snprintf(why, sizeof why, "this PC if possible");
    } else if (g_director == 2) {
        want = 0x7FFFFFF0u;
        std::snprintf(why, sizeof why, "another PC if possible");
    } else {
        double rtt, jit;
        if (!online_open_quality(rtt, jit)) return;   // DirectPlay: GP4 decides
        double score = rtt + 2 * jit;
        want = 1000 + (uint32_t)std::min(1e8, score * 10);
        std::snprintf(why, sizeof why, "the best connection (here %.1f ms round trip, %.1f ms jitter)", rtt, jit);
    }
    static const uint8_t host_read[] = {0x8B, 0x1D, 0x98, 0x3C, 0x83, 0x00};   // mov ebx, [0x833c98]
    static const uint8_t join_read[] = {0xA1, 0x98, 0x3C, 0x83, 0x00};         // mov eax, [0x833c98]
    if (!readable(0x53F80F, sizeof host_read) || std::memcmp((void*)0x53F80F, host_read, sizeof host_read) ||
        !readable(0x5401A8, sizeof join_read) || std::memcmp((void*)0x5401A8, join_read, sizeof join_read) ||
        !readable(0x833C98, 4)) {
        nlog("net: race director (%s) not applied: other GP4 build", why);
        return;
    }
    uint32_t* speed = (uint32_t*)0x833C98;
    nlog("net: race director: %s; start-up speed test %lu replaced by %lu", why, (unsigned long)*speed, (unsigned long)want);
    *speed = want;
}

HRESULT STDMETHODCALLTYPE h_Open(void* self, LPDPSESSIONDESC2 d, DWORD flags) {
    std::string desc = describe_session(d);
    HRESULT hr = o_Open(self, d, flags);
    if (SUCCEEDED(hr)) apply_race_director();   // GP4 reads the value right after Open
    nlog("net: Open %s %s -> 0x%08lx", (flags & DPOPEN_CREATE) ? "CREATE (host)" : (flags & DPOPEN_JOIN) ? "JOIN" : "?",
         desc.c_str(), (unsigned long)hr);
    return hr;
}

// One message that came off the network: logged and counted as it arrives.
void note_received(double t, DPID from, const void* data, DWORD size) {
    if (from == DPID_SYSMSG && size >= 4) {
        DWORD type;
        std::memcpy(&type, data, 4);
        nlog("net: system message 0x%04lx (%s)", (unsigned long)type, sysmsg_name(type));
    } else if (g_stats) {
        g_stats->on_recv(t, from, (const uint8_t*)data, size);
    }
}

// DirectPlay system messages hold pointers to their own strings (player and session names):
// a message received at `from` and handed over at `to` gets them moved along.
void relocate(std::vector<uint8_t>& m, uintptr_t from, uintptr_t to) {
    for (size_t off = 0; off + sizeof(uintptr_t) <= m.size(); off += sizeof(uint32_t)) {
        uintptr_t v;
        std::memcpy(&v, &m[off], sizeof v);
        if (v >= from && v < from + m.size()) {
            v = to + (v - from);
            std::memcpy(&m[off], &v, sizeof v);
        }
    }
}

// Free-running races: everything waiting is pulled into the playout buffer and GP4 gets
// the next message that is due (core/net_playout.h).
HRESULT receive_paced(void* self, LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size, double t) {
    for (int guard = 0; guard < 1024; ++guard) {
        DPID f = 0, d = 0;
        DWORD n = (DWORD)g_rxbuf.size();
        HRESULT hr = o_Receive(self, &f, &d, flags, g_rxbuf.data(), &n);
        if (hr == (HRESULT)DPERR_BUFFERTOOSMALL && n > g_rxbuf.size()) {
            g_rxbuf.resize(n);
            continue;
        }
        if (hr != DP_OK) {
            if (hr != (HRESULT)DPERR_NOMESSAGES) nlog("net: Receive -> 0x%08lx", (unsigned long)hr);
            break;
        }
        note_received(t, f, g_rxbuf.data(), n);
        NetPlayout::Msg m;
        m.from = f;
        m.to = d;
        m.data.assign(g_rxbuf.data(), g_rxbuf.data() + n);
        m.origin = (uintptr_t)g_rxbuf.data();
        g_playout.push(std::move(m));
    }
    NetPlayout::Msg m;
    if (!g_playout.pop(t * 1000.0, m)) {
        if (g_stats) g_stats->on_empty_poll(t);
        return DPERR_NOMESSAGES;
    }
    if (!size || (data && *size < m.data.size()) || !data) {
        if (size) *size = (DWORD)m.data.size();
        g_playout.unpop(std::move(m));
        return DPERR_BUFFERTOOSMALL;
    }
    if (m.from == DPID_SYSMSG) relocate(m.data, m.origin, (uintptr_t)data);
    std::memcpy(data, m.data.data(), m.data.size());
    *size = (DWORD)m.data.size();
    if (from) *from = m.from;
    if (to) *to = m.to;
    return DP_OK;
}

HRESULT STDMETHODCALLTYPE h_Receive(void* self, LPDPID from, LPDPID to, DWORD flags, LPVOID data, LPDWORD size) {
    if (g_playout_on && (flags & DPRECEIVE_ALL) && !(flags & DPRECEIVE_PEEK)) {
        std::lock_guard<std::mutex> lk(g_mtx);
        double t = now();
        HRESULT hr = receive_paced(self, from, to, flags, data, size, t);
        flush_stats(t);
        return hr;
    }
    HRESULT hr = o_Receive(self, from, to, flags, data, size);
    std::lock_guard<std::mutex> lk(g_mtx);
    double t = now();
    if (hr == DP_OK && from && data && size) {
        note_received(t, *from, data, *size);
    } else if (hr == (HRESULT)DPERR_NOMESSAGES && g_stats) {
        g_stats->on_empty_poll(t);
    } else if (hr != (HRESULT)DPERR_BUFFERTOOSMALL) {
        nlog("net: Receive -> 0x%08lx", (unsigned long)hr);
    }
    flush_stats(t);
    return hr;
}

HRESULT STDMETHODCALLTYPE h_Send(void* self, DPID from, DPID to, DWORD flags, LPVOID data, DWORD size) {
    HRESULT hr = o_Send(self, from, to, flags, data, size);
    std::lock_guard<std::mutex> lk(g_mtx);
    double t = now();
    if (g_stats) g_stats->on_send(t, to, flags, (const uint8_t*)data, size);
    if (FAILED(hr)) nlog("net: Send %lu bytes to 0x%08lx flags 0x%lx -> 0x%08lx", (unsigned long)size, (unsigned long)to,
                         (unsigned long)flags, (unsigned long)hr);
    flush_stats(t);
    return hr;
}

HRESULT STDMETHODCALLTYPE h_SendEx(void* self, DPID from, DPID to, DWORD flags, LPVOID data, DWORD size, DWORD prio, DWORD timeout,
                                   LPVOID ctx, DWORD_PTR* msgid) {
    HRESULT hr = o_SendEx(self, from, to, flags, data, size, prio, timeout, ctx, msgid);
    std::lock_guard<std::mutex> lk(g_mtx);
    double t = now();
    if (g_stats) g_stats->on_send(t, to, flags, (const uint8_t*)data, size);
    flush_stats(t);
    return hr;
}

HRESULT STDMETHODCALLTYPE h_SetSessionDesc(void* self, LPDPSESSIONDESC2 d, DWORD flags) {
    std::string desc = describe_session(d);
    HRESULT hr = o_SetSessionDesc(self, d, flags);
    nlog("net: SetSessionDesc %s -> 0x%08lx", desc.c_str(), (unsigned long)hr);
    return hr;
}

struct ConnCtx {
    LPDPENUMCONNECTIONSCALLBACK cb;
    LPVOID ctx;
};

BOOL FAR PASCAL enum_conn_cb(LPCGUID sp, LPVOID conn, DWORD size, LPCDPNAME name, DWORD flags, LPVOID ctx) {
    auto* c = (ConnCtx*)ctx;
    nlog("net:   connection '%s' (%s)", name && name->lpszShortNameA ? name->lpszShortNameA : "", sp ? guid_name(*sp).c_str() : "?");
    return c->cb(sp, conn, size, name, flags, c->ctx);
}

HRESULT STDMETHODCALLTYPE h_EnumConnections(void* self, LPCGUID app, LPDPENUMCONNECTIONSCALLBACK cb, LPVOID ctx, DWORD flags) {
    nlog("net: EnumConnections flags 0x%lx", (unsigned long)flags);
    ConnCtx c{cb, ctx};
    HRESULT hr = o_EnumConnections(self, app, cb ? enum_conn_cb : nullptr, cb ? (LPVOID)&c : ctx, flags);
    if (FAILED(hr)) nlog("net: EnumConnections -> 0x%08lx", (unsigned long)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE h_InitializeConnection(void* self, LPVOID conn, DWORD flags) {
    // The connection buffer's size is not passed; DirectPlay addresses start with their
    // total size element when built by the lobby, otherwise read the SP element only.
    std::string what = "?";
    if (conn) {
        DWORD n;
        std::memcpy(&n, (const uint8_t*)conn + sizeof(GUID), 4);
        GUID first;
        std::memcpy(&first, conn, sizeof first);
        DWORD total = sizeof(GUID) + 4 + n;
        if (first == DPAID_TotalSize && n == 4) std::memcpy(&total, (const uint8_t*)conn + sizeof(GUID) + 4, 4);
        what = describe_address(conn, total);
    }
    HRESULT hr = o_InitializeConnection(self, conn, flags);
    nlog("net: InitializeConnection %s -> 0x%08lx", what.c_str(), (unsigned long)hr);
    return hr;
}

void hook_dplay(void* obj) {
    void** v = *(void***)obj;
    hook_slot(v, gp4_dp_QueryInterface, (void*)h_QI, (void**)&o_QI);
    hook_slot(v, gp4_dp_Close, (void*)h_Close, (void**)&o_Close);
    hook_slot(v, gp4_dp_CreatePlayer, (void*)h_CreatePlayer, (void**)&o_CreatePlayer);
    hook_slot(v, gp4_dp_DestroyPlayer, (void*)h_DestroyPlayer, (void**)&o_DestroyPlayer);
    hook_slot(v, gp4_dp_EnumSessions, (void*)h_EnumSessions, (void**)&o_EnumSessions);
    hook_slot(v, gp4_dp_GetMessageCount, (void*)h_GetMessageCount, (void**)&o_GetMessageCount);
    hook_slot(v, gp4_dp_Open, (void*)h_Open, (void**)&o_Open);
    hook_slot(v, gp4_dp_Receive, (void*)h_Receive, (void**)&o_Receive);
    hook_slot(v, gp4_dp_Send, (void*)h_Send, (void**)&o_Send);
    hook_slot(v, gp4_dp_SetSessionDesc, (void*)h_SetSessionDesc, (void**)&o_SetSessionDesc);
    hook_slot(v, gp4_dp_EnumConnections, (void*)h_EnumConnections, (void**)&o_EnumConnections);
    hook_slot(v, gp4_dp_InitializeConnection, (void*)h_InitializeConnection, (void**)&o_InitializeConnection);
    hook_slot(v, gp4_dp_SendEx, (void*)h_SendEx, (void**)&o_SendEx);
}

HRESULT WINAPI h_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* out) {
    // [net] server: GP4 gets the GP4 Ultimate server client instead of DirectPlay.
    if (online_active() && (clsid == CLSID_DirectPlay || clsid == CLSID_DirectPlayLobby)) {
        bool dp = clsid == CLSID_DirectPlay;
        HRESULT hr = outer ? CLASS_E_NOAGGREGATION : dp ? online_create_dplay(riid, out) : online_create_lobby(riid, out);
        nlog("net: CoCreateInstance(%s, %s) -> 0x%08lx (GP4 Ultimate server, not DirectPlay)",
             dp ? "DirectPlay" : "DirectPlayLobby", guid_str(&riid).c_str(), (unsigned long)hr);
        if (dp && SUCCEEDED(hr) && out && *out) {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_stats && g_log) g_stats.reset(new NetStats(g_dumps));
            hook_dplay(*out);
        }
        return hr;
    }
    HRESULT hr = o_CoCreateInstance(clsid, outer, ctx, riid, out);
    if (clsid == CLSID_DirectPlay) {
        nlog("net: CoCreateInstance(DirectPlay, %s) -> 0x%08lx%s", guid_str(&riid).c_str(), (unsigned long)hr,
             FAILED(hr) ? "  (is DirectPlay enabled in Windows features?)" : "");
        if (SUCCEEDED(hr) && out && *out) {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_stats && g_log) g_stats.reset(new NetStats(g_dumps));
            hook_dplay(*out);
        }
    } else if (clsid == CLSID_DirectPlayLobby) {
        nlog("net: CoCreateInstance(DirectPlayLobby) -> 0x%08lx", (unsigned long)hr);
    }
    return hr;
}

// Replaces one import of the main module; returns false if it is not imported.
bool patch_import(HMODULE mod, const char* dll, const char* fn, void* repl, void** orig) {
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    FARPROC target = GetProcAddress(GetModuleHandleA(dll), fn);
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (lstrcmpiA((const char*)(base + imp->Name), dll) != 0) continue;
        auto* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto* names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
        for (int i = 0; thunk[i].u1.Function; ++i) {
            bool match = target && (FARPROC)thunk[i].u1.Function == target;
            if (!match && names && !(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                auto* by = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                match = std::strcmp((const char*)by->Name, fn) == 0;
            }
            if (!match) continue;
            DWORD old;
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *orig = (void*)thunk[i].u1.Function;
            thunk[i].u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

}  // namespace

void install_net_hooks(int dumps, bool log, bool playout) {
    g_dumps = dumps;
    g_log = log;
    g_playout_on = playout;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    LoadLibraryA("ole32.dll");
    if (patch_import(GetModuleHandleA(nullptr), "ole32.dll", "CoCreateInstance", (void*)h_CoCreateInstance,
                     (void**)&o_CoCreateInstance)) {
        nlog("net: log on (DirectPlay calls and traffic summaries, %d packet dumps per kind)", dumps);
        if (playout) logf("net: race packets paced one per sender and step (playout buffer)");
    } else {
        logf("net: the game does not import CoCreateInstance - network hooks off");
    }
}

void net_playout_begin_step() {
    if (!g_playout_on) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_playout.begin_step(now() * 1000.0);
}

void net_set_race_director(int mode) { g_director = mode; }

NetPlayoutCounts net_playout_counts() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return {g_playout.delivered(), g_playout.caught_up()};
}

}  // namespace gp4u
