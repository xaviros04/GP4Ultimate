// Native unit tests for the online server and its protocol (src/core/net_*), driven
// through a simulated network with delay, jitter, loss and duplicates.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/net_channel.h"
#include "core/net_client.h"
#include "core/net_proto.h"
#include "core/net_server.h"
#include "core/net_wire.h"

using namespace gp4u;
using namespace gp4u::net;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// Deterministic random numbers.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)s;
    }
    double unit() { return (next() & 0xffffff) / double(0x1000000); }
};

struct LinkCfg {
    double delay = 0.020, jitter = 0.0, loss = 0.0, dup = 0.0;
};

// In-flight datagrams between endpoints.
struct Net {
    struct Packet {
        double at;
        Endpoint from, to;
        Bytes data;
    };
    LinkCfg cfg;
    Rng rng{12345};
    std::vector<Packet> air;
    std::map<uint64_t, bool> down;   // endpoints that no longer receive or send
    unsigned sent = 0, lost = 0;

    static uint64_t key(const Endpoint& e) { return (uint64_t)e.ip << 16 | e.port; }
    void put(double now, const Endpoint& from, const Endpoint& to, Bytes d) {
        ++sent;
        if (down[key(from)] || rng.unit() < cfg.loss) { ++lost; return; }
        int copies = rng.unit() < cfg.dup ? 2 : 1;
        for (int i = 0; i < copies; ++i)
            air.push_back({now + cfg.delay + cfg.jitter * rng.unit(), from, to, d});
    }
    // Datagrams due by `now`, in arrival order.
    std::vector<Packet> due(double now) {
        std::vector<Packet> v, keep;
        for (auto& p : air) (p.at <= now ? v : keep).push_back(std::move(p));
        air.swap(keep);
        std::stable_sort(v.begin(), v.end(), [](const Packet& a, const Packet& b) { return a.at < b.at; });
        std::vector<Packet> r;
        for (auto& p : v)
            if (!down[key(p.to)]) r.push_back(std::move(p));
        return r;
    }
};

static void test_wire() {
    Writer w;
    Guid g;
    for (int i = 0; i < 16; ++i) g.b[i] = (uint8_t)(i * 7 + 1);
    w.u8(0xab).u16(0x1234).u32(0xdeadbeef).guid(g).str("Xavi").bytes("abc", 3);
    Reader r(w.buf);
    CHECK(r.u8() == 0xab);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0xdeadbeef);
    CHECK(r.guid() == g);
    CHECK(r.str() == "Xavi");
    CHECK(r.bytes(10) == Bytes({'a', 'b', 'c'}));
    CHECK(r.ok() && r.left() == 0);
    CHECK(r.u32() == 0 && !r.ok());   // past the end: zeros, not ok
    Reader r2(w.buf.data(), 4);
    r2.u8();
    r2.u32();
    CHECK(!r2.ok());
    // Bytes longer than allowed are refused.
    Reader r3(w.buf);
    r3.u8(); r3.u16(); r3.u32(); r3.guid(); r3.str();
    CHECK(r3.bytes(2).empty() && !r3.ok());

    SessionDesc d;
    d.instance = g;
    d.flags = kSessionMigrateHost;
    d.max_players = 22;
    d.players = 3;
    d.name = "GP3 2000 Network Session";
    d.user[2] = 77;
    Writer w2;
    write_desc(w2, d);
    Reader r4(w2.buf);
    SessionDesc e = read_desc(r4);
    CHECK(r4.ok() && e.instance == g && e.flags == d.flags && e.max_players == 22 && e.players == 3 && e.name == d.name &&
          e.user[2] == 77);
    CHECK(joinable(e));
    e.flags |= kSessionJoinDisabled;
    CHECK(!joinable(e));
    e.flags = 0;
    e.players = 22;
    CHECK(!joinable(e));

    uint32_t token;
    uint8_t kind;
    Bytes junk(kHeaderSize, 0);
    junk[0] = 'G', junk[1] = '4', junk[2] = 9, junk[3] = kUnreliable;
    CHECK(!peek_header(junk.data(), junk.size(), token, kind));   // other wire version
    junk[2] = kWireVersion;
    CHECK(peek_header(junk.data(), junk.size(), token, kind) && kind == kUnreliable);
    CHECK(!peek_header(junk.data(), 10, token, kind));
}

