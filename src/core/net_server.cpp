#include "core/net_server.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gp4u {
namespace net {

std::string Endpoint::str() const {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
    return b;
}

namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

std::string quoted(const std::string& s) { return "'" + s + "'"; }

}  // namespace

std::string Server::who(const Client& c) const {
    return c.name.empty() ? c.ep.str() : quoted(c.name) + " (" + c.ep.str() + ")";
}

Server::Session* Server::session_of(const Client& c) {
    auto it = sessions_.find(c.session);
    return it == sessions_.end() ? nullptr : &it->second;
}

// The list shows the host's name and the player count: every GP4 game has the same name.
SessionDesc Server::shown(const Session& s) const {
    SessionDesc d = s.desc;
    auto h = clients_.find(s.host);
    if (h != clients_.end() && !h->second.players.empty()) {
        char n[16];
        std::snprintf(n, sizeof n, " %u/%u", d.players, d.max_players);
        std::string host = h->second.players.front().short_name;
        if (host.empty()) host = h->second.name;
        host = host.substr(0, kMaxSessionName - std::strlen(n));
        d.name = host + n;
    }
    if (d.name.size() > kMaxSessionName) d.name.resize(kMaxSessionName);
    return d;
}

std::vector<SessionDesc> Server::list(const std::string& version, const std::string& room) const {
    std::vector<SessionDesc> v;
    for (auto& s : sessions_)
        if (s.second.version == version && s.second.room == room) v.push_back(shown(s.second));
    return v;
}

size_t Server::clients() const {
    size_t n = 0;
    for (auto& c : clients_) n += c.second.welcomed;
    return n;
}

std::vector<std::string> Server::take_log() {
    std::vector<std::string> v;
    v.swap(log_);
    return v;
}

void Server::send(Client& c, const Writer& w, bool reliable, double now, std::vector<Out>& out) {
    std::vector<Bytes> dg;
    c.ch.send(w.buf, reliable, now, dg);
    for (auto& b : dg) out.push_back({c.ep, std::move(b)});
}

void Server::result(Client& c, uint32_t req, uint32_t hr, double now, std::vector<Out>& out, const SessionDesc* d,
                    uint32_t id) {
    Writer w;
    w.u8(kResult).u32(req).u32(hr);
    if (d) write_desc(w, *d);
    if (id) w.u32(id);
    send(c, w, true, now, out);
}

void Server::receive(const Endpoint& from, const uint8_t* d, size_t n, double now, std::vector<Out>& out) {
    uint32_t token;
    uint8_t kind;
    if (!peek_header(d, n, token, kind) || token == 0) return;
    auto it = clients_.find(token);
    if (it == clients_.end()) {
        // A new player starts with its hello, sent reliably.
        if (kind != kReliable || clients_.size() >= cfg_.max_clients) return;
        it = clients_.emplace(token, Client{}).first;
        it->second.token = token;
        it->second.ch = Channel(token);
        it->second.ep = from;
    }
    Client& c = it->second;
    if (c.ep != from) {   // the player's NAT gave it a new port (or address)
        if (c.welcomed) log(who(c) + " now at " + from.str());
        c.ep = from;
    }
    std::vector<Bytes> in, dg;
    c.ch.receive(d, n, now, in, dg);
    for (auto& b : dg) out.push_back({c.ep, std::move(b)});
    for (auto& m : in) {
        if (clients_.find(token) == clients_.end()) break;   // removed by kBye
        handle(c, m, now, out);
    }
}

