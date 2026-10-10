// End-to-end test of online play through the GP4 Ultimate server (src/win/dplay_online.cpp):
// imports CoCreateInstance like GP4.exe, loads our dinput8.dll (which patches that import),
// then plays three GP4s in one process, calling DirectPlay exactly as GP4 does
// (docs/GP4-FINDINGS.md): probe connections, build the TCP/IP address with the lobby,
// host, list, join, players, messages, closing the game, host migration.
//   test_dpserver local            the server runs inside the DLL ([net] server = local)
//   test_dpserver HOST:PORT        an outside server (tests/run_tests.sh starts gp4ultimate-server)
//   test_dpserver unreachable      nobody answers: GP4 gets errors, not a hang
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <dinput.h>
#include <dplay.h>
#include <dplobby.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::string g_dir;
static const char* g_mode = "local";

static std::string read_log() {
    std::ifstream f(g_dir + "\\gp4ultimate.log");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// GP4's application GUID for network games and its fixed session name (0x506e6c).
DEFINE_GUID(GP4_NETWORK_APP, 0x20e0ff41, 0x64f0, 0x11d1, 0xa6, 0xfb, 0x00, 0x60, 0xb0, 0x7d, 0x02, 0xd7);

struct Found {
    std::vector<DPSESSIONDESC2> list;
    std::vector<std::string> names;
    bool timed_out = false;
};

static BOOL FAR PASCAL session_cb(LPCDPSESSIONDESC2 d, LPDWORD, DWORD flags, LPVOID ctx) {
    auto* f = (Found*)ctx;
    if (flags & DPESC_TIMEDOUT) {
        f->timed_out = true;
        return FALSE;   // like GP4's 0x5078b0
    }
    f->list.push_back(*d);
    f->names.push_back(d->lpszSessionNameA ? d->lpszSessionNameA : "");
    return TRUE;
}

struct Conn {
    GUID sp{};
    std::vector<char> addr;
    int count = 0;
};

static BOOL FAR PASCAL conn_cb(LPCGUID sp, LPVOID conn, DWORD size, LPCDPNAME, DWORD, LPVOID ctx) {
    auto* c = (Conn*)ctx;
    ++c->count;
    c->sp = *sp;
    c->addr.assign((char*)conn, (char*)conn + size);
    return TRUE;
}

// GP4's 0x506d30 + 0x507000: a DirectPlay object and a TCP/IP address from the lobby.
static IDirectPlay4A* new_dp(IDirectPlayLobby3A* lobby) {
    IDirectPlay4A* dp = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DirectPlay, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlay4A, (void**)&dp)) || !dp)
        return nullptr;
    DPCOMPOUNDADDRESSELEMENT el[2];
    el[0].guidDataType = DPAID_ServiceProvider;
    el[0].dwDataSize = sizeof(GUID);
    el[0].lpData = (LPVOID)&DPSPGUID_TCPIP;
    el[1].guidDataType = DPAID_INet;   // the client's is empty (0x50723d)
    el[1].dwDataSize = 0;
    el[1].lpData = nullptr;
    DWORD size = 0;
    HRESULT hr = lobby->CreateCompoundAddress(el, 2, nullptr, &size);
    CHECK(hr == DPERR_BUFFERTOOSMALL && size > 0);
    std::vector<char> addr(size);
    CHECK(lobby->CreateCompoundAddress(el, 2, addr.data(), &size) == DP_OK);
    CHECK(dp->InitializeConnection(addr.data(), 0) == DP_OK);
    return dp;
}

static DPSESSIONDESC2 gp4_desc() {
    DPSESSIONDESC2 d;
    std::memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DPSESSION_MIGRATEHOST;
    d.guidApplication = GP4_NETWORK_APP;
    d.dwMaxPlayers = 22;
    d.lpszSessionNameA = (LPSTR) "GP3 2000 Network Session";
    return d;
}

static DPID new_player(IDirectPlay4A* dp, const char* name) {
    DPNAME n;
    std::memset(&n, 0, sizeof n);
    n.dwSize = sizeof n;
    n.lpszShortNameA = (LPSTR)name;
    DPID id = 0;
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CHECK(dp->CreatePlayer(&id, &n, ev, nullptr, 0, 0) == DP_OK);
    CHECK(id != 0);
    return id;
}