// Two channels over a bad link: reliable messages arrive once and in order, whatever happens.
static void channel_run(LinkCfg link, uint64_t seed, const char* what, int steps = 8000) {
    Channel a(0x1111), b(0x1111);
    Net net;
    net.cfg = link;
    net.rng = Rng(seed);
    Endpoint ea{1, 1}, eb{2, 2};
    std::vector<Bytes> got_rel, got_unrel;
    std::vector<Bytes> want_rel;
    unsigned unrel_sent = 0;
    Rng msgs(seed + 1);
    double t = 0;
    for (int step = 0; step < steps; ++step, t += 0.001) {
        std::vector<Bytes> out;
        if (step < 3000 && step % 7 == 0) {
            size_t n = (step % 700 == 0) ? 5000 : 1 + msgs.next() % 1500;   // some need several datagrams
            Bytes m(n);
            for (auto& c : m) c = (uint8_t)msgs.next();
            m[0] = 'R';
            want_rel.push_back(m);
            a.send(m, true, t, out);
        }
        if (step < 3000 && step % 16 == 0) {
            Bytes m(step % 32 == 0 ? 1400 : 700, 'U');   // the race master's packets are 1400 bytes
            m[1] = (uint8_t)(m.size() >> 8);
            m[2] = (uint8_t)m.size();
            ++unrel_sent;
            a.send(m, false, t, out);
        }
        a.poll(t, out);
        for (auto& d : out) net.put(t, ea, eb, std::move(d));
        out.clear();
        b.poll(t, out);
        for (auto& d : out) net.put(t, eb, ea, std::move(d));
        for (auto& p : net.due(t)) {
            std::vector<Bytes> in, back;
            if (p.to == eb) {
                CHECK(b.receive(p.data.data(), p.data.size(), t, in, back));
                for (auto& m : in) (m[0] == 'U' ? got_unrel : got_rel).push_back(m);
            } else {
                CHECK(a.receive(p.data.data(), p.data.size(), t, in, back));
            }
        }
    }
    bool same = got_rel.size() == want_rel.size();
    for (size_t i = 0; same && i < want_rel.size(); ++i) same = got_rel[i] == want_rel[i];
    if (!same) std::printf("  %s: %zu of %zu reliable messages in order\n", what, got_rel.size(), want_rel.size());
    CHECK(same);
    CHECK(a.unacked() == 0);
    for (auto& m : got_unrel) CHECK(m.size() == (size_t)(m[1] << 8 | m[2]));   // whole, never half
    if (link.loss == 0) CHECK(a.stats().resent == 0);   // no spurious resends (nor of unreliable parts)
    CHECK(got_unrel.size() <= unrel_sent * (link.dup > 0 ? 2 : 1));
    if (link.loss == 0 && link.dup == 0) CHECK(got_unrel.size() == unrel_sent);
    double rtt_min = 2 * link.delay - 0.002, rtt_max = 2 * (link.delay + link.jitter) + 0.002;
    if (a.rtt() < rtt_min || a.rtt() > rtt_max) std::printf("  %s: rtt %.3f\n", what, a.rtt());
    CHECK(a.rtt() >= rtt_min && a.rtt() <= rtt_max);
    CHECK(!a.failed(t));
}

static void test_channel() {
    channel_run({0.020, 0.0, 0.0, 0.0}, 1, "clean");
    channel_run({0.030, 0.040, 0.20, 0.05}, 2, "20% loss, jitter, duplicates");
    channel_run({0.150, 0.100, 0.35, 0.10}, 3, "35% loss, 150-250 ms", 30000);

    // Keep-alives: an idle channel still sends once a second; a dead peer is noticed.
    Channel a(7);
    std::vector<Bytes> out;
    a.poll(0, out);
    CHECK(out.size() == 1);   // first keep-alive
    out.clear();
    a.poll(0.5, out);
    CHECK(out.empty());
    a.poll(1.01, out);
    CHECK(out.size() == 1);
    out.clear();
    Bytes m = {1, 2, 3};
    a.send(m, true, 2, out);
    for (double t = 2; t < 30; t += 0.01) a.poll(t, out);
    CHECK(a.failed(30));
    CHECK(a.stats().resent > 5 && a.stats().resent < 40);   // backs off to one try a second
    // A datagram for another conversation is refused.
    Channel b(8);
    std::vector<Bytes> in, back;
    CHECK(!b.receive(out[0].data(), out[0].size(), 1, in, back));
}

