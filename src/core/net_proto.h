// Messages of the GP4 Ultimate online protocol, carried by core/net_channel.h.
//
// The server replaces what DirectPlay did between the players' PCs: it keeps the list of
// games, gives every player its id, tells the others when someone joins or leaves, picks
// a new host when the host leaves, and relays GP4's messages (to everyone or to one
// player, guaranteed or not). GP4 itself still runs the race: one player's GP4 is the race
// master, as with DirectPlay. docs/ONLINE.md describes the whole design.
//
// Every message starts with its type byte. Strings and SessionDesc are encoded with
// core/net_wire.h.
//
// Pure C++, no Windows: unit-tested in tests/test_online.cpp.
#pragma once
#include <cstdint>
#include <string>

#include "core/net_wire.h"

namespace gp4u {
namespace net {

constexpr uint32_t kProtocol = 2;   // 2: unreliable messages in parts (kind 4)
constexpr uint16_t kDefaultPort = 24004;

enum MsgType : uint8_t {
    // player -> server
    kHello = 1,        // u32 protocol, str mod version, str room, str player name
    kList = 2,         // guid application (null: all)
    kCreate = 3,       // u32 request, SessionDesc
    kJoin = 4,         // u32 request, guid instance
    kCreatePlayer = 5, // u32 request, str short name, str long name
    kSend = 6,         // u32 to (0: everyone else), u32 flags (1: guaranteed), data
    kSetDesc = 7,      // SessionDesc (host only)
    kLeave = 8,        // leave the game
    kBye = 9,          // leave the server
    // server -> player
    kWelcome = 101,    // u32 protocol, str server version, str your address as the server sees it
    kRefused = 102,    // str reason
    kSessions = 103,   // u16 count, SessionDesc...
    kResult = 104,     // u32 request, u32 HRESULT, then kCreate/kJoin: SessionDesc; kCreatePlayer: u32 id
    kData = 105,       // u32 from, u32 to, data
    kSystem = 106,     // u32 DPSYS_* type, then: player joined: u32 id, u32 players, str, str;
                       // player left: u32 id, str, str; session description: SessionDesc
};

// DirectPlay values the protocol shares with GP4 (dplay.h), so the core needs no Windows headers.
constexpr uint32_t kSysCreatePlayer = 0x0003;
constexpr uint32_t kSysDestroyPlayer = 0x0005;
constexpr uint32_t kSysSessionLost = 0x0031;
constexpr uint32_t kSysHost = 0x0101;
constexpr uint32_t kSysSetSessionDesc = 0x0104;

constexpr uint32_t kSessionNewPlayersDisabled = 0x01;
constexpr uint32_t kSessionMigrateHost = 0x04;
constexpr uint32_t kSessionJoinDisabled = 0x20;

constexpr uint32_t kSendGuaranteed = 0x01;

constexpr uint32_t kOk = 0;
constexpr uint32_t kErrCantAddPlayer = 0x88770028;
constexpr uint32_t kErrCantCreateSession = 0x88770046;
constexpr uint32_t kErrInvalidPlayer = 0x88770096;
constexpr uint32_t kErrNoConnection = 0x887700aa;
constexpr uint32_t kErrNoSessions = 0x887700dc;
constexpr uint32_t kErrTimeout = 0x887700f0;
constexpr uint32_t kErrNoNewPlayers = 0x8877014a;
constexpr uint32_t kErrInvalidParams = 0x80070057;

// GP4 copies session names into 32-byte slots without a length check (0x5078b0).
constexpr size_t kMaxSessionName = 31;
// GP4 receives into a 4 KB buffer and cannot skip a longer message (0x507360).
constexpr size_t kMaxGameMessage = 0x1000;

struct SessionDesc {
    Guid instance, application;
    uint32_t flags = 0, max_players = 0, players = 0;
    std::string name;
    uint32_t user[4] = {};
};

inline void write_desc(Writer& w, const SessionDesc& d) {
    w.guid(d.instance).guid(d.application).u32(d.flags).u32(d.max_players).u32(d.players).str(d.name);
    for (uint32_t u : d.user) w.u32(u);
}

inline SessionDesc read_desc(Reader& r) {
    SessionDesc d;
    d.instance = r.guid();
    d.application = r.guid();
    d.flags = r.u32();
    d.max_players = r.u32();
    d.players = r.u32();
    d.name = r.str();
    for (uint32_t& u : d.user) u = r.u32();
    return d;
}

// Open for new players (what DPENUMSESSIONS_AVAILABLE lists).
inline bool joinable(const SessionDesc& d) {
    return !(d.flags & (kSessionJoinDisabled | kSessionNewPlayersDisabled)) && d.players < d.max_players;
}

}  // namespace net
}  // namespace gp4u