void Server::handle(Client& c, const Bytes& m, double now, std::vector<Out>& out) {
    if (m.empty()) return;
    Reader r(m);
    uint8_t type = r.u8();
    if (!c.welcomed && type != kHello) return;
    switch (type) {
    case kHello: {
        uint32_t proto = r.u32();
        c.version = r.str();
        c.room = r.str();
        c.name = r.str().substr(0, 64);
        if (!r.ok() || proto != kProtocol) {
            Writer w;
            w.u8(kRefused).str("this server speaks protocol " + std::to_string(kProtocol) +
                               ", your GP4 Ultimate speaks " + std::to_string(proto) + ": use the same version");
            send(c, w, true, now, out);
            log(c.ep.str() + " refused: protocol " + std::to_string(proto));
            return;
        }
        if (!c.welcomed) log(who(c) + " connected, GP4 Ultimate " + c.version + (c.room.empty() ? "" : ", room " + quoted(c.room)));
        c.welcomed = true;
        Writer w;
        w.u8(kWelcome).u32(kProtocol).str(cfg_.version).str(c.ep.str());
        send(c, w, true, now, out);
        break;
    }
    case kList: {
        Guid app = r.guid();
        std::vector<SessionDesc> v;
        for (auto& s : sessions_)
            if (s.second.version == c.version && s.second.room == c.room && (app.null() || s.second.desc.application == app))
                v.push_back(shown(s.second));
        if (v.size() > 64) v.resize(64);
        Writer w;
        w.u8(kSessions).u16((uint16_t)v.size());
        for (auto& d : v) write_desc(w, d);
        send(c, w, true, now, out);
        break;
    }
    case kCreate: {
        uint32_t req = r.u32();
        SessionDesc d = read_desc(r);
        if (!r.ok()) return;
        if (c.session) leave(c, now, out, "left");
        if (sessions_.size() >= cfg_.max_sessions) {
            result(c, req, kErrCantCreateSession, now, out);
            log(who(c) + " could not create a game: server full");
            return;
        }
        Session s;
        s.id = next_session_++;
        if (guid_state_ == 0x9e3779b97f4a7c15ull) guid_state_ ^= (uint64_t)(now * 1e6) ^ ((uint64_t)c.token << 20);
        for (int i = 0; i < 2; ++i) {
            uint64_t v = splitmix(guid_state_);
            std::memcpy(d.instance.b + 8 * i, &v, 8);
        }
        d.max_players = std::min(std::max<uint32_t>(d.max_players, 1), cfg_.max_players);
        d.players = 0;
        d.name = d.name.substr(0, 64);
        s.desc = d;
        s.version = c.version;
        s.room = c.room;
        s.host = c.token;
        s.members.push_back(c.token);
        c.session = s.id;
        sessions_[s.id] = s;
        log(who(c) + " created a game (" + std::to_string(d.max_players) + " players)");
        result(c, req, kOk, now, out, &d);
        break;
    }
    case kJoin: {
        uint32_t req = r.u32();
        Guid inst = r.guid();
        if (!r.ok()) return;
        Session* found = nullptr;
        for (auto& s : sessions_)
            if (s.second.desc.instance == inst && s.second.version == c.version && s.second.room == c.room) found = &s.second;
        if (!found) {
            result(c, req, kErrNoSessions, now, out);
            return;
        }
        if (found->id == c.session) {   // already in it
            result(c, req, kOk, now, out, &found->desc);
            return;
        }
        if (!joinable(found->desc)) {
            result(c, req, kErrNoNewPlayers, now, out);
            log(who(c) + " could not join a full or closed game");
            return;
        }
        uint32_t id = found->id;
        if (c.session) leave(c, now, out, "left");
        Session& s = sessions_.at(id);
        s.members.push_back(c.token);
        c.session = s.id;
        auto host = clients_.find(s.host);
        log(who(c) + " joined the game of " + (host != clients_.end() ? quoted(host->second.name) : "?"));
        result(c, req, kOk, now, out, &s.desc);
        break;
    }
    case kCreatePlayer: {
        uint32_t req = r.u32();
        Player p;
        p.short_name = r.str().substr(0, 64);
        p.long_name = r.str().substr(0, 64);
        Session* s = session_of(c);
        if (!r.ok() || !s) {
            result(c, req, kErrInvalidParams, now, out);
            return;
        }
        if (s->desc.players >= s->desc.max_players) {
            result(c, req, kErrCantAddPlayer, now, out);
            return;
        }
        p.id = next_id_++;
        if (next_id_ < cfg_.first_id) next_id_ = cfg_.first_id;
        if (!p.short_name.empty() && c.name.empty()) c.name = p.short_name;   // GP4's driver name, for the log
        c.players.push_back(p);
        ++s->desc.players;
        result(c, req, kOk, now, out, nullptr, p.id);
        for (uint32_t t : s->members) {
            if (t == c.token) continue;
            Writer w;
            w.u8(kSystem).u32(kSysCreatePlayer).u32(p.id).u32(s->desc.players).str(p.short_name).str(p.long_name);
            send(clients_.at(t), w, true, now, out);
        }
        break;
    }
    case kSend: {
        uint32_t to = r.u32(), flags = r.u32();
        size_t n = r.left();
        Session* s = session_of(c);
        if (!r.ok() || !s || c.players.empty() || n > kMaxMessage) return;
        bool reliable = (flags & kSendGuaranteed) != 0;
        for (uint32_t t : s->members) {
            if (t == c.token) continue;
            Client& dst = clients_.at(t);
            for (auto& p : dst.players) {
                if (to != 0 && p.id != to) continue;
                Writer w;
                w.u8(kData).u32(c.players.front().id).u32(p.id).raw(r.here(), n);
                send(dst, w, reliable, now, out);
                break;   // one copy per PC (GP4 has one player per PC)
            }
        }
        break;
    }
    case kSetDesc: {
        SessionDesc d = read_desc(r);
        Session* s = session_of(c);
        if (!r.ok() || !s || s->host != c.token) return;
        s->desc.flags = d.flags;
        s->desc.max_players = std::min(std::max(d.max_players, s->desc.players), cfg_.max_players);
        s->desc.name = d.name.substr(0, 64);
        std::memcpy(s->desc.user, d.user, sizeof d.user);
        for (uint32_t t : s->members) {
            if (t == c.token) continue;
            Writer w;
            w.u8(kSystem).u32(kSysSetSessionDesc);
            write_desc(w, s->desc);
            send(clients_.at(t), w, true, now, out);
        }
        break;
    }
    case kLeave:
        if (c.session) leave(c, now, out, "left");
        break;
    case kBye: {
        uint32_t token = c.token;
        if (c.session) leave(c, now, out, "left");
        log(who(c) + " disconnected");
        std::vector<Bytes> dg;
        c.ch.flush_ack(dg);   // or it would keep resending its goodbye
        for (auto& b : dg) out.push_back({c.ep, std::move(b)});
        clients_.erase(token);
        break;
    }
    default:
        break;
    }
}