// Server plus players on one simulated network.
struct World {
    Net net;
    Server server;
    Endpoint server_ep{0x0a000001, kDefaultPort};
    struct Player {
        Endpoint ep;
        std::unique_ptr<Client> c;
        bool alive = true;
    };
    std::vector<Player> players;
    double t = 0;

    explicit World(LinkCfg link = {}, ServerConfig sc = {}) : server(sc) {
        net.cfg = link;
        if (sc.version.empty()) server = Server(ServerConfig{"test"});
    }
    Client& add(const std::string& name, const std::string& version = "1.1.0", const std::string& room = "") {
        Player p;
        p.ep = Endpoint{0xc0a80000u + (uint32_t)players.size() + 2, (uint16_t)(50000 + players.size())};
        p.c.reset(new Client(0x100 + (uint32_t)players.size(), version, room, name));
        std::vector<Bytes> out;
        p.c->start(t, out);
        for (auto& d : out) net.put(t, p.ep, server_ep, std::move(d));
        players.push_back(std::move(p));
        return *players.back().c;
    }
    void out_of(size_t i, std::vector<Bytes>& out) {
        for (auto& d : out) net.put(t, players[i].ep, server_ep, std::move(d));
        out.clear();
    }
    // Runs the network for `secs`.
    void run(double secs) {
        double end = t + secs;
        for (; t < end; t += 0.002) {
            std::vector<Server::Out> so;
            for (auto& p : net.due(t)) {
                if (p.to == server_ep) {
                    server.receive(p.from, p.data.data(), p.data.size(), t, so);
                } else {
                    for (size_t i = 0; i < players.size(); ++i)
                        if (players[i].ep == p.to && players[i].alive) {
                            std::vector<Bytes> out;
                            players[i].c->receive(p.data.data(), p.data.size(), t, out);
                            out_of(i, out);
                        }
                }
            }
            server.poll(t, so);
            for (auto& o : so) net.put(t, server_ep, o.to, std::move(o.data));
            for (size_t i = 0; i < players.size(); ++i) {
                if (!players[i].alive) continue;
                std::vector<Bytes> out;
                players[i].c->poll(t, out);
                out_of(i, out);
            }
        }
    }
    // Waits for the answer to a request.
    Client::Result wait(size_t i, uint32_t req, double max = 5) {
        Client::Result r;
        r.hr = 0xffffffff;
        for (double end = t + max; t < end && !players[i].c->take_result(req, r);) run(0.01);
        return r;
    }
    template <typename F>
    uint32_t call(size_t i, F f) {
        std::vector<Bytes> out;
        uint32_t req = f(*players[i].c, out);
        out_of(i, out);
        return req;
    }
    void kill(size_t i) {   // the PC disappears without a word
        players[i].alive = false;
        net.down[Net::key(players[i].ep)] = true;
    }
};

static size_t count_events(Client& c, uint32_t type, uint32_t id = 0) {
    size_t n = 0;
    for (auto& e : c.events())
        if (e.system && e.type == type && (!id || e.id == id)) ++n;
    return n;
}

static std::vector<Event> data_events(Client& c) {
    std::vector<Event> v;
    for (auto& e : c.events())
        if (!e.system) v.push_back(e);
    return v;
}

