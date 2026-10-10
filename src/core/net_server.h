// The GP4 Ultimate game server: games list, player ids, joins and leaves, host migration
// and message relay (core/net_proto.h). Used by tools/server (gp4ultimate-server) and,
// for a player who hosts it inside GP4, by src/win/dplay_server.cpp.
//
// Pure C++, no sockets: the owner moves datagrams in and out and calls poll() every few
// milliseconds. Unit-tested in tests/test_online.cpp.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/net_channel.h"
#include "core/net_proto.h"

namespace gp4u {
namespace net {

struct Endpoint {
    uint32_t ip = 0;     // IPv4, host byte order
    uint16_t port = 0;
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
    std::string str() const;
};

struct ServerConfig {
    std::string version;            // shown to players (the mod version of the build)
    size_t max_clients = 256;
    size_t max_sessions = 64;
    uint32_t max_players = 22;      // per game (GP4 asks for 22)
    double timeout = 15;            // seconds without a datagram from a player
    double hello_timeout = 5;       // ... from one that has not said hello
    uint32_t first_id = 0x10001;    // first player id handed out
};

class Server {
public:
    struct Out {
        Endpoint to;
        Bytes data;
    };

    explicit Server(ServerConfig c = {}) : cfg_(std::move(c)), next_id_(cfg_.first_id) {}

    void receive(const Endpoint& from, const uint8_t* d, size_t n, double now, std::vector<Out>& out);
    void poll(double now, std::vector<Out>& out);

    // What happened since the last call, one line per event, for the console.
    std::vector<std::string> take_log();

    size_t clients() const;   // connected (said hello)
    size_t sessions() const { return sessions_.size(); }
    // Every game as the list shows it (tests and the console status line).
    std::vector<SessionDesc> list(const std::string& version, const std::string& room) const;

private:
    struct Player {
        uint32_t id = 0;
        std::string short_name, long_name;
    };
    struct Client {
        uint32_t token = 0;
        Endpoint ep;
        Channel ch;
        bool welcomed = false;
        std::string version, room, name;
        uint32_t session = 0;     // 0: in no game
        std::vector<Player> players;
    };
    struct Session {
        uint32_t id = 0;
        SessionDesc desc;          // as the host set it (players kept up to date)
        std::string version, room;
        uint32_t host = 0;         // token of the hosting player
        std::vector<uint32_t> members;   // tokens, in joining order
    };

    void handle(Client& c, const Bytes& m, double now, std::vector<Out>& out);
    void send(Client& c, const Writer& w, bool reliable, double now, std::vector<Out>& out);
    void result(Client& c, uint32_t req, uint32_t hr, double now, std::vector<Out>& out, const SessionDesc* d = nullptr,
                uint32_t id = 0);
    void leave(Client& c, double now, std::vector<Out>& out, const char* why);
    SessionDesc shown(const Session& s) const;
    Session* session_of(const Client& c);
    std::string who(const Client& c) const;
    void log(const std::string& s) { log_.push_back(s); }

    ServerConfig cfg_;
    std::map<uint32_t, Client> clients_;     // by token
    std::map<uint32_t, Session> sessions_;   // by id
    uint32_t next_session_ = 1, next_id_;
    uint64_t guid_state_ = 0x9e3779b97f4a7c15ull;
    std::vector<std::string> log_;
};

}  // namespace net
}  // namespace gp4u