struct Msg {
    DPID from = 0, to = 0;
    std::vector<char> data;
};

// Everything that arrives within `ms`.
static std::vector<Msg> drain(IDirectPlay4A* dp, int ms) {
    std::vector<Msg> v;
    DWORD end = GetTickCount() + ms;
    do {
        for (;;) {
            char buf[4096];
            DPID from = 0, to = 0;
            DWORD n = sizeof buf;
            if (dp->Receive(&from, &to, DPRECEIVE_ALL, buf, &n) != DP_OK) break;
            Msg m;
            m.from = from;
            m.to = to;
            m.data.assign(buf, buf + n);
            // System messages carry pointers into the buffer: resolve the names now.
            if (from == DPID_SYSMSG) {
                DWORD type = *(DWORD*)buf;
                const char* name = nullptr;
                if (type == DPSYS_CREATEPLAYERORGROUP) name = ((DPMSG_CREATEPLAYERORGROUP*)buf)->dpnName.lpszShortNameA;
                if (type == DPSYS_DESTROYPLAYERORGROUP) name = ((DPMSG_DESTROYPLAYERORGROUP*)buf)->dpnName.lpszShortNameA;
                if (type == DPSYS_SETSESSIONDESC) name = ((DPMSG_SETSESSIONDESC*)buf)->dpDesc.lpszSessionNameA;
                if (name) {
                    CHECK(name >= buf && name < buf + n);
                    m.data.push_back(0);
                    m.data.insert(m.data.end(), name, name + std::strlen(name));
                }
            }
            v.push_back(m);
        }
        Sleep(10);
    } while (GetTickCount() < end);
    return v;
}

static int count_sys(const std::vector<Msg>& v, DWORD type, DPID id = 0, const char* name = nullptr) {
    int n = 0;
    for (auto& m : v) {
        if (m.from != DPID_SYSMSG || *(const DWORD*)m.data.data() != type) continue;
        if (id && ((const DPMSG_CREATEPLAYERORGROUP*)m.data.data())->dpId != id) continue;
        if (name && std::string(m.data.end() - std::strlen(name), m.data.end()) != name) continue;
        ++n;
    }
    return n;
}

// The games list as GP4's browse menu polls it (asynchronously, every frame).
static Found browse(IDirectPlay4A* dp, int ms, DWORD flags = DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ASYNC) {
    Found f;
    DWORD end = GetTickCount() + ms;
    for (;;) {
        f = Found();
        DPSESSIONDESC2 filter = gp4_desc();
        CHECK(dp->EnumSessions(&filter, 0, session_cb, &f, flags) == DP_OK);
        CHECK(f.timed_out);
        if (!f.list.empty() || GetTickCount() >= end) return f;
        Sleep(16);
    }
}

static void unreachable(IDirectPlayLobby3A* lobby) {
    IDirectPlay4A* dp = new_dp(lobby);
    if (!dp) return;
    Found f = browse(dp, 300);
    CHECK(f.list.empty());
    DPSESSIONDESC2 d = gp4_desc();
    DWORD t0 = GetTickCount();
    HRESULT hr = dp->Open(&d, DPOPEN_CREATE | DPOPEN_RETURNSTATUS);
    CHECK(hr == DPERR_NOCONNECTION);
    CHECK(GetTickCount() - t0 < 15000);
    dp->Close();
    dp->Release();
    std::string log = read_log();
    CHECK(log.find("net: no server") != std::string::npos);
}