static void scenario(LinkCfg link, const char* what) {
    std::printf("  scenario: %s\n", what);
    World w(link);
    Client& a = w.add("Xavi");
    Client& b = w.add("Marcos");
    Client& c = w.add("Mario");
    Client& other_room = w.add("Pepe", "1.1.0", "room2");
    Client& old = w.add("Old", "1.0.1");
    w.run(1);
    CHECK(a.state() == Client::State::Ready && b.state() == Client::State::Ready && c.state() == Client::State::Ready);
    CHECK(a.server_version() == "test");
    CHECK(a.seen_as() == w.players[0].ep.str());
    CHECK(w.server.clients() == 5);

    // Xavi hosts, like GP4: Open(CREATE), CreatePlayer.
    Guid app;
    app.b[0] = 0x41;
    SessionDesc d;
    d.application = app;
    d.flags = kSessionMigrateHost;
    d.max_players = 22;
    d.name = "GP3 2000 Network Session";
    uint32_t req = w.call(0, [&](Client& cl, std::vector<Bytes>& o) { return cl.create(d, w.t, o); });
    Client::Result r = w.wait(0, req);
    CHECK(r.hr == kOk && !r.desc.instance.null() && a.in_session());
    Guid inst = r.desc.instance;
    req = w.call(0, [&](Client& cl, std::vector<Bytes>& o) { return cl.create_player("Xavi", "", w.t, o); });
    r = w.wait(0, req);
    uint32_t id_a = r.id;
    CHECK(r.hr == kOk && id_a >= 0x10001 && a.my_id() == id_a);

    // The others see it, named after its host; another room and another version do not.
    for (size_t i : {1, 3, 4}) w.call(i, [&](Client& cl, std::vector<Bytes>& o) { cl.request_list(app, w.t, o); return 0u; });
    w.run(0.5);
    CHECK(b.sessions().size() == 1 && b.sessions()[0].instance == inst && b.sessions()[0].name == "Xavi 1/22" &&
          b.sessions()[0].players == 1 && joinable(b.sessions()[0]));
    CHECK(other_room.sessions().empty() && other_room.sessions_time() >= 0);
    CHECK(old.sessions().empty() && old.sessions_time() >= 0);
    Guid other_app;
    other_app.b[0] = 0x40;
    w.call(1, [&](Client& cl, std::vector<Bytes>& o) { cl.request_list(other_app, w.t, o); return 0u; });
    w.run(0.5);
    CHECK(b.sessions().empty());   // the 2-player GP4 game type is another application

    // Marcos and Mario join and create their players; the host hears about each.
    req = w.call(1, [&](Client& cl, std::vector<Bytes>& o) { return cl.join(inst, w.t, o); });
    CHECK(w.wait(1, req).hr == kOk && b.in_session());
    req = w.call(1, [&](Client& cl, std::vector<Bytes>& o) { return cl.create_player("Marcos", "", w.t, o); });
    uint32_t id_b = w.wait(1, req).id;
    req = w.call(2, [&](Client& cl, std::vector<Bytes>& o) { return cl.join(inst, w.t, o); });
    CHECK(w.wait(2, req).hr == kOk);
    req = w.call(2, [&](Client& cl, std::vector<Bytes>& o) { return cl.create_player("Mario", "", w.t, o); });
    uint32_t id_c = w.wait(2, req).id;
    CHECK(id_b && id_c && id_b != id_a && id_c != id_b);
    // Joining from another room or version fails as if the game did not exist.
    req = w.call(3, [&](Client& cl, std::vector<Bytes>& o) { return cl.join(inst, w.t, o); });
    CHECK(w.wait(3, req).hr == kErrNoSessions);
    w.run(0.5);
    CHECK(count_events(a, kSysCreatePlayer, id_b) == 1 && count_events(a, kSysCreatePlayer, id_c) == 1);
    CHECK(count_events(b, kSysCreatePlayer, id_c) == 1 && count_events(b, kSysCreatePlayer, id_a) == 0);
    for (auto& e : a.events())
        if (e.system && e.id == id_c) CHECK(e.players == 3 && e.short_name == "Mario");
    for (auto* cl : {&a, &b, &c}) cl->events().clear();

    // Messages: to everyone (not back to the sender), and to one player.
    Bytes race(684, 0x5a), menu = {0x05, 0, 0, 0x81, 1, 2, 3};
    for (int i = 0; i < 20; ++i)
        w.call(0, [&](Client& cl, std::vector<Bytes>& o) { cl.send(0, false, race.data(), race.size(), w.t, o); return 0u; });
    w.call(0, [&](Client& cl, std::vector<Bytes>& o) { cl.send(id_b, true, menu.data(), menu.size(), w.t, o); return 0u; });
    w.call(2, [&](Client& cl, std::vector<Bytes>& o) { cl.send(0, true, menu.data(), menu.size(), w.t, o); return 0u; });
    w.run(1);
    auto eb = data_events(b), ec = data_events(c), ea = data_events(a);
    size_t race_b = 0, menu_b = 0, race_c = 0, menu_c = 0;
    for (auto& e : eb) {
        if (e.data == race) { ++race_b; CHECK(e.from == id_a && e.to == id_b); }
        if (e.data == menu) ++menu_b;
    }
    for (auto& e : ec) {
        if (e.data == race) { ++race_c; CHECK(e.to == id_c); }
        if (e.data == menu) ++menu_c;
    }
    CHECK(menu_b == 2 && menu_c == 0);   // Xavi's to Marcos only + Mario's to everyone
    CHECK(ea.size() == 1 && ea[0].data == menu && ea[0].from == id_c && ea[0].to == id_a);
    if (link.loss == 0) CHECK(race_b == 20 && race_c == 20);
    else CHECK(race_b > 5 && race_c > 5 && race_b <= 40);
    for (auto* cl : {&a, &b, &c}) cl->events().clear();

    // The host closes the game to new players (GP4 does it when the race starts).
    SessionDesc closed = d;
    closed.flags |= kSessionJoinDisabled;
    w.call(0, [&](Client& cl, std::vector<Bytes>& o) { cl.set_desc(closed, w.t, o); return 0u; });
    w.run(0.5);
    CHECK(count_events(b, kSysSetSessionDesc) == 1 && count_events(c, kSysSetSessionDesc) == 1);
    Client& late = w.add("Late");
    w.run(0.5);
    w.call(5, [&](Client& cl, std::vector<Bytes>& o) { cl.request_list(app, w.t, o); return 0u; });
    w.run(0.5);
    CHECK(late.sessions().size() == 1 && !joinable(late.sessions()[0]) && late.sessions()[0].name == "Xavi 3/22");
    req = w.call(5, [&](Client& cl, std::vector<Bytes>& o) { return cl.join(inst, w.t, o); });
    CHECK(w.wait(5, req).hr == kErrNoNewPlayers);
    // Only the host may change the description.
    SessionDesc hijack = d;
    hijack.max_players = 2;
    w.call(1, [&](Client& cl, std::vector<Bytes>& o) { cl.set_desc(hijack, w.t, o); return 0u; });
    w.run(0.5);
    CHECK(count_events(c, kSysSetSessionDesc) == 1);
    for (auto* cl : {&a, &b, &c}) cl->events().clear();

    // The host leaves: everyone is told, and the longest-standing player becomes the host.
    w.call(0, [&](Client& cl, std::vector<Bytes>& o) { cl.leave(w.t, o); return 0u; });
    w.run(0.5);
    CHECK(count_events(b, kSysDestroyPlayer, id_a) == 1 && count_events(c, kSysDestroyPlayer, id_a) == 1);
    CHECK(count_events(b, kSysHost) == 1 && count_events(c, kSysHost) == 0);
    w.call(1, [&](Client& cl, std::vector<Bytes>& o) { cl.request_list(app, w.t, o); return 0u; });
    w.run(0.5);
    CHECK(b.sessions().size() == 1 && b.sessions()[0].name == "Marcos 2/22");
    CHECK(a.events().empty());
    for (auto* cl : {&a, &b, &c}) cl->events().clear();

    // Mario's PC vanishes: the server notices and tells Marcos.
    w.kill(2);
    w.run(17);
    CHECK(count_events(b, kSysDestroyPlayer, id_c) == 1);
    CHECK(w.server.clients() == 5);   // Xavi, Marcos, Pepe, Old, Late
    // Marcos quits GP4: the game is gone.
    w.call(1, [&](Client& cl, std::vector<Bytes>& o) { cl.bye(w.t, o); return 0u; });
    w.run(1);
    CHECK(w.server.sessions() == 0);
    CHECK(w.server.clients() == 4);
    bool logged_host = false;
    for (auto& l : w.server.take_log()) logged_host = logged_host || (l.find("'Marcos' (") == 0 && l.find("is now the host") != std::string::npos);
    CHECK(logged_host);
}

