// Online play through the GP4 Ultimate server instead of DirectPlay.
//
// With [net] server set, the DirectPlay objects GP4 creates (CoCreateInstance, patched by
// win/net_hook.cpp) are this module's: an IDirectPlay4A and an IDirectPlayLobby3A that do
// exactly what GP4 uses of DirectPlay (docs/GP4-FINDINGS.md "Lo que GP4 usa de
// DirectPlay") over one UDP conversation with the server (core/net_client.h). GP4 still
// sees "TCP/IP", a games list, players joining and leaving, and host migration. No
// DirectPlay Windows feature, no VPN, and no open port on the players' PCs.
//
// server = local runs the server inside this GP4 as well (others then use this PC's
// address); see docs/ONLINE.md.
#pragma once
#include <cstdint>
#include <string>

#include <windows.h>

namespace gp4u {

struct OnlineConfig {
    std::string server;        // "" off, "local", or "host[:port]"
    uint16_t port = 24004;     // server = local: UDP port to listen on
    std::string room;          // only games of the same room are listed
};

void online_setup(const OnlineConfig& c);
bool online_active();
std::string online_summary();   // for the log

// Round trip to the server (smoothed) and its jitter, both ms, of the game opened last.
bool online_open_quality(double& rtt_ms, double& jitter_ms);

HRESULT online_create_dplay(REFIID riid, void** out);
HRESULT online_create_lobby(REFIID riid, void** out);

}  // namespace gp4u