void Server::leave(Client& c, double now, std::vector<Out>& out, const char* why) {
    Session* sp = session_of(c);
    std::vector<Player> gone;
    gone.swap(c.players);
    c.session = 0;
    if (!sp) return;
    Session& s = *sp;
    s.members.erase(std::remove(s.members.begin(), s.members.end(), c.token), s.members.end());
    s.desc.players -= std::min<uint32_t>(s.desc.players, (uint32_t)gone.size());
    log(who(c) + " " + why + " the game");
    if (s.members.empty()) {
        log("game closed (no players left)");
        sessions_.erase(s.id);
        return;
    }
    for (uint32_t t : s.members)
        for (auto& p : gone) {
            Writer w;
            w.u8(kSystem).u32(kSysDestroyPlayer).u32(p.id).str(p.short_name).str(p.long_name);
            send(clients_.at(t), w, true, now, out);
        }
    if (s.host == c.token) {
        // DirectPlay's host migration (GP4 opens its games with DPSESSION_MIGRATEHOST):
        // the longest-standing player becomes the host and is told so.
        s.host = s.members.front();
        Client& h = clients_.at(s.host);
        Writer w;
        w.u8(kSystem).u32(kSysHost);
        send(h, w, true, now, out);
        log(who(h) + " is now the host");
    }
}

void Server::poll(double now, std::vector<Out>& out) {
    std::vector<uint32_t> dead;
    for (auto& it : clients_) {
        Client& c = it.second;
        if (now - c.ch.last_heard() > (c.welcomed ? cfg_.timeout : cfg_.hello_timeout) || c.ch.failed(now)) {
            dead.push_back(it.first);
            continue;
        }
        std::vector<Bytes> dg;
        c.ch.poll(now, dg);
        for (auto& b : dg) out.push_back({c.ep, std::move(b)});
    }
    for (uint32_t t : dead) {
        auto it = clients_.find(t);
        if (it == clients_.end()) continue;
        Client& c = it->second;
        if (c.session) leave(c, now, out, "lost connection, left");
        if (c.welcomed) log(who(c) + " timed out");
        clients_.erase(t);
    }
}

}  // namespace net
}  // namespace gp4u