static void test_server() {
    scenario({0.020, 0.0, 0.0, 0.0}, "clean");
    scenario({0.040, 0.030, 0.10, 0.03}, "10% loss, jitter, duplicates");

    // Long names never overflow GP4's 32-byte session name slot.
    {
        World w;
        Client& a = w.add("A");
        Client& b = w.add("B");
        w.run(0.5);
        SessionDesc d;
        d.max_players = 22;
        d.name = std::string(80, 'n');
        uint32_t req = w.call(0, [&](Client& cl, std::vector<Bytes>& o) { return cl.create(d, w.t, o); });
        CHECK(w.wait(0, req).hr == kOk);
        w.call(1, [&](Client& cl, std::vector<Bytes>& o) { cl.request_list(Guid{}, w.t, o); return 0u; });
        w.run(0.3);
        CHECK(b.sessions().size() == 1 && b.sessions()[0].name.size() <= kMaxSessionName);   // no player yet
        req = w.call(0, [&](Client& cl, std::vector<Bytes>& o) { return cl.create_player(std::string(60, 'x'), "", w.t, o); });
        CHECK(w.wait(0, req).hr == kOk);
        w.call(1, [&](Client& cl, std::vector<Bytes>& o) { cl.request_list(Guid{}, w.t, o); return 0u; });
        w.run(0.3);
        CHECK(b.sessions().size() == 1 && b.sessions()[0].name.size() <= kMaxSessionName &&
              b.sessions()[0].name.find(" 1/22") != std::string::npos);
        (void)a;
    }
    // A player of another protocol is refused with a reason.
    {
        World w;
        Endpoint ep{0x01020304, 4000};
        Channel ch(0x77);
        Writer hello;
        hello.u8(kHello).u32(kProtocol + 1).str("9.9").str("").str("Future");
        std::vector<Bytes> out, in, back;
        ch.send(hello.buf, true, 0, out);
        std::vector<Server::Out> so;
        for (auto& d : out) w.server.receive(ep, d.data(), d.size(), 0, so);
        bool refused = false;
        for (auto& o : so) {
            in.clear();
            ch.receive(o.data.data(), o.data.size(), 0.01, in, back);
            for (auto& m : in) refused = refused || (m.size() > 1 && m[0] == kRefused);
        }
        CHECK(refused);
        // And a stranger's datagram that is not a hello creates nothing.
        Channel stranger(0x99);
        out.clear();
        stranger.send(Bytes{1, 2, 3}, false, 0, out);
        so.clear();
        w.server.receive(Endpoint{5, 5}, out[0].data(), out[0].size(), 0, so);
        CHECK(so.empty());
    }
    // No server: the player gives up after the connect timeout.
    {
        World w;
        Client& a = w.add("Alone");
        w.net.down[Net::key(w.server_ep)] = true;
        w.run(Client::kConnectTimeout + 1);
        CHECK(a.state() == Client::State::Lost && !a.problem().empty());
    }
    // The server goes away during a game: GP4 gets DPSYS_SESSIONLOST.
    {
        World w;
        Client& a = w.add("A");
        w.run(0.5);
        SessionDesc d;
        d.max_players = 22;
        uint32_t req = w.call(0, [&](Client& cl, std::vector<Bytes>& o) { return cl.create(d, w.t, o); });
        CHECK(w.wait(0, req).hr == kOk);
        w.net.down[Net::key(w.server_ep)] = true;
        w.run(Client::kSilence + 1);
        CHECK(a.state() == Client::State::Lost && count_events(a, kSysSessionLost) == 1 && !a.in_session());
    }
    // A player whose NAT changes its port keeps its game.
    {
        World w;
        Client& a = w.add("A");
        Client& b = w.add("B");
        w.run(0.5);
        SessionDesc d;
        d.max_players = 22;
        uint32_t req = w.call(0, [&](Client& cl, std::vector<Bytes>& o) { return cl.create(d, w.t, o); });
        Guid inst = w.wait(0, req).desc.instance;
        w.wait(0, w.call(0, [&](Client& cl, std::vector<Bytes>& o) { return cl.create_player("A", "", w.t, o); }));
        CHECK(w.wait(1, w.call(1, [&](Client& cl, std::vector<Bytes>& o) { return cl.join(inst, w.t, o); })).hr == kOk);
        w.wait(1, w.call(1, [&](Client& cl, std::vector<Bytes>& o) { return cl.create_player("B", "", w.t, o); }));
        w.players[1].ep.port = 61000;
        Bytes m = {9, 9, 9};
        w.call(0, [&](Client& cl, std::vector<Bytes>& o) { cl.send(0, true, m.data(), m.size(), w.t, o); return 0u; });
        w.run(0.3);   // B's keep-alive or ack from the new port moves it
        w.call(0, [&](Client& cl, std::vector<Bytes>& o) { cl.send(0, true, m.data(), m.size(), w.t, o); return 0u; });
        w.run(2);
        CHECK(data_events(b).size() == 2);
        (void)a;
    }
}

int main() {
    test_wire();
    test_channel();
    test_server();
    std::printf("[online] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
