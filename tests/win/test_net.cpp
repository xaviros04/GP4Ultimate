// End-to-end test of the network log (src/win/net_hook.cpp): imports CoCreateInstance like
// GP4.exe, loads our dinput8.dll (which patches that import), then drives the system's
// DirectPlay like GP4 does: enumerate connections, TCP/IP, host a session, create a player,
// send and poll. Checks the "net:" lines in gp4ultimate.log. Under Wine the DirectPlay
// implementation is Wine's; on Windows it needs the legacy DirectPlay feature.
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

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::string g_dir;

static std::string read_log() {
    std::ifstream f(g_dir + "\\gp4ultimate.log");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static BOOL FAR PASCAL conn_cb(LPCGUID, LPVOID, DWORD, LPCDPNAME, DWORD, LPVOID) { return TRUE; }

int main() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
    { std::ofstream f(g_dir + "\\gp4ultimate.ini"); f << "[general]\nlog = 1\ninput = 0\n[net]\nlog = 1\ndump_packets = 2\n"; }
    DeleteFileA((g_dir + "\\gp4ultimate.log").c_str());

    // Loading our dinput8 (first DirectInput8Create) installs the CoCreateInstance hook.
    IDirectInput8A* di = nullptr;
    DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di, nullptr);
    CHECK(read_log().find("net: log on") != std::string::npos);

    CoInitialize(nullptr);
    IDirectPlay4A* dp = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_DirectPlay, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlay4A, (void**)&dp);
    CHECK(read_log().find("net: CoCreateInstance(DirectPlay") != std::string::npos);
    if (FAILED(hr) || !dp) {
        std::printf("(no DirectPlay on this system: 0x%08lx - only the hook was tested)\n", (unsigned long)hr);
    } else {
        dp->EnumConnections(nullptr, conn_cb, nullptr, 0);
        CHECK(read_log().find("net: EnumConnections") != std::string::npos);
        // TCP/IP connection, address built like GP4 does (service provider element).
        IDirectPlayLobby3A* lobby = nullptr;
        CoCreateInstance(CLSID_DirectPlayLobby, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlayLobby3A, (void**)&lobby);
        char addr[256];
        DWORD asize = sizeof addr;
        bool addr_ok = false;
        if (lobby) {
            DPCOMPOUNDADDRESSELEMENT el[2];
            el[0].guidDataType = DPAID_ServiceProvider; el[0].dwDataSize = sizeof(GUID); el[0].lpData = (LPVOID)&DPSPGUID_TCPIP;
            el[1].guidDataType = DPAID_INet; el[1].dwDataSize = 10; el[1].lpData = (LPVOID)"127.0.0.1";
            addr_ok = SUCCEEDED(lobby->CreateCompoundAddress(el, 2, addr, &asize));
        }
        if (addr_ok) {
            hr = dp->InitializeConnection(addr, 0);
            std::string log = read_log();
            CHECK(log.find("net: InitializeConnection") != std::string::npos);
            CHECK(log.find("service provider TCP/IP") != std::string::npos);
            CHECK(log.find("IP '127.0.0.1'") != std::string::npos);
        }
        DPSESSIONDESC2 sd{};
        sd.dwSize = sizeof sd;
        sd.dwFlags = DPSESSION_MIGRATEHOST;
        sd.guidApplication = {0x12345678, 1, 2, {3, 4, 5, 6, 7, 8, 9, 10}};
        sd.dwMaxPlayers = 22;
        sd.lpszSessionNameA = (LPSTR)"GP3 2000 Network Session";
        hr = dp->Open(&sd, DPOPEN_CREATE);
        CHECK(read_log().find("net: Open CREATE (host) 'GP3 2000 Network Session'") != std::string::npos);
        if (SUCCEEDED(hr)) {
            DPID me = 0;
            DPNAME name{};
            name.dwSize = sizeof name;
            name.lpszShortNameA = (LPSTR)"Tester";
            dp->CreatePlayer(&me, &name, nullptr, nullptr, 0, 0);
            unsigned char pkt[64] = {0x05, 0x00, 0x00, 0x81};   // a menu message (bit 31 set, type 1)
            for (int i = 0; i < 30; ++i) {
                dp->Send(me, DPID_ALLPLAYERS, 0, pkt, sizeof pkt);
                char buf[4096];
                DPID from = 0, to = 0;
                DWORD n = sizeof buf;
                while (SUCCEEDED(dp->Receive(&from, &to, DPRECEIVE_ALL, buf, &n))) n = sizeof buf;
                Sleep(50);
            }
            std::string log = read_log();
            CHECK(log.find("net: CreatePlayer 'Tester'") != std::string::npos);
            CHECK(log.find("net: packet out menu1") != std::string::npos);
            CHECK(log.find("net: net ") != std::string::npos && log.find("out menu1=") != std::string::npos);
        } else {
            std::printf("(Open failed 0x%08lx on this system: session part not tested)\n", (unsigned long)hr);
        }
        dp->Close();
        CHECK(read_log().find("net: Close") != std::string::npos);
        dp->Release();
        if (lobby) lobby->Release();
    }
    if (di) di->Release();
    std::printf("[net] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