static void play(IDirectPlayLobby3A* lobby) {
    // Xavi hosts.
    IDirectPlay4A* host = new_dp(lobby);
    if (!host) { CHECK(!"no DirectPlay object"); return; }
    DPSESSIONDESC2 d = gp4_desc();
    HRESULT hr = host->Open(&d, DPOPEN_CREATE | DPOPEN_RETURNSTATUS);
    CHECK(hr == DP_OK);
    if (hr != DP_OK) { std::printf("Open(CREATE) -> 0x%08lx\n", (unsigned long)hr); host->Release(); return; }
    DPID xavi = new_player(host, "Xavi");
    DPCAPS caps;
    std::memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    CHECK(host->GetCaps(&caps, 0) == DP_OK && caps.dwMaxBufferSize >= 4096);

    // Marcos finds it in the list, named after its host, and joins.
    IDirectPlay4A* marcos_dp = new_dp(lobby);
    Found f = browse(marcos_dp, 3000);
    CHECK(f.list.size() == 1);
    if (f.list.empty()) return;
    CHECK(f.names[0] == "Xavi 1/22");
    CHECK(f.list[0].dwCurrentPlayers == 1 && f.list[0].dwMaxPlayers == 22);
    DPSESSIONDESC2 j = gp4_desc();
    j.guidInstance = f.list[0].guidInstance;   // GP4 copies the chosen one (0x506f70)
    CHECK(marcos_dp->Open(&j, DPOPEN_JOIN) == DP_OK);
    DPID marcos = new_player(marcos_dp, "Marcos");
    // Mario too.
    IDirectPlay4A* mario_dp = new_dp(lobby);
    f = browse(mario_dp, 3000);
    CHECK(f.list.size() == 1 && f.names[0] == "Xavi 2/22");
    CHECK(mario_dp->Open(&j, DPOPEN_JOIN) == DP_OK);
    DPID mario = new_player(mario_dp, "Mario");
    CHECK(xavi != marcos && marcos != mario);

    auto hm = drain(host, 300);
    CHECK(count_sys(hm, DPSYS_CREATEPLAYERORGROUP, marcos, "Marcos") == 1);
    CHECK(count_sys(hm, DPSYS_CREATEPLAYERORGROUP, mario, "Mario") == 1);
    auto mm = drain(marcos_dp, 50);
    CHECK(count_sys(mm, DPSYS_CREATEPLAYERORGROUP, mario) == 1 && count_sys(mm, DPSYS_CREATEPLAYERORGROUP, xavi) == 0);
    drain(mario_dp, 50);

    // Race packets to everyone, a guaranteed menu message to Marcos only.
    char race[1400];   // the race master's packets: two datagrams, sent unreliably
    std::memset(race, 0x5a, sizeof race);
    race[3] = 0;
    race[5] = 1;
    char menu[] = {0x05, 0, 0, (char)0x81, 1, 2, 3};
    for (int i = 0; i < 10; ++i) CHECK(host->Send(xavi, DPID_ALLPLAYERS, 0, race, sizeof race) == DP_OK);
    CHECK(host->Send(xavi, marcos, DPSEND_GUARANTEED, menu, sizeof menu) == DP_OK);
    CHECK(host->Send(marcos, DPID_ALLPLAYERS, 0, race, sizeof race) == DPERR_INVALIDPLAYER);   // not ours
    Sleep(300);
    DWORD count = 0;
    CHECK(marcos_dp->GetMessageCount(marcos, &count) == DP_OK && count >= 5);
    // Too small a buffer: the size comes back and the message stays.
    char small[4];
    DPID from = 0, to = 0;
    DWORD n = sizeof small;
    CHECK(marcos_dp->Receive(&from, &to, DPRECEIVE_ALL, small, &n) == DPERR_BUFFERTOOSMALL &&
          (n == sizeof race || n == sizeof menu));
    mm = drain(marcos_dp, 200);
    auto om = drain(mario_dp, 50);
    int races_m = 0, menus_m = 0, races_o = 0, menus_o = 0;
    for (auto& m : mm) {
        if (m.data.size() == sizeof race) { ++races_m; CHECK(m.from == xavi && m.to == marcos); }
        if (m.data.size() == sizeof menu) ++menus_m;
    }
    for (auto& m : om) {
        if (m.data.size() == sizeof race) { ++races_o; CHECK(m.to == mario); }
        if (m.data.size() == sizeof menu) ++menus_o;
    }
    CHECK(races_m == 10 && menus_m == 1 && races_o == 10 && menus_o == 0);
    CHECK(drain(host, 50).empty());   // nothing comes back to the sender

    // The race starts: the host closes the game (0x507930).
    d.dwFlags |= DPSESSION_JOINDISABLED;
    CHECK(host->SetSessionDesc(&d, 0) == DP_OK);
    om = drain(mario_dp, 300);
    CHECK(count_sys(om, DPSYS_SETSESSIONDESC) == 1);
    IDirectPlay4A* late = new_dp(lobby);
    f = browse(late, 1200);
    CHECK(f.list.empty());   // not available any more
    f = browse(late, 1200, DPENUMSESSIONS_ALL | DPENUMSESSIONS_ASYNC);
    CHECK(f.list.size() == 1);
    CHECK(late->Open(&j, DPOPEN_JOIN) == DPERR_NONEWPLAYERS);

    // Xavi quits: Marcos becomes the host, both are told Xavi left.
    host->Close();
    host->Release();
    mm = drain(marcos_dp, 500);
    om = drain(mario_dp, 50);
    CHECK(count_sys(mm, DPSYS_DESTROYPLAYERORGROUP, xavi, "Xavi") == 1);
    CHECK(count_sys(om, DPSYS_DESTROYPLAYERORGROUP, xavi) == 1);
    CHECK(count_sys(mm, DPSYS_HOST) == 1 && count_sys(om, DPSYS_HOST) == 0);
    // The new host can reopen the game.
    d.dwFlags &= ~DPSESSION_JOINDISABLED;
    CHECK(marcos_dp->SetSessionDesc(&d, 0) == DP_OK);
    Sleep(300);
    f = browse(late, 2000);
    CHECK(f.list.size() == 1 && f.names[0] == "Marcos 2/22");

    late->Release();
    mario_dp->Close();
    mario_dp->Release();
    marcos_dp->Close();
    marcos_dp->Release();

    std::string log = read_log();
    CHECK(log.find("net: online through the GP4 Ultimate server") != std::string::npos);
    CHECK(log.find("net: connected to the server") != std::string::npos);
    CHECK(log.find("(GP4 Ultimate server, not DirectPlay)") != std::string::npos);
    CHECK(log.find("net: Open CREATE (host)") != std::string::npos);
    // auto: the director is chosen by the connection; this is not GP4, so it is only reported.
    CHECK(log.find("net: race director (the best connection (here ") != std::string::npos);
    CHECK(log.find("not applied: other GP4 build") != std::string::npos);
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "local";
    g_mode = argc > 1 ? argv[1] : "local";
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
    {
        std::ofstream f(g_dir + "\\gp4ultimate.ini");
        f << "[general]\nlog = 1\ninput = 0\n[net]\nlog = 1\ndump_packets = 1\n";
        if (mode == "local") f << "server = local\nserver_port = 24931\n";
        else if (mode == "unreachable") f << "server = 127.0.0.1:9\n";
        else f << "server = " << mode << "\n";
    }
    DeleteFileA((g_dir + "\\gp4ultimate.log").c_str());

    IDirectInput8A* di = nullptr;
    DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di, nullptr);
    CoInitialize(nullptr);

    // GP4's start-up probe (0x506ad0): connections of a throw-away object.
    IDirectPlay4A* probe = nullptr;
    CHECK(SUCCEEDED(CoCreateInstance(CLSID_DirectPlay, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlay4A, (void**)&probe)));
    Conn c;
    if (probe) {
        CHECK(probe->EnumConnections(nullptr, conn_cb, &c, 0) == DP_OK);
        CHECK(c.count == 1 && c.sp == DPSPGUID_TCPIP);
        IDirectPlay4A* test = nullptr;
        CoCreateInstance(CLSID_DirectPlay, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlay4A, (void**)&test);
        if (test) {
            CHECK(test->InitializeConnection(c.addr.data(), 0) == DP_OK);
            CHECK(test->InitializeConnection(c.addr.data(), 0) == DPERR_ALREADYINITIALIZED);
            test->Release();
        }
        probe->Release();
    }
    IDirectPlayLobby3A* lobby = nullptr;
    CHECK(SUCCEEDED(CoCreateInstance(CLSID_DirectPlayLobby, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlayLobby3A,
                                     (void**)&lobby)));
    if (lobby) {
        if (mode == "unreachable") unreachable(lobby);
        else play(lobby);
        lobby->Release();
    }
    if (di) di->Release();
    std::printf("[dpserver %s] %d passed, %d failed\n", mode.c_str(), g_pass, g_fail);
    if (g_fail) std::printf("%s\n", read_log().c_str());
    return g_fail ? 1 : 0;
}
