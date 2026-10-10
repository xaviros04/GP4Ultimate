// A player's side of the GP4 Ultimate online protocol (core/net_proto.h): what the
// DirectPlay replacement in src/win/dplay_client.cpp asks the server, and what comes back.
//
// Pure C++, no sockets: the owner moves datagrams in and out and calls poll() every few
// milliseconds. Unit-tested against core/net_server.h in tests/test_online.cpp.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "core/net_channel.h"
#include "core/net_proto.h"

namespace gp4u {
namespace net {

// Something for GP4's Receive: a game message or a DirectPlay system message.
struct Event {
    bool system = false;
    uint32_t from = 0, to = 0;   // game message
    Bytes data;
    uint32_t type = 0;           // system message (kSys*)
    uint32_t id = 0, players = 0;
    std::string short_name, long_name;
    SessionDesc desc;
};

class Client {
public:
    enum class State { Connecting, Ready, Refused, Lost, Closed };

    struct Result {
        uint32_t hr = 0;
        SessionDesc desc;
        uint32_t id = 0;
    };

    Client(uint32_t token, std::string version, std::string room, std::string name);

    // Sends the hello; the server answers with its welcome (state() becomes Ready).
    void start(double now, std::vector<Bytes>& out);

    void request_list(const Guid& app, double now, std::vector<Bytes>& out);
    uint32_t create(const SessionDesc& d, double now, std::vector<Bytes>& out);
    uint32_t join(const Guid& instance, double now, std::vector<Bytes>& out);
    uint32_t create_player(const std::string& short_name, const std::string& long_name, double now,
                           std::vector<Bytes>& out);
    void send(uint32_t to, bool guaranteed, const uint8_t* d, size_t n, double now, std::vector<Bytes>& out);
    void set_desc(const SessionDesc& d, double now, std::vector<Bytes>& out);
    void leave(double now, std::vector<Bytes>& out);
    // Leaves the server (Closed). Keep polling for a moment so the goodbye gets through.
    void bye(double now, std::vector<Bytes>& out);
    bool idle() const { return ch_.unacked() == 0; }

    // A datagram from the server; false if it is not one of ours.
    bool receive(const uint8_t* d, size_t n, double now, std::vector<Bytes>& out);
    void poll(double now, std::vector<Bytes>& out);

    State state() const { return state_; }
    const std::string& problem() const { return problem_; }   // why Refused / Lost
    const std::string& server_version() const { return server_version_; }
    const std::string& seen_as() const { return seen_as_; }    // our address as the server sees it
    double rtt() const { return ch_.rtt(); }
    double jitter() const { return ch_.rtt_var(); }

    // Latest games list and when it arrived (-1: none yet).
    const std::vector<SessionDesc>& sessions() const { return sessions_; }
    double sessions_time() const { return sessions_time_; }

    // The answer to a request, once (false while it has not arrived).
    bool take_result(uint32_t req, Result& r);

    std::deque<Event>& events() { return events_; }
    bool in_session() const { return in_session_; }
    uint32_t my_id() const { return my_id_; }
    double last_heard() const { return ch_.last_heard(); }

    static constexpr double kConnectTimeout = 6;    // no welcome in this time: unreachable
    static constexpr double kSilence = 15;          // nothing heard for this long: lost

private:
    void msg(const Writer& w, bool reliable, double now, std::vector<Bytes>& out) { ch_.send(w.buf, reliable, now, out); }
    void handle(const Bytes& m, double now);
    void lost(const std::string& why);

    Channel ch_;
    std::string version_, room_, name_;
    State state_ = State::Connecting;
    std::string problem_, server_version_, seen_as_;
    double started_ = 0, closed_ = 0;
    uint32_t next_req_ = 1;
    std::map<uint32_t, Result> results_;
    std::vector<SessionDesc> sessions_;
    double sessions_time_ = -1;
    std::deque<Event> events_;
    bool in_session_ = false;
    uint32_t my_id_ = 0;
};

}  // namespace net
}  // namespace gp4u
